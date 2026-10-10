#ifndef ILI9341_FREERTOS_H
#define ILI9341_FREERTOS_H

/*
 * ============================================================================
 * @file    ili9341_freertos.h
 * @brief   ILI9341 的 FreeRTOS 串行化、服务任务与 DMA 等待层公开接口
 * ============================================================================
 *
 * 一、这一层在整个驱动中的位置
 * ---------------------------------------------------------------------------
 *
 *      Application / UI Tasks
 *               |
 *               v
 *      ili9341_freertos.h / .c          <- 当前层
 *               |
 *        +------+------+
 *        |             |
 *        v             v
 *   ili9341_gfx     ili9341 Core
 *        |             |
 *        +------+------+
 *               |
 *               v
 *          ili9341_port
 *
 * 二、这一层解决什么问题
 * ---------------------------------------------------------------------------
 * Core / GFX 本身故意不依赖 FreeRTOS，因此它们也不会自动创建 Mutex。
 * 当多个任务同时操作 LCD 时，如果没有额外串行化，可能出现：
 *
 *      Task A: BeginWrite()
 *      Task B: SetRotation()
 *      Task A: Write...
 *
 * 这样的交叉调用会破坏 Core 的 Transaction 状态机。
 *
 * 本层提供：
 *      1. Recursive Mutex（递归互斥锁），保护直接 Core/GFX 访问；
 *      2. Service Task（LCD 服务任务），把上层 Job 串行执行；
 *      3. Queue（任务队列），把 Job 送给服务任务；
 *      4. Task Notification（任务通知），通知提交者 Job 已完成；
 *      5. DMA Callback -> Task Notification 的 ISR 到任务桥接；
 *      6. 大块 raw RGB565 wire-byte 数据的分块 DMA 发送。
 *
 * 三、为什么使用两个 Notification Index
 * ---------------------------------------------------------------------------
 * 当前工程 FreeRTOSConfig.h 中：
 *
 *      configTASK_NOTIFICATION_ARRAY_ENTRIES = 3
 *
 * 因此这里默认：
 *
 *      Index 1 -> Service Job 完成通知；
 *      Index 2 -> DMA 完成/错误通知。
 *
 * Index 0 保留给应用自己的普通 Task Notification，减少相互干扰。
 *
 * 四、重要约束
 * ---------------------------------------------------------------------------
 * 1. DMA IRQ 的 NVIC 抢占优先级必须满足 FreeRTOS FromISR API 要求。
 *    当前 Port 使用优先级 6，而工程 max syscall priority 为 5，满足要求。
 *
 * 2. 本层的 Job Queue 保存的是“指向调用者栈上请求对象的指针”。
 *    因此一旦 Job 成功进入 Queue，提交者必须一直等待到服务任务通知完成。
 *    代码内部已经这样实现：queue_wait_ticks 只控制“入队等待时间”；
 *    入队成功后不会再超时返回，从而避免悬空指针。
 *
 * 3. ILI9341_RTOS_WriteWindowBytesDMA() 接受的是已经按 ILI9341 线上顺序
 *    排列好的 uint8_t Byte Stream，例如 RGB565 0xF800 必须是 F8 00。
 *    普通 STM32 uint16_t[] 小端数组不能直接传入这个 DMA 接口。
 *
 * 4. 本层不修改已经冻结的 Port / Core / GFX 代码。
 * ============================================================================
 */

/* Core 状态码以及 Core API。 */
#include "ili9341.h"

/* FreeRTOS 基础类型和配置宏。 */
#include "FreeRTOS.h"

/* TaskHandle_t、TickType_t、任务通知 API。 */
#include "task.h"

/*
 * 编译期配置检查：当前实现明确依赖动态对象、递归 Mutex 和至少 3 个通知槽。
 */
#if (configSUPPORT_DYNAMIC_ALLOCATION != 1)
#error "ili9341_freertos.c requires configSUPPORT_DYNAMIC_ALLOCATION == 1"
#endif

#if (configUSE_MUTEXES != 1)
#error "ili9341_freertos.c requires configUSE_MUTEXES == 1"
#endif

#if (configUSE_RECURSIVE_MUTEXES != 1)
#error "ili9341_freertos.c requires configUSE_RECURSIVE_MUTEXES == 1"
#endif


#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. 可配置参数
 * ============================================================================
 */

/* LCD 服务任务队列最多可以等待多少个 Job。 */
#ifndef ILI9341_RTOS_QUEUE_LENGTH
#define ILI9341_RTOS_QUEUE_LENGTH                 8U
#endif

/* 服务任务栈深，单位是 StackType_t 个数，不是 Byte。 */
#ifndef ILI9341_RTOS_SERVICE_STACK_DEPTH
#define ILI9341_RTOS_SERVICE_STACK_DEPTH          768U
#endif

/* 服务任务优先级。默认比 idle 高 2 级。 */
#ifndef ILI9341_RTOS_SERVICE_PRIORITY
#define ILI9341_RTOS_SERVICE_PRIORITY             (tskIDLE_PRIORITY + 2U)
#endif

/* Job 完成使用的任务通知数组下标。 */
#ifndef ILI9341_RTOS_NOTIFY_INDEX_JOB
#define ILI9341_RTOS_NOTIFY_INDEX_JOB             1U
#endif

/* DMA ISR -> 等待任务使用的任务通知数组下标。 */
#ifndef ILI9341_RTOS_NOTIFY_INDEX_DMA
#define ILI9341_RTOS_NOTIFY_INDEX_DMA             2U
#endif

/* 两个内部通知下标都必须落在 FreeRTOS 配置的通知数组范围内。 */
#if (ILI9341_RTOS_NOTIFY_INDEX_JOB >= configTASK_NOTIFICATION_ARRAY_ENTRIES)
#error "ILI9341_RTOS_NOTIFY_INDEX_JOB is outside configTASK_NOTIFICATION_ARRAY_ENTRIES"
#endif

#if (ILI9341_RTOS_NOTIFY_INDEX_DMA >= configTASK_NOTIFICATION_ARRAY_ENTRIES)
#error "ILI9341_RTOS_NOTIFY_INDEX_DMA is outside configTASK_NOTIFICATION_ARRAY_ENTRIES"
#endif

#if (ILI9341_RTOS_NOTIFY_INDEX_JOB == ILI9341_RTOS_NOTIFY_INDEX_DMA)
#error "ILI9341 RTOS Job and DMA notification indexes must be different"
#endif

/* ============================================================================
 * 2. Job 类型
 * ============================================================================
 *
 * function：真正需要在 LCD 服务任务中执行的函数。
 * user_context：用户自定义上下文，可指向结构体、参数集合等。
 *
 * Job 函数直接返回 Core 的 ILI9341_Status_t，这样 GFX/Core 错误可以原样
 * 传回提交任务，不再人为制造第二套错误码。
 */
typedef ILI9341_Status_t (*ILI9341_RTOS_JobFunction_t)(void *user_context);

/* ============================================================================
 * 3. 生命周期
 * ============================================================================
 */

/*
 * 初始化 FreeRTOS 资源，并初始化 ILI9341 Core。
 *
 * 成功：ILI9341_OK
 * 常见失败：
 *      ILI9341_ERROR_HW      -> Queue/Mutex/Task 创建失败或底层初始化失败映射；
 *      其他 Core 状态码      -> ILI9341_Init() 原样返回。
 */
ILI9341_Status_t ILI9341_RTOS_Init(void);

/*
 * 反初始化当前 RTOS 层与 Core。
 * 调用前应用应停止继续提交新的 LCD Job。
 */
ILI9341_Status_t ILI9341_RTOS_DeInit(void);

/* 返回 1 表示 RTOS 层已完全初始化；返回 0 表示未初始化。 */
uint8_t ILI9341_RTOS_IsInitialized(void);

/* ============================================================================
 * 4. 直接访问锁
 * ============================================================================
 *
 * 如果某个任务不想通过 Service Queue，而要直接连续调用多个 Core/GFX API，
 * 可以先 Lock，再执行一组操作，最后 Unlock。
 */
ILI9341_Status_t ILI9341_RTOS_Lock(TickType_t timeout_ticks);
ILI9341_Status_t ILI9341_RTOS_Unlock(void);

/* ============================================================================
 * 5. 服务任务 Job
 * ============================================================================
 */

/*
 * 把一个 Job 交给 LCD Service Task 串行执行。
 *
 * queue_wait_ticks：
 *      用于“提交阶段”的等待上限：先等待 Submit Mutex，再等待 Queue 空位。
 *      两个阶段分别使用该上限，因此极端情况下总提交等待时间可能接近两倍。
 *
 * job_result：
 *      如果非 NULL，函数成功完成后会写入 Job 本身返回的 ILI9341_Status_t。
 *
 * 返回值表示“Job 调度过程”是否成功：
 *      ILI9341_OK            -> Job 已执行完成；真正的绘图结果看 *job_result；
 *      ILI9341_ERROR_BUSY    -> Queue 在 0 tick 尝试时已满；
 *      ILI9341_ERROR_TIMEOUT -> 等待 Queue 空位超时；
 *      ILI9341_ERROR_NOT_INITIALIZED -> 本层未初始化。
 *
 * 如果调用者本身就是 LCD Service Task，则不会再次排队，而是直接执行 Job，
 * 避免“服务任务等待自己处理队列”形成死锁。
 */
ILI9341_Status_t ILI9341_RTOS_Execute(
    ILI9341_RTOS_JobFunction_t function,
    void *user_context,
    TickType_t queue_wait_ticks,
    ILI9341_Status_t *job_result
);

/* ============================================================================
 * 6. DMA Window 写入
 * ============================================================================
 */

/*
 * 在一个完整的 BeginWrite -> 多笔 DMA -> EndWrite Transaction 中，
 * 把 wire-order Byte Stream 写入指定矩形。
 *
 * data_length 必须严格等于：width * height * 2。
 *
 * 单笔 DMA 会自动根据 ILI9341_GetDMAMaxTransfer() 分块，
 * 因此整屏 153600 Byte 也可以一次调用本 API。
 *
 * dma_wait_ticks：每一笔 DMA 等待 IRQ 的最长时间。
 * 任意一笔超时/失败都会调用 AbortWrite() 尝试恢复当前 Transaction。
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
);

#ifdef __cplusplus
}
#endif

#endif /* ILI9341_FREERTOS_H */
