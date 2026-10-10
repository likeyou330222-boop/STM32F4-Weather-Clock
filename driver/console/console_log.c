/**
 * @file    console_log.c
 * @brief   STM32F407 FreeRTOS 多任务串口日志（整条消息互斥输出）。
 *
 * @details
 * 为什么不能仅在 board.c 的 fputc() 内部加 Mutex？
 *     printf() 会产生多个单独的字符发送操作。如果每个字符都单独加锁，
 *     Task A 输出了 '[S' 后仍可能切到 Task B，继续出现日志交错。
 *
 * 本模块的解决方法：
 *     1. 任务调用 ConsoleLog_Printf()；
 *     2. 使用 xSemaphoreTake() 获取互斥量（支持优先级继承）；
 *     3. 在共享静态缓冲区中格式化完整的日志文本；
 *     4. 调用原 console_write() 一次性发送该文本；
 *     5. 使用 xSemaphoreGive() 释放互斥量。
 *
 * 持有 Mutex 时既保护格式化缓冲区，也保护 USART1 的整条输出。
 * 这里不新建 LogTask / Message Queue，因为当前日志速率很低、堆空间有限。
 *
 * 本模块不是 ISR-safe，也不是无阻塞 DMA Logger。
 */

#include "console_log.h"            /* ConsoleLog_Init / ConsoleLog_Printf */
#include "console.h"                /* 原有 console_write() 接口 */

#include <stdarg.h>                  /* va_list、va_start、va_end */
#include <stddef.h>                  /* NULL */
#include <stdio.h>                   /* vsnprintf() */
#include <string.h>                  /* memcpy() */

#include "FreeRTOS.h"               /* BaseType_t、TickType_t */
#include "task.h"                   /* xTaskGetSchedulerState() */
#include "semphr.h"                 /* xSemaphoreCreateMutex/Take/Give */

/*
 * 整条日志的最大缓存容量（含末尾 '\0'）。
 *
 * 存储区域是 static RAM，而不是当前任务的栈。
 * 例如 384 字节长度的输出字符串最多容纳 383 字节字符。
 * 若超限，后续会用 ...\r\n 结束当前行。
 */
#define CONSOLE_LOG_BUFFER_SIZE       384U

/*
 * 获得日志 Mutex 的最长等待时间。
 * 如果另一任务迟迟不释放日志互斥量，主动舍弃当前日志，
 * 避免普通业务任务永久等待日志资源。
 *
 * 注意：这只限制等待 Mutex 的时间，不限制持锁后 UART 发送耗时。
 */
#define CONSOLE_LOG_LOCK_WAIT_MS      500U

/* 全局唯一的日志互斥量，初始化后不再被删除。 */
static SemaphoreHandle_t s_log_mutex = NULL;

/* 共享的日志格式化缓冲区，只有持有 Mutex 的任务才能使用。 */
static char s_log_buffer[CONSOLE_LOG_BUFFER_SIZE];

/**
 * @brief 创建用于保护串口日志的互斥量。
 *
 * xSemaphoreCreateMutex()：
 *   创建 FreeRTOS 互斥量，支持优先级继承（Priority Inheritance）。
 *   底层会从 FreeRTOS heap_4 申请少量 RAM。
 *
 * 本函数必须在 vTaskStartScheduler() 之前由 board_init() 调用。
 */
uint8_t ConsoleLog_Init(void)
{
    if (s_log_mutex != NULL)
    {
        /* 初始化接口允许重复调用，不重复创建 Mutex。 */
        return 1U;
    }

    /*
     * 不在任务已经运行时动态创建此 Mutex，避免其他任务先调用日志 API。
     * 也避免调度器暂停等特殊时机创建内核对象。
     */
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        return 0U;
    }

    s_log_mutex = xSemaphoreCreateMutex();
    return (s_log_mutex != NULL) ? 1U : 0U;
}

/**
 * @brief 安全地输出一条完整的格式化日志。
 *
 * 不应在 ISR / FreeRTOS Hook / 调度器挂起或中断被屏蔽的代码中调用。
 *
 * 这里使用 static 缓冲区而不是局部大数组，是因为之前已测量出
 * NetworkTask 的栈余量虽然充足，但没有必要让每次输出额外占用 384 字节栈。
 */
int ConsoleLog_Printf(const char *format, ...)
{
    BaseType_t lock_result = pdFAIL;
    BaseType_t need_unlock = pdFALSE;
    BaseType_t scheduler_state;
    va_list arguments;
    int format_length;
    int actual_length;

    /*
     * 参数防御：format 必须指向有效的 C 格式字符串。
     * Mutex 必须已经由 ConsoleLog_Init() 创建成功。
     */
    if ((format == NULL) || (s_log_mutex == NULL))
    {
        return -1;
    }

    scheduler_state = xTaskGetSchedulerState();

    if (scheduler_state == taskSCHEDULER_RUNNING)
    {
        /*
         * FreeRTOS 已运行：必须先取得 Mutex。
         * 只有此任务持有 Mutex 时，才能使用 s_log_buffer 和 UART1。
         */
        lock_result = xSemaphoreTake(
            s_log_mutex,
            pdMS_TO_TICKS(CONSOLE_LOG_LOCK_WAIT_MS)
        );

        if (lock_result != pdTRUE)
        {
            /* 日志只能作为诊断；拿不到锁不应阻塞业务直到死锁。 */
            return -2;
        }

        need_unlock = pdTRUE;
    }
    else if (scheduler_state == taskSCHEDULER_NOT_STARTED)
    {
        /*
         * 启动阶段只有 main 上下文，没有并发任务。
         * 因此无需调用 xSemaphoreTake()，也可以合法输出日志。
         */
    }
    else
    {
        /*
         * 例如调度器处于 SUSPENDED：不调用可能导致阻塞的日志函数。
         * 这里没有取得 Mutex，因此直接返回。
         */
        return -1;
    }

    /*
     * va_list 是处理可变参数的标准 C 机制。
     * vsnprintf() 在固定缓冲区中完成格式化，自动预留尾部 '\0'。
     * 我们将格式化过程也放在锁内，避免不同任务同时写 s_log_buffer。
     */
    va_start(arguments, format);
    format_length = vsnprintf(
        s_log_buffer,
        sizeof(s_log_buffer),
        format,
        arguments
    );
    va_end(arguments);

    if (format_length < 0)
    {
        /* 格式化失败时仍必须释放 Mutex。 */
        if (need_unlock == pdTRUE)
        {
            (void)xSemaphoreGive(s_log_mutex);
        }
        return -1;
    }

    if ((size_t)format_length >= sizeof(s_log_buffer))
    {
        /*
         * 字符串超过最大容量：替换最后几个字节为 ...\r\n，
         * 避免在串口上出现没有行结束符的截断日志。
         * sizeof("...\r\n") 包括末尾字符串终止符，恰好 6 字节。
         */
        memcpy(
            s_log_buffer + sizeof(s_log_buffer) - sizeof("...\r\n"),
            "...\r\n",
            sizeof("...\r\n")
        );
        actual_length = (int)(sizeof(s_log_buffer) - 1U);
    }
    else
    {
        /* vsnprintf() 返回不包含末尾 '\0' 的实际字符数。 */
        actual_length = format_length;
    }

    /*
     * console_write() 是原工程中已有的 USART1 轮询发送接口。
     * 需要注意它是“裸驱动”：自身不做 FreeRTOS 任务间互斥。
     * 由于我们在这里持有 Mutex，这条输出不会被其他 ConsoleLog 任务打断。
     */
    console_write(s_log_buffer);

    if (need_unlock == pdTRUE)
    {
        (void)xSemaphoreGive(s_log_mutex);
    }

    return actual_length;
}
