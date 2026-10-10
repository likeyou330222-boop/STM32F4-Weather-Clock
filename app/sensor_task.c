#include "sensor_task.h"
#include "app.h"             /* App_NotifyEvent()，只传事件不传测量数据。 */

#include <stdio.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "aht20.h"
#include "console_log.h"  /* FreeRTOS Mutex：整条 USART1 日志互斥输出 */



//FreeRTOS 中这里的单位通常是 StackType_t，STM32F407 使用 32-bit StackType_t，所以 512 个 StackType_t 大约对应 2048 Byte。
#define SENSOR_TASK_STACK_DEPTH               512U  //给该任务配置栈深 

#define SENSOR_TASK_PRIORITY                  2U    //配置优先级

#define SENSOR_TASK_MEASUREMENT_WAIT_MS       85U   //AHT20测量等待时间，手册要求80MS

#define SENSOR_TASK_SAMPLE_PERIOD_MS          2000U //温湿度采样周期 单位是ms


//这里不是保存历史测量序列， 而是只保存“当前最新的一份温湿度”。 所以 Queue Length = 1。
#define SENSOR_TASK_MEASUREMENT_QUEUE_LENGTH    1U

/*
 * 正常测量值是否每 2 秒输出到串口：
 *   0：默认关闭。温湿度仍正常写入 Queue，并通知 AppTask 更新 LCD。
 *   1：开发调试时临时开启，打印每一次采样结果。
 *
 * 这个开关只影响日志，不影响 AHT20 的采样周期和测量数据。
 */
#define SENSOR_TASK_PRINT_EACH_SAMPLE             0U




/*
 * ============================================================================
 * Sensor Measurement Queue Handle
 * ============================================================================
 * QueueHandle_t：  FreeRTOS 用来表示一个 Queue 的句柄类型。
 * s_measurement_queue：  保存 SensorTask 最新测量结果的 Queue。
 * static： 只允许 sensor_task.c 内部直接访问这个 Queue。
 * 其他模块不能直接操作 Queue，
 * 必须通过：SensorTask_GetLatestMeasurement() 获取数据。
 */
static QueueHandle_t s_measurement_queue = NULL;





/*
 * ============================================================================
 * 私有函数声明
 * ============================================================================
 */
//SensorTask 真正的任务入口函数，这个函数只由 FreeRTOS Scheduler 调用，App 层不直接调用它。
static void prvSensorTask(void *argument);

#if SENSOR_TASK_PRINT_EACH_SAMPLE
static void prvPrintMeasurement(const AHT20_Measurement_t *measurement);//打印一次温湿度
#endif




/*
 * ============================================================================
 * SensorTask_Init
 * ============================================================================
 */
SensorTask_Status_t SensorTask_Init(void)
{
    BaseType_t create_result;

    /*
     * =========================================================================
     * 创建最新温湿度 Queue
     * =========================================================================
     * Queue 长度：   1
     * 每个 Queue Item 的大小：   sizeof(AHT20_Measurement_t)
     * 也就是说这个 Queue 的每一个位置，能完整保存一份：  temperature_celsius  humidity_percent
     */
    s_measurement_queue = xQueueCreate(SENSOR_TASK_MEASUREMENT_QUEUE_LENGTH, sizeof(AHT20_Measurement_t));


    // xQueueCreate() 创建失败时返回 NULL。 最常见原因之一是 FreeRTOS Heap 不够。
    if (s_measurement_queue == NULL)
    {
        return SENSOR_TASK_ERROR_QUEUE_CREATE;
    }

    /*
     * =========================================================================
     * 创建 SensorTask
     * =========================================================================
     */
    create_result = xTaskCreate(prvSensorTask, "SensorTask", SENSOR_TASK_STACK_DEPTH, NULL, SENSOR_TASK_PRIORITY, NULL);


    /*
     * Task 创建失败。
     */
    if (create_result != pdPASS)
    {
        /*
         * Queue 已经创建成功，但是 Task 没创建成功。
         * 如果直接 return，Queue 占用的 FreeRTOS Heap 就会白白留下。
         * 所以把刚刚创建的 Queue 删除掉。
         */
        vQueueDelete(s_measurement_queue);

        s_measurement_queue = NULL;

        return SENSOR_TASK_ERROR_TASK_CREATE;
    }


    return SENSOR_TASK_OK;
}


SensorTask_Status_t SensorTask_GetLatestMeasurement(AHT20_Measurement_t *measurement)
{
    BaseType_t queue_result;

    if (measurement == NULL)  //参数检查
    {
        return SENSOR_TASK_ERROR_PARAM;
    }

    if (s_measurement_queue == NULL)//Queue 是否已经创建
    {
        return SENSOR_TASK_NO_DATA;
    }

    /*
     * =========================================================================
     * 查看 Queue 中最新的数据
     * =========================================================================
     * xQueuePeek() 和 xQueueReceive() 很像， 但是有一个重要区别：
     *     Receive： 读取以后，数据从 Queue 中删除。
     *      Peek：   读取以后，数据仍然保留在 Queue 中。
     * 我们这是“最新状态邮箱”，所以使用 Peek 更合适。
     * 最后的 0U： 不等待。 如果当前 Queue 还是空的，立即返回。
     */
    queue_result = xQueuePeek(s_measurement_queue, measurement, 0U);

    if (queue_result != pdPASS)
    {
        /*
         * 一般说明 SensorTask 还没有产生第一次有效数据。
         */
        return SENSOR_TASK_NO_DATA;
    }

    return SENSOR_TASK_OK;
}




/*
 * ============================================================================
 * prvSensorTask
 * ============================================================================
 *                  SensorTask启动
                       │
                       ↓
                  AHT20_Init()
                  /          \
               成功            失败
                │               │
                ↓               ↓
        sensor_ready=1    sensor_ready=0
                │               │
                │          下周期重试Init
                │               │
                └───────←───────┘
                ↓
       StartMeasurement()
          /          \
       成功            失败
        │               │
        ↓               ↓
     等85ms          本轮失败
        │               │
        ↓               │
 ReadMeasurement()      │
   /         \           │
成功         失败        │
 │            │          │
 ↓            ↓          │
输出数据     打印错误     │
 │            │          │
 └────────────┴──────────┘
              ↓
          等下个周期
              ↓
            再试
 */
static void prvSensorTask(void *argument)
{
    AHT20_Status_t aht20_status;
    AHT20_Measurement_t measurement;

    TickType_t last_wake_time;  //用于周期任务调度

    uint8_t sensor_ready = 0U;  //标记 AHT20 是否已经初始化成功 0是未成功


    // 当前没有通过 xTaskCreate() 给任务传参数。 显式写出来，避免编译器提示 argument 未使用。
    (void)argument;


    aht20_status = AHT20_Init();//初始化 AHT20

    if (aht20_status == AHT20_OK)  
    {
        sensor_ready = 1U;
        ConsoleLog_Printf("[SensorTask] AHT20 init OK\r\n");
    }
    else
    {
        sensor_ready = 0U;
        ConsoleLog_Printf("[SensorTask] AHT20 init failed: %d\r\n", (int)aht20_status);
    }

    last_wake_time = xTaskGetTickCount(); //获取当前 Tick，这个值会作为周期调度的时间基准

    for (;;) //SensorTask 永久运行
    {
        if (sensor_ready == 0U)
        {
            aht20_status = AHT20_Init();
            if (aht20_status == AHT20_OK)
            {
                sensor_ready = 1U;
                ConsoleLog_Printf("[SensorTask] AHT20 init recovered\r\n");
            }
            else
            {
                ConsoleLog_Printf("[SensorTask] AHT20 init retry failed: %d\r\n", (int)aht20_status);
                vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(SENSOR_TASK_SAMPLE_PERIOD_MS));
                continue;  //立即结束本轮 for 循环,从下一轮 for (;;) 顶部重新开始
            }
        }

        aht20_status = AHT20_StartMeasurement();  //触发一次 AHT20 测量

        if (aht20_status != AHT20_OK)  
        {
            ConsoleLog_Printf("[SensorTask] AHT20 read failed: %d\r\n", (int)aht20_status);
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(SENSOR_TASK_SAMPLE_PERIOD_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_TASK_MEASUREMENT_WAIT_MS));  //等待传感器完成测量，当前TASK进入阻塞状态

        aht20_status = AHT20_ReadMeasurement(&measurement);//读取解析后的温湿度

        if (aht20_status == AHT20_OK)
        {
#if SENSOR_TASK_PRINT_EACH_SAMPLE
            /* 调试开关打开后才打印数值；默认不增加串口负担。 */
            prvPrintMeasurement(&measurement);
#endif
            /* 先发布完整结构体，再通知 AppTask；Queue 长度为 1，只保存最新值。 */
            if (xQueueOverwrite(s_measurement_queue, &measurement) == pdPASS)
            {
                /* eSetBits 会合并重复事件；AppTask 读取最新数据，不回放历史采样。 */
                App_NotifyEvent(APP_EVENT_SENSOR_UPDATED);
            }
            else
            {
                ConsoleLog_Printf("[SensorTask][ERROR] Measurement queue publish failed\r\n");
            }
        }
        else
        {
            ConsoleLog_Printf("[SensorTask] AHT20 read failed: %d\r\n", (int)aht20_status);
        }
        
        //等待下一个固定采样周期
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(SENSOR_TASK_SAMPLE_PERIOD_MS));
    }
}


/*
 * ============================================================================
 * prvPrintMeasurement
 * ============================================================================
 */
#if SENSOR_TASK_PRINT_EACH_SAMPLE
static void prvPrintMeasurement(const AHT20_Measurement_t *measurement)
{
    int32_t temperature_x100;
    uint32_t humidity_x100;

    if (measurement == NULL)  //防御性参数检查
    {
        return;
    }

    temperature_x100 = (int32_t)(measurement->temperature_celsius * 100.0f);//将浮点数放大100倍转换成整数
    humidity_x100 = (uint32_t)(measurement->humidity_percent * 100.0f);

    /*
     * 温度可能为负数。
     */
    if (temperature_x100 < 0)
    {
        ConsoleLog_Printf(
            "[SensorTask] Temperature = -%ld.%02ld C\r\n",
            (long)(-temperature_x100 / 100),
            (long)(-temperature_x100 % 100)
        );
    }
    else
    {
        ConsoleLog_Printf(
            "[SensorTask] Temperature = %ld.%02ld C\r\n",
            (long)(temperature_x100 / 100),
            (long)(temperature_x100 % 100)
        );
    }


    /*
     * 相对湿度。
     */
    ConsoleLog_Printf(
        "[SensorTask] Humidity = %lu.%02lu %%RH\r\n",
        (unsigned long)(humidity_x100 / 100U),
        (unsigned long)(humidity_x100 % 100U)
    );
}




#endif /* SENSOR_TASK_PRINT_EACH_SAMPLE */
