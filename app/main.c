#include "stm32f4xx.h"
#include "misc.h"

#include "FreeRTOS.h"
#include "task.h"

#include "ili9341.h"
#include "ili9341_freertos.h"

#include "board.h"
#include "app.h"

#include <stdio.h>

#include "rtc.h"



/*
 * ============================================================================
 * 系统故障诊断信息
 * ============================================================================
 * 这些变量仅用于记录致命错误。
 * 使用 volatile：
 *     告诉编译器这些变量的值可能在程序正常控制流之外被观察，避免优化器删除必要的存储操作。
 * 使用全局变量：
 *     方便在 Keil Debug 的 Watch 窗口中查看。
 * 错误代码：
 *     0：未发生记录的致命错误
 *     1：FreeRTOS 断言失败
 *     2：FreeRTOS 任务栈溢出
 *     3：FreeRTOS 内存分配失败
 */
volatile uint32_t g_system_fault_code = 0U;

/* 发生断言的源文件位置。 */
const char * volatile g_system_fault_file = NULL;

/* 发生断言的代码行号。 */
volatile int g_system_fault_line = 0;

/* 发生任务栈溢出时的任务名称。 */
const char * volatile g_system_fault_task_name = NULL;

/* 发生任务栈溢出时的任务句柄。 */
TaskHandle_t volatile g_system_fault_task_handle = NULL;






int main(void)
{

    ILI9341_Status_t lcd_status;

    RTC_Status_t rtc_status;
    RTC_DateTime_t test_time;

    App_Status_t app_status;


    /*
     * 配置 STM32 的中断优先级分组。
     *
     * 当前 Console 会使用 USART1 中断，
     * 后面 FreeRTOS、LCD DMA、ESP32-C3 UART
     * 也都会使用中断。
     *
     * 所以在初始化这些具体外设之前，
     * 先统一规定整个工程的中断优先级规则。
     */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    board_lowlevel_init();

    board_init();

    printf("[BOOT] Weather clock starting...\r\n");

    rtc_status = RTC_Driver_Init();  //初始化RTC
    if (rtc_status != RTC_STATUS_OK)
    {
        printf("[BOOT] RTC init failed: %d\r\n", (int)rtc_status);
        while (1)
        {
        }
    }
    printf( "[BOOT] RTC init OK\r\n");


    if (RTC_Driver_HasValidDateTime() == 0U)  //检查 RTC 是否已经拥有有效时间。
    {
        /*
        * ------------------------------------------------------------
        * 设置一个临时测试时间
        * ------------------------------------------------------------
        * 注意：这只是 RTC Bring-up 阶段使用。
        * 后面正式天气时钟绝对不会每次开机都把时间重新写成这个固定值。
        */
        test_time.year    = 2026U;
        test_time.month   = 9U;
        test_time.day     = 22U;
        test_time.hour    = 12U;
        test_time.minute  = 0U;
        test_time.second  = 0U;
        test_time.weekday = 2U;


        rtc_status = RTC_Driver_SetDateTime(&test_time);


        if (rtc_status != RTC_STATUS_OK)
        {
            printf("[BOOT] RTC set failed: %d\r\n",(int)rtc_status);
            while (1)
            {
            }
        }


        printf("[BOOT] RTC test time set OK\r\n");

    }
    else
    {
        /*
        * RTC 曾经初始化过。不重新设置日期时间。
        */
        printf("[BOOT] RTC retained time detected\r\n");
    }






    lcd_status = ILI9341_RTOS_Init();  //初始化LCD
    if (lcd_status != ILI9341_OK)
    {
        printf("[BOOT] LCD init failed: %d\r\n", (int)lcd_status);
        while (1)
        {
        }
    }
    printf("[BOOT] LCD init OK\r\n");


     /*
     * ------------------------------------------------------------
     * 初始化天气时钟 Application
     * 当前 App_Init() 会创建 AppTask。
     * ------------------------------------------------------------
     */
    app_status = App_Init();

    if (app_status != APP_OK)
    {
        printf("[BOOT] App init failed: %d\r\n", (int)app_status);
        while (1)
        {
        }
    }

    printf("[BOOT] App init OK\r\n");
    //检查调度器启动之前还剩多少 FreeRTOS Heap
    printf("[BOOT] FreeRTOS free heap: %lu bytes\r\n", (unsigned long)xPortGetFreeHeapSize());


     /*
     * ------------------------------------------------------------
     * 启动 FreeRTOS Scheduler
     * ------------------------------------------------------------
     */
    vTaskStartScheduler();


    /*
     * 正常情况下不会执行到这里。如果执行到了这里，说明 Scheduler 启动失败。
     */
    printf("[BOOT] Scheduler start failed\r\n");


    while (1)
    {
    }
}


















/*
 * ============================================================================
 * prvSystemFatalStop
 * ============================================================================
 * 功能：
 *     记录系统发生的致命错误，并停止程序继续运行。
 * 参数：
 *     fault_code：错误类型编号。
 *     file：      发生断言的源文件。
 *     line：      发生断言的行号。
 *     task_name： 发生错误的任务名称。
 *     task_handle：发生错误的任务句柄。
 * 注意：
 *     这是致命错误处理函数，不会正常返回。
 *     这里不使用 printf()。
 * 原因：
 *     如果发生栈溢出或内存异常，
 *     此时再执行复杂的串口格式化输出可能不安全。
 * 建议通过 Keil Debug Watch 查看全局故障变量。
 */
static void prvSystemFatalStop(
    uint32_t fault_code,
    const char *file,
    int line,
    const char *task_name,
    TaskHandle_t task_handle
)
{
    /*
     * 第一阶段：禁止可屏蔽中断，
     * 避免其他任务或外设继续执行不可靠操作。
     */
    __disable_irq();

    /* 第二阶段：记录错误信息。 */
    g_system_fault_code = fault_code;

    g_system_fault_file = file;

    g_system_fault_line = line;

    g_system_fault_task_name = task_name;

    g_system_fault_task_handle = task_handle;

    /*
     * 第三阶段：停止程序继续运行。
     *
     * __NOP() 是处理器空操作指令。
     *
     * 在 Keil Debug 模式下，
     * 可以暂停程序并查看上述故障变量。
     *
     * 这里不主动复位，目的是保留错误现场。
     */
    for (;;)
    {
        __NOP();
    }
}







/*
 * ============================================================================
 * 1. FreeRTOS 断言失败 Hook
 * ============================================================================
 * 当 configASSERT() 检查失败时进入。
 * 记录：
 *     错误代码 = 1
 *     错误文件
 *     错误行号
 */
void vAssertCalled(const char *file, int line)
{
    prvSystemFatalStop(
        1U,
        file,
        line,
        NULL,
        NULL
    );
}

/*
 * ============================================================================
 * 2. FreeRTOS 任务栈溢出 Hook
 * ============================================================================
 * 当 FreeRTOS 检测到任务栈溢出时进入。
 * xTask：  发生栈溢出的任务句柄。
 * pcTaskName： 发生栈溢出的任务名称。
 * 记录：  错误代码 = 2
 */
void vApplicationStackOverflowHook(
    TaskHandle_t xTask,
    char *pcTaskName
)
{
    prvSystemFatalStop(
        2U,
        NULL,
        0,
        pcTaskName,
        xTask
    );
}


/*
 * ============================================================================
 * 3. FreeRTOS 动态内存分配失败 Hook
 * ============================================================================
 * 当 FreeRTOS 无法完成动态内存分配时进入。
 * 记录：   错误代码 = 3
 */
void vApplicationMallocFailedHook(void)
{
    prvSystemFatalStop(
        3U,
        NULL,
        0,
        NULL,
        NULL
    );
}

