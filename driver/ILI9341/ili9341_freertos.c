/*
 * ============================================================================
 * @file    ili9341_freertos.c
 * @brief   ILI9341 FreeRTOS 串行化、服务任务和 DMA 等待层实现
 * ============================================================================
 *
 * 一、总设计
 * ---------------------------------------------------------------------------
 * 本文件把“RTOS 并发问题”封装在 Core/GFX 之外。
 *
 *      Application Task A ----+
 *                             |
 *      Application Task B ----+----> Job Queue ----> LCD Service Task
 *                             |                         |
 *      Application Task C ----+                         v
 *                                              Recursive Mutex
 *                                                     |
 *                                                     v
 *                                                Core / GFX
 *
 * DMA 路径：
 *
 *      Service/Caller Task
 *           |
 *           v
 *      Core DMAStart
 *           |
 *           v
 *      DMA IRQ -> Port -> Core Callback
 *                         |
 *                         v
 *              prvCoreDMACallbackFromISR()
 *                         |
 *                         v
 *        vTaskNotifyGiveIndexedFromISR()
 *                         |
 *                         v
 *                  等待任务被唤醒
 *                         |
 *                         v
 *                  ILI9341_DMAFinalize()
 *
 * 二、为什么使用 Recursive Mutex
 * ---------------------------------------------------------------------------
 * 服务任务在执行一个 Job 前会先取得 LCD Mutex。
 * 某些 Job 内部还可能调用本层的其他“带锁”接口。
 * 递归互斥锁允许同一个任务重复 Lock，并按次数 Unlock，避免自锁死锁。
 *
 * 三、为什么 DMA IRQ 中不 Finalize
 * ---------------------------------------------------------------------------
 * Port/Core Callback 运行在 DMA ISR 上下文。
 * Finalize 需要等待 SPI TXE / BSY，因此不应放在 ISR 中阻塞。
 * ISR 只做事件记录 + FromISR Notification，真正 Finalize 回到任务上下文完成。
 * ============================================================================
 */

/* 当前层公开接口。 */
#include "ili9341_freertos.h"

/* FreeRTOS Queue API。 */
#include "queue.h"

/* FreeRTOS Semaphore / Mutex API。 */
#include "semphr.h"

/* NULL 和 size_t。 */
#include <stddef.h>

/* ============================================================================
 * 1. 内部 Job 请求对象
 * ============================================================================
 *
 * 注意：该对象由 ILI9341_RTOS_Execute() 创建在“调用者任务栈”上。
 * Queue 中只保存它的指针。
 *
 * 安全性的关键：
 *      一旦请求成功入队，Execute() 就一直等待到服务任务处理完成，
 *      因此 request 的栈内存不会提前失效。
 */
typedef struct
{
    /* 服务任务真正要调用的函数地址。 */
    ILI9341_RTOS_JobFunction_t function;

    /* 原样转交给 function() 的用户上下文。 */
    void *user_context;

    /* 提交 Job 的任务句柄，服务任务完成后通过通知唤醒它。 */
    TaskHandle_t caller_task;

    /* 服务任务把 function() 的实际 Core/GFX 返回值写在这里。 */
    volatile ILI9341_Status_t result;

    /*
     * 0：普通 Job。
     * 1：DeInit 发送的服务任务停止哨兵。
     */
    uint8_t stop_service;

} ILI9341_RTOS_JobRequest_t;

/* ============================================================================
 * 2. RTOS 层静态软件状态
 * ============================================================================
 */

/* 保护所有直接 Core/GFX 访问的递归 Mutex。 */
static SemaphoreHandle_t s_lcd_mutex = NULL;

/*
 * 保护“检查是否允许提交 + Queue Send”这一小段临界流程。
 * 它与 LCD Mutex 分离，避免单纯提交 Job 必须长期占用 LCD 访问锁。
 */
static SemaphoreHandle_t s_submit_mutex = NULL;

/* 保存 JobRequest 指针的 Queue。 */
static QueueHandle_t s_job_queue = NULL;

/* LCD Service Task 句柄。 */
static TaskHandle_t s_service_task = NULL;

/* 只有当 Queue / Mutex / Core / Service Task 全部成功后才置 1。 */
static volatile uint8_t s_initialized = 0U;

/*
 * 控制是否还接受新的上层访问。
 * DeInit 首先把它清零，阻止新 Job/Lock 进入，但允许已在执行的 Job 收尾。
 */
static volatile uint8_t s_accept_jobs = 0U;

/*
 * 当前正在等待 DMA IRQ 的任务。
 * 一次只允许一笔 Core DMA pending，因此这里只需要保存一个等待者。
 */
static volatile TaskHandle_t s_dma_wait_task = NULL;

/* ISR 写入：最近一次 Core DMA Event 是否已经到达。 */
static volatile uint8_t s_dma_event_valid = 0U;

/* ISR 写入：最近一次 DMA Event 是 COMPLETE 还是 ERROR。 */
static volatile ILI9341_DMAEvent_t s_dma_event = ILI9341_DMA_EVENT_COMPLETE;

/* ============================================================================
 * 3. 私有声明
 * ============================================================================
 */

/* LCD 服务任务主循环。 */
static void prvServiceTask(void *argument);

/* Core DMA Callback；运行在 DMA ISR 调用链中。 */
static void prvCoreDMACallbackFromISR(
    ILI9341_DMAEvent_t event,
    void *user_context
);

/* 清掉指定 Notification Index 上可能残留的旧计数。 */
static void prvDrainNotification(UBaseType_t index);

/* 执行一笔 DMA，并在任务上下文等待 IRQ + Finalize。 */
static ILI9341_Status_t prvDMAChunkWait(
    const uint8_t *data,
    uint32_t length,
    TickType_t dma_wait_ticks
);

/* 销毁当前层创建的 FreeRTOS 对象；仅内部错误回滚使用。 */
static void prvDeleteRTOSObjects(void);

/* ============================================================================
 * 4. 私有工具：清空旧 Task Notification
 * ============================================================================
 */
static void prvDrainNotification(UBaseType_t index)
{
    /*
     * ulTaskNotifyTakeIndexed(..., pdTRUE, 0) 的含义：
     *      如果当前 index 已有通知计数，就一次性清零并返回旧计数；
     *      如果没有通知，立即返回 0，不阻塞。
     *
     * while 的目的不是等待，而是确保进入新事务前没有旧通知残留。
     */
    while (ulTaskNotifyTakeIndexed(index, pdTRUE, 0U) != 0UL)
    {
        /* 当前值已经被 pdTRUE 清零，正常情况下循环最多执行一次。 */
    }
}

/* ============================================================================
 * 5. Core DMA Callback -> FreeRTOS Task Notification
 * ============================================================================
 */
static void prvCoreDMACallbackFromISR(
    ILI9341_DMAEvent_t event,
    void *user_context
)
{
    /* FreeRTOS FromISR API 用于判断退出 ISR 前是否需要立即切换任务。 */
    BaseType_t higher_priority_task_woken = pdFALSE;

    /* volatile 全局先复制到局部变量，避免后续表达式重复读取。 */
    TaskHandle_t waiter;

    /* 当前版本没有使用 Core Callback 自带的 context。 */
    (void)user_context;

    /* 保存事件类型，等待任务醒来后可以知道 DMA 是成功还是错误。 */
    s_dma_event = event;

    /* 先写事件内容，再把 valid 置 1，表达“完整事件已经可读”。 */
    s_dma_event_valid = 1U;

    /* 读取当前唯一 DMA 等待任务。 */
    waiter = (TaskHandle_t)s_dma_wait_task;

    /* 如果确实有任务正在等待本次 DMA，就从 ISR 发出通知。 */
    if (waiter != NULL)
    {
        /*
         * 使用 Notification Index 2，避免占用应用通常使用的 Index 0。
         * 这里只增加通知计数，不执行任何阻塞动作。
         */
        vTaskNotifyGiveIndexedFromISR(
            waiter,
            (UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_DMA,
            &higher_priority_task_woken
        );

        /*
         * 如果被唤醒任务优先级高于当前被中断任务，
         * portYIELD_FROM_ISR() 会请求在 ISR 退出时进行调度切换。
         */
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

/* ============================================================================
 * 6. 私有工具：等待一笔 DMA 真正完成
 * ============================================================================
 */
static ILI9341_Status_t prvDMAChunkWait(
    const uint8_t *data,
    uint32_t length,
    TickType_t dma_wait_ticks
)
{
    /* 当前执行 DMA 的任务句柄。 */
    TaskHandle_t current_task;

    /* Core API 返回值。 */
    ILI9341_Status_t status;

    /* 等待通知返回的计数；0 代表超时。 */
    uint32_t notification_count;

    /* 参数必须有效。 */
    if ((data == NULL) || (length == 0UL))
    {
        return ILI9341_ERROR_PARAM;
    }

    /* DMA 等待必须发生在 FreeRTOS Task 上下文。 */
    current_task = xTaskGetCurrentTaskHandle();

    /* 正常运行的 Task 必须能取得自己的句柄。 */
    if (current_task == NULL)
    {
        return ILI9341_ERROR_STATE;
    }

    /* 清除该任务 DMA Notification Index 上的历史通知。 */
    prvDrainNotification((UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_DMA);

    /* 新事务开始前，先标记当前还没有收到 DMA Event。 */
    s_dma_event_valid = 0U;

    /* 先登记等待任务，再启动 DMA，防止 DMA 很快完成后找不到等待者。 */
    s_dma_wait_task = current_task;

    /* 启动 Core DMA；此时只代表 DMA 已经开始，不代表真正完成。 */
    status = ILI9341_WriteBytesDMAStart(data, length);

    /* DMA 根本没启动成功，就撤销 waiter 并把原始 Core 错误返回。 */
    if (status != ILI9341_OK)
    {
        s_dma_wait_task = NULL;
        s_dma_event_valid = 0U;
        return status;
    }

    /*
     * 当前任务阻塞等待 DMA ISR 通知。
     * pdTRUE 表示取到通知后把该 Index 的计数清零。
     */
    notification_count =
        ulTaskNotifyTakeIndexed(
            (UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_DMA,
            pdTRUE,
            dma_wait_ticks
        );

    /* 没等到 ISR 通知，说明本笔 DMA 超时。 */
    if (notification_count == 0UL)
    {
        /* 不再允许晚到的 ISR 去通知已经退出等待路径的任务。 */
        s_dma_wait_task = NULL;

        /* 事件标志清零，避免把晚到旧事件误认为下一笔 DMA 的事件。 */
        s_dma_event_valid = 0U;

        /*
         * 这里返回 Core 已有的 TIMEOUT 状态。
         * 上一级会 AbortWrite()，由 Port/Core 尝试停止 DMA、释放 CS。
         */
        return ILI9341_ERROR_TIMEOUT;
    }

    /* 已经收到 ISR 通知，当前 waiter 的职责完成。 */
    s_dma_wait_task = NULL;

    /*
     * 理论上通知来自 Core DMA Callback，因此 valid 应该已经是 1。
     * 如果不是，说明通知状态与 DMA 事件状态不一致，按状态错误处理。
     */
    if (s_dma_event_valid == 0U)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * 无论 Event 是 COMPLETE 还是 ERROR，都调用 Finalize：
     *      COMPLETE -> 等 TXE / BSY，正式把 pending 转 committed；
     *      ERROR    -> 让 Port/Core 完成 DMA 错误清理。
     */
    status = ILI9341_DMAFinalize();

    /* 当前 Event 已经消费完毕。 */
    s_dma_event_valid = 0U;

    /*
     * 如果 ISR 明确报告 ERROR，而某种异常情况下 Finalize 却返回 OK，
     * 仍以 DMA Error 为准，避免把已知错误吞掉。
     */
    if ((s_dma_event == ILI9341_DMA_EVENT_ERROR) &&
        (status == ILI9341_OK))
    {
        return ILI9341_ERROR_DMA;
    }

    /* 正常把 Finalize 的 Core 状态原样返回。 */
    return status;
}

/* ============================================================================
 * 7. LCD Service Task
 * ============================================================================
 */
static void prvServiceTask(void *argument)
{
    /* Queue 中保存的是 JobRequest 指针。 */
    ILI9341_RTOS_JobRequest_t *request;

    /* Job 最终返回值。 */
    ILI9341_Status_t result;

    /* 当前版本不需要 task parameter。 */
    (void)argument;

    /* 服务任务在整个生命周期内持续等待 Job。 */
    for (;;)
    {
        /* 初始化为空，避免编译器或静态分析误判未初始化使用。 */
        request = NULL;

        /*
         * portMAX_DELAY：没有 Job 时服务任务睡眠，不占 CPU。
         * 当任意任务 xQueueSend() 后，服务任务被唤醒。
         */
        if (xQueueReceive(
                s_job_queue,
                &request,
                portMAX_DELAY
            ) != pdPASS)
        {
            /* 理论上无限等待不应失败；如果失败就继续下一轮。 */
            continue;
        }

        /* 防御性检查：Queue 中不应该存在 NULL request。 */
        if (request == NULL)
        {
            continue;
        }

        /*
         * DeInit 会把一个 stop_service=1 的特殊请求放到 Queue 尾部。
         * FIFO 保证它前面的所有普通 Job 都已经处理完以后才会走到这里。
         */
        if (request->stop_service != 0U)
        {
            /* 告诉 DeInit：服务任务已经消费到停止哨兵。 */
            request->result = ILI9341_OK;

            /*
             * 先把全局句柄清掉，再通知 DeInit。
             * 此后本任务不再访问 Queue/Mutex，只会立即自删除。
             */
            s_service_task = NULL;

            if (request->caller_task != NULL)
            {
                (void)xTaskNotifyGiveIndexed(
                    request->caller_task,
                    (UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_JOB
                );
            }

            /* 服务任务生命周期到此结束。 */
            vTaskDelete(NULL);

            /* vTaskDelete(NULL) 正常不会返回；保留 return 便于静态分析。 */
            return;
        }

        /* 普通 Job 必须有可执行函数。 */
        if (request->function == NULL)
        {
            continue;
        }

        /*
         * 取得统一 LCD Mutex。
         * 这样 Service Task 与“直接 Lock 后访问 Core/GFX”的任务不会交叉。
         */
        if (xSemaphoreTakeRecursive(
                s_lcd_mutex,
                portMAX_DELAY
            ) != pdTRUE)
        {
            /* 无限等待理论上不会失败；按硬件/RTOS异常反馈。 */
            result = ILI9341_ERROR_HW;
        }
        else
        {
            /* DeInit 过程中可能已经关闭服务入口。 */
            if (s_initialized == 0U)
            {
                result = ILI9341_ERROR_NOT_INITIALIZED;
            }
            else
            {
                /* 真正执行用户 Job；Job 可以调用任意 Core/GFX API。 */
                result = request->function(request->user_context);
            }

            /* 与 TakeRecursive 成对释放 Mutex。 */
            (void)xSemaphoreGiveRecursive(s_lcd_mutex);
        }

        /* 把实际 Job 结果写回调用者仍然存活的栈对象。 */
        request->result = result;

        /*
         * 通知提交 Job 的任务：服务任务已经完成。
         * 这里是普通 Task 上下文，因此使用 xTaskNotifyGiveIndexed()。
         */
        if (request->caller_task != NULL)
        {
            (void)xTaskNotifyGiveIndexed(
                request->caller_task,
                (UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_JOB
            );
        }
    }
}

/* ============================================================================
 * 8. 私有工具：删除 FreeRTOS 对象
 * ============================================================================
 */
static void prvDeleteRTOSObjects(void)
{
    /* 如果服务任务已经创建，先删除它。 */
    if (s_service_task != NULL)
    {
        vTaskDelete(s_service_task);
        s_service_task = NULL;
    }

    /* 删除 Queue；FreeRTOS 会释放动态分配内存。 */
    if (s_job_queue != NULL)
    {
        vQueueDelete(s_job_queue);
        s_job_queue = NULL;
    }

    /* 删除递归 LCD Mutex。 */
    if (s_lcd_mutex != NULL)
    {
        vSemaphoreDelete(s_lcd_mutex);
        s_lcd_mutex = NULL;
    }

    /* 删除 Job 提交 Mutex。 */
    if (s_submit_mutex != NULL)
    {
        vSemaphoreDelete(s_submit_mutex);
        s_submit_mutex = NULL;
    }

    /* 清除 DMA 等待桥状态。 */
    s_dma_wait_task = NULL;
    s_dma_event_valid = 0U;

    /* RTOS 对象不存在时当然也不接受新 Job。 */
    s_accept_jobs = 0U;
}

/* ============================================================================
 * 9. Public Init
 * ============================================================================
 */
ILI9341_Status_t ILI9341_RTOS_Init(void)
{
    /* Core 初始化返回值。 */
    ILI9341_Status_t status;

    /* xTaskCreate() 返回值。 */
    BaseType_t task_create_result;

    /* 已经初始化时设计成幂等成功。 */
    if (s_initialized != 0U)
    {
        return ILI9341_OK;
    }

    /*
     * 递归 Mutex 使用动态创建，因为当前工程：
     *      configSUPPORT_DYNAMIC_ALLOCATION = 1
     *      configUSE_RECURSIVE_MUTEXES      = 1
     */
    s_lcd_mutex = xSemaphoreCreateRecursiveMutex();

    /* 分配失败。 */
    if (s_lcd_mutex == NULL)
    {
        prvDeleteRTOSObjects();
        return ILI9341_ERROR_HW;
    }

    /*
     * 普通 Mutex 专门保护 Job 提交与 DeInit 停止哨兵之间的先后关系。
     * 这样可以保证 stop request 入队之后，绝不会再有新 Job 排到它后面。
     */
    s_submit_mutex = xSemaphoreCreateMutex();

    if (s_submit_mutex == NULL)
    {
        prvDeleteRTOSObjects();
        return ILI9341_ERROR_HW;
    }

    /* Queue 每个元素只保存一个 JobRequest 指针。 */
    s_job_queue = xQueueCreate(
        (UBaseType_t)ILI9341_RTOS_QUEUE_LENGTH,
        (UBaseType_t)sizeof(ILI9341_RTOS_JobRequest_t *)
    );

    /* Queue 创建失败就回滚 Mutex。 */
    if (s_job_queue == NULL)
    {
        prvDeleteRTOSObjects();
        return ILI9341_ERROR_HW;
    }

    /*
     * 先初始化 Core/Port/LCD。
     * Core 自己会注册 Port -> Core 的 DMA Bridge。
     */
    status = ILI9341_Init();

    /* Core 初始化失败就销毁已创建的 RTOS 资源。 */
    if (status != ILI9341_OK)
    {
        prvDeleteRTOSObjects();
        return status;
    }

    /*
     * 再把“Core -> FreeRTOS”这一跳 Callback 注册进 Core。
     * 以后 DMA IRQ 会最终进入 prvCoreDMACallbackFromISR()。
     */
    ILI9341_SetDMACallback(
        prvCoreDMACallbackFromISR,
        NULL
    );

    /* 创建专用 LCD Service Task。 */
    task_create_result = xTaskCreate(
        prvServiceTask,
        "lcd_svc",
        (configSTACK_DEPTH_TYPE)ILI9341_RTOS_SERVICE_STACK_DEPTH,
        NULL,
        (UBaseType_t)ILI9341_RTOS_SERVICE_PRIORITY,
        &s_service_task
    );

    /* Task 创建失败，需要把 Core 和 Callback 一起回滚。 */
    if (task_create_result != pdPASS)
    {
        ILI9341_SetDMACallback(NULL, NULL);
        (void)ILI9341_DeInit();
        prvDeleteRTOSObjects();
        return ILI9341_ERROR_HW;
    }

    /* 所有组件都已经建立成功，最后才宣布初始化完成并开放新 Job。 */
    s_initialized = 1U;
    s_accept_jobs = 1U;

    return ILI9341_OK;
}

/* ============================================================================
 * 10. Public DeInit
 * ============================================================================
 */
ILI9341_Status_t ILI9341_RTOS_DeInit(void)
{
    /* Core DeInit 的最终返回值。 */
    ILI9341_Status_t status;

    /* 停止服务任务使用的栈上请求对象。 */
    ILI9341_RTOS_JobRequest_t stop_request;

    /* Queue 实际发送的是请求对象指针。 */
    ILI9341_RTOS_JobRequest_t *stop_pointer;

    /* 当前调用 DeInit 的任务。 */
    TaskHandle_t current_task;

    /* 等待服务任务停止确认的通知计数。 */
    uint32_t notification_count;

    /* 未初始化时幂等成功。 */
    if (s_initialized == 0U)
    {
        return ILI9341_OK;
    }

    /* DeInit 必须在普通任务上下文执行。 */
    current_task = xTaskGetCurrentTaskHandle();

    if (current_task == NULL)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * 不能从 LCD Service Task 自己内部 DeInit。
     * 否则当前 Job 调用栈和服务任务生命周期会互相销毁。
     */
    if (current_task == s_service_task)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * 先取得提交 Mutex，把“关闭新入口 + 放入 stop 哨兵”做成不可穿插的流程。
     * 这样其他任务不可能在 stop 哨兵之后再塞进一个普通 Job。
     */
    if (xSemaphoreTake(
            s_submit_mutex,
            portMAX_DELAY
        ) != pdTRUE)
    {
        return ILI9341_ERROR_HW;
    }

    /*
     * 只关闭“新入口”，但暂时保持 s_initialized=1。
     * 已经在执行/已经排队的 Job 仍然可以正常收尾。
     */
    s_accept_jobs = 0U;

    /* 清掉 DeInit 调用任务 Job Notification Index 上的历史通知。 */
    prvDrainNotification((UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_JOB);

    /* 构造特殊停止哨兵。 */
    stop_request.function = NULL;
    stop_request.user_context = NULL;
    stop_request.caller_task = current_task;
    stop_request.result = ILI9341_ERROR_STATE;
    stop_request.stop_service = 1U;

    /* Queue 保存请求指针。 */
    stop_pointer = &stop_request;

    /*
     * 把停止请求放到 FIFO Queue 尾部。
     * 它前面的所有已提交 Job 会先执行完，然后服务任务才自删除。
     */
    if (xQueueSend(
            s_job_queue,
            &stop_pointer,
            portMAX_DELAY
        ) != pdPASS)
    {
        /* 理论上无限等待不应失败；恢复入口并报告 RTOS 异常。 */
        s_accept_jobs = 1U;
        (void)xSemaphoreGive(s_submit_mutex);
        return ILI9341_ERROR_HW;
    }

    /* stop 已经成为 Queue 中最后一个合法请求，现在可以释放提交 Mutex。 */
    (void)xSemaphoreGive(s_submit_mutex);

    /* 等待 Service Task 消费停止哨兵。 */
    notification_count =
        ulTaskNotifyTakeIndexed(
            (UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_JOB,
            pdTRUE,
            portMAX_DELAY
        );

    if (notification_count == 0UL)
    {
        return ILI9341_ERROR_HW;
    }

    /*
     * 服务任务已经退出，不再有 Queue Job 会访问 LCD。
     * 再取得 Mutex，等待可能仍持锁的“直接访问任务”完全退出。
     */
    if (xSemaphoreTakeRecursive(
            s_lcd_mutex,
            portMAX_DELAY
        ) != pdTRUE)
    {
        return ILI9341_ERROR_HW;
    }

    /* 到这里已经不存在新的合法 LCD 访问入口。 */
    s_initialized = 0U;

    /* 断开 Core -> FreeRTOS DMA Callback。 */
    ILI9341_SetDMACallback(NULL, NULL);

    /* 正式关闭 Core / Port / LCD 生命周期。 */
    status = ILI9341_DeInit();

    /* 与上面的 TakeRecursive 成对。 */
    (void)xSemaphoreGiveRecursive(s_lcd_mutex);

    /* 删除 Queue、Mutex 等动态 RTOS 对象。 */
    prvDeleteRTOSObjects();

    /* 保持入口关闭。 */
    s_accept_jobs = 0U;

    return status;
}


/* ============================================================================
 * 11. Is Initialized
 * ============================================================================
 */
uint8_t ILI9341_RTOS_IsInitialized(void)
{
    return (s_initialized != 0U) ? 1U : 0U;
}

/* ============================================================================
 * 12. Direct Lock
 * ============================================================================
 */
ILI9341_Status_t ILI9341_RTOS_Lock(TickType_t timeout_ticks)
{
    /* RTOS 层必须已经完整建立。 */
    if ((s_initialized == 0U) || (s_lcd_mutex == NULL))
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    /*
     * DeInit 已关闭新入口时，普通任务不能再拿新锁。
     * 但正在执行旧 Job 的 Service Task 可以继续递归 Lock 完成收尾。
     */
    if ((s_accept_jobs == 0U) &&
        (xTaskGetCurrentTaskHandle() != s_service_task))
    {
        return ILI9341_ERROR_BUSY;
    }

    /* 尝试在指定时间内获得递归 Mutex。 */
    if (xSemaphoreTakeRecursive(
            s_lcd_mutex,
            timeout_ticks
        ) != pdTRUE)
    {
        /* 0 tick 失败更接近 BUSY；有等待时间则表达为 TIMEOUT。 */
        return (timeout_ticks == 0U) ?
            ILI9341_ERROR_BUSY :
            ILI9341_ERROR_TIMEOUT;
    }

    return ILI9341_OK;
}

/* ============================================================================
 * 13. Direct Unlock
 * ============================================================================
 */
ILI9341_Status_t ILI9341_RTOS_Unlock(void)
{
    /* Mutex 必须存在。 */
    if (s_lcd_mutex == NULL)
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    /*
     * GiveRecursive 只有 Mutex 当前属于调用任务时才会成功。
     * 失败说明调用顺序/所有者不正确。
     */
    if (xSemaphoreGiveRecursive(s_lcd_mutex) != pdTRUE)
    {
        return ILI9341_ERROR_STATE;
    }

    return ILI9341_OK;
}

/* ============================================================================
 * 14. Execute Job Through Service Task
 * ============================================================================
 */
ILI9341_Status_t ILI9341_RTOS_Execute(
    ILI9341_RTOS_JobFunction_t function,   //要干什么
    void *user_context,   //干这件事需要带什么参数
    TickType_t queue_wait_ticks,  //提交这项工作时 我最多愿意等多久
    ILI9341_Status_t *job_result  //这项工作做完之后，结果告诉我
)
{
    /* 请求对象放在当前调用任务栈上。 */
    ILI9341_RTOS_JobRequest_t request;  //一张工作单

    /* Queue 保存 request 指针，因此需要一个指针变量作为发送元素。 */
    ILI9341_RTOS_JobRequest_t *request_pointer;  //工作单的地址

    /* 当前提交任务。 */
    TaskHandle_t current_task;  //是谁提交的这张工作单

    /* 等待 Job 完成的通知计数。 */
    uint32_t notification_count;  //后面有没有真的收到“工作完成”通知

    /* function 必须存在。 */
    if (function == NULL)
    {
        return ILI9341_ERROR_PARAM;
    }

    /* RTOS 服务层资源必须已经初始化。 */
    if ((s_initialized == 0U) ||
        (s_job_queue == NULL))
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    /* 取得当前任务句柄。 */
    current_task = xTaskGetCurrentTaskHandle();

    /* 本 API 只能由 Task 上下文调用。 */
    if (current_task == NULL)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * 如果 Service Task 内部的 Job 又调用 Execute()，不能再排队等待自己。
     * 即使 DeInit 已经关闭“新外部入口”，正在执行的旧 Job 仍允许完成嵌套调用。
     */
    if (current_task == s_service_task)
    {
        ILI9341_Status_t direct_result = function(user_context);

        if (job_result != NULL)
        {
            *job_result = direct_result;
        }

        return ILI9341_OK;
    }

    /*
     * 取得提交 Mutex，把“检查入口 + 入队”与 DeInit 的 stop 哨兵严格排序。
     */
    if (xSemaphoreTake(
            s_submit_mutex,
            queue_wait_ticks
        ) != pdTRUE)
    {
        return (queue_wait_ticks == 0U) ?
            ILI9341_ERROR_BUSY :
            ILI9341_ERROR_TIMEOUT;
    }

    /* DeInit 已开始时，不再接受新的外部 Job。 */
    if ((s_accept_jobs == 0U) || (s_service_task == NULL))
    {
        (void)xSemaphoreGive(s_submit_mutex);
        return ILI9341_ERROR_BUSY;
    }

    /* 新 Job 前清掉调用任务 Index 1 上可能残留的旧通知。 */
    prvDrainNotification((UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_JOB);

    /* 填写请求。 */
    request.function = function;
    request.user_context = user_context;
    request.caller_task = current_task;
    request.result = ILI9341_ERROR_STATE;
    request.stop_service = 0U;

    /* Queue 元素是“指向 request 的指针”。 */
    request_pointer = &request;

    /*
     * queue_wait_ticks 同时限制“取得提交 Mutex”和“等待 Queue 空位”。
     * 为了不在这里制造复杂的剩余 tick 计算，Mutex 成功后 Queue 仍使用同一上限。
     */
    if (xQueueSend(
            s_job_queue,
            &request_pointer,
            queue_wait_ticks
        ) != pdPASS)
    {
        (void)xSemaphoreGive(s_submit_mutex);

        return (queue_wait_ticks == 0U) ?
            ILI9341_ERROR_BUSY :
            ILI9341_ERROR_TIMEOUT;
    }

    /* 普通 Job 已经排在 stop 哨兵之前或 DeInit 还没开始，释放提交 Mutex。 */
    (void)xSemaphoreGive(s_submit_mutex);

    /*
     * 一旦成功入队，绝不能再设置有限等待超时后返回，
     * 否则 request 栈对象可能失效，而 Queue/Service Task 仍持有它的指针。
     * 所以这里固定 portMAX_DELAY，直到服务任务明确通知完成。
     */
    notification_count =
        ulTaskNotifyTakeIndexed(
            (UBaseType_t)ILI9341_RTOS_NOTIFY_INDEX_JOB,
            pdTRUE,
            portMAX_DELAY
        );

    /* 无限等待正常不会得到 0；防御性处理。 */
    if (notification_count == 0UL)
    {
        return ILI9341_ERROR_HW;
    }

    /* 如果调用者需要，返回 Job 真正的 Core/GFX 结果。 */
    if (job_result != NULL)
    {
        *job_result = request.result;
    }

    /* 调度本身成功。 */
    return ILI9341_OK;
}

/* ============================================================================
 * 15. Complete Window Write by Chunked DMA
 * ============================================================================
 */
ILI9341_Status_t ILI9341_RTOS_WriteWindowBytesDMA(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t *wire_bytes,
    uint32_t data_length,
    TickType_t lock_wait_ticks,
    TickType_t dma_wait_ticks
)
{
    /* 使用 64 bit 计算理论 Byte 数，避免乘法中间溢出。 */
    uint64_t expected_length;

    /* 当前数据偏移。 */
    uint32_t offset;

    /* 还剩多少 Byte 没发送。 */
    uint32_t remaining;

    /* 当前这一笔 DMA 发送多少 Byte。 */
    uint32_t chunk;

    /* Core/RTOS 状态。 */
    ILI9341_Status_t status;

    /* 单笔 DMA 最大传输值。 */
    uint32_t dma_max;

    /* 参数检查。 */
    if ((wire_bytes == NULL) ||
        (width == 0U) ||
        (height == 0U) ||
        (data_length == 0UL))
    {
        return ILI9341_ERROR_PARAM;
    }

    /* RGB565：width * height * 2 Byte。 */
    expected_length =
        (uint64_t)width *
        (uint64_t)height *
        2ULL;

    /* 当前接口要求调用者给出完整 Window 数据，长度必须严格一致。 */
    if ((expected_length > 0xFFFFFFFFULL) ||
        ((uint32_t)expected_length != data_length))
    {
        return ILI9341_ERROR_PARAM;
    }

    /* 先取得统一 LCD Mutex，保证整个 Window Transaction 不被别的任务插入。 */
    status = ILI9341_RTOS_Lock(lock_wait_ticks);

    if (status != ILI9341_OK)
    {
        return status;
    }

    /* 建立一个完整 RAMWR Window。 */
    status = ILI9341_BeginWrite(
        x,
        y,
        width,
        height
    );

    /* BeginWrite 失败时还没有有效 Transaction，只需 Unlock。 */
    if (status != ILI9341_OK)
    {
        (void)ILI9341_RTOS_Unlock();
        return status;
    }

    /* 向 Core 查询当前 Port 单笔 DMA 的硬件最大值。 */
    dma_max = ILI9341_GetDMAMaxTransfer();

    /* 0 表示底层能力异常，不能进入循环。 */
    if (dma_max == 0UL)
    {
        (void)ILI9341_AbortWrite();
        (void)ILI9341_RTOS_Unlock();
        return ILI9341_ERROR_HW;
    }

    /* 初始化遍历状态。 */
    offset = 0UL;
    remaining = data_length;

    /* 把整块数据按 Port 能力拆成多笔 DMA。 */
    while (remaining > 0UL)
    {
        /* 默认本轮发送全部剩余数据。 */
        chunk = remaining;

        /* 超过单笔硬件上限时截成 dma_max。 */
        if (chunk > dma_max)
        {
            chunk = dma_max;
        }

        /*
         * 启动这一块 DMA -> 等 IRQ -> Finalize。
         * 注意 chunk 可以是奇数；整个 RAMWR Byte Stream 仍然连续。
         */
        status = prvDMAChunkWait(
            wire_bytes + offset,
            chunk,
            dma_wait_ticks
        );

        /* 任意一块失败都终止整个 Window Transaction。 */
        if (status != ILI9341_OK)
        {
            (void)ILI9341_AbortWrite();
            (void)ILI9341_RTOS_Unlock();
            return status;
        }

        /* 本块已正式 committed，推进数据指针。 */
        offset += chunk;

        /* 更新剩余量。 */
        remaining -= chunk;
    }

    /* 所有 DMA 块都 Finalize 成功以后，正常结束 RAMWR Transaction。 */
    status = ILI9341_EndWrite();

    /*
     * 无论 EndWrite 成功与否，都必须释放 Mutex。
     * EndWrite 如果失败且 Transaction 仍开着，尝试 Abort 作为额外恢复。
     */
    if (status != ILI9341_OK)
    {
        (void)ILI9341_AbortWrite();
    }

    /* 与最前面的 RTOS_Lock 成对。 */
    (void)ILI9341_RTOS_Unlock();

    return status;
}
