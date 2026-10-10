/**
 * @file    app.c
 * @brief   STM32F407 天气时钟应用层 —— FreeRTOS 四任务 + LVGL 8.3.11 集成版。
 *
 * ============================================================================
 * 一、文件定位
 * ============================================================================
 * 本文件是应用层的“总协调者”，不直接操作 LCD GPIO / SPI / DMA，也不负责
 * AHT20 的 I2C 测量和 ESP32-C3 的 AT 协议实现。
 *
 * 原工程的四个 FreeRTOS 任务仍然保留：
 *
 *   SensorTask  -- 每约 2 秒采样 AHT20，将最新值放入长度为 1 的 Queue；
 *                  通过 App_NotifyEvent(APP_EVENT_SENSOR_UPDATED) 提醒 AppTask。
 *
 *   NetworkTask -- ESP32-C3 / Wi-Fi / HTTP / SNTP 的唯一拥有者；成功拿到天气
 *                  后保存快照，并通过 APP_EVENT_WEATHER_UPDATED 提醒 AppTask。
 *
 *   AppTask     -- 本文件实现。**唯一拥有 LVGL 对象和 lv_timer_handler()**。
 *                  周期读取 RTC、获取室内和室外快照、更新界面；不操作 SPI。
 *
 *   LCD Service -- 唯一执行 ILI9341 实际硬件访问的任务。LVGL 的 flush_cb
 *                  将 RGB565 绘图结果提交给 LCD Service，然后等待 DMA 完成。
 *
 * 二、LVGL 像素刷新调用关系
 * ============================================================================
 *
 *   AppTask
 *      |  更新天气 UI 的 label / 七段数字等 LVGL 对象
 *      v
 *   lv_timer_handler()
 *      |  LVGL 自动渲染失效区域
 *      v
 *   lvgl_port_disp.c 中的 flush_cb（仍运行于 AppTask）
 *      |  RGB565 字节序转换 + ILI9341_RTOS_Execute()
 *      v
 *   LCD Service Task
 *      |  ILI9341_RTOS_WriteWindowBytesDMA()
 *      v
 *   SPI DMA -> DMA IRQ -> LCD Service Task 收尾
 *      |  任务通知让等待中的 AppTask 继续
 *      v
 *   flush_cb 调用 lv_disp_flush_ready()，LVGL 才能复用绘图缓冲区。
 *
 * 三、为何 AppTask 统一持有 LVGL？
 * ============================================================================
 * LVGL 8.3 在本工程没有启用跨任务图形锁。若 SensorTask / NetworkTask 直接
 * 操作 LVGL 对象，会与 AppTask 的 lv_timer_handler() 产生并发访问。
 * 因此数据生产者仅负责“发布快照 + 通知事件”，所有 WeatherLVGL_*()
 * 都只允许在 AppTask 中调用。
 *
 * 四、与旧工程相比有什么变化？
 * ============================================================================
 * - 不再提交 WeatherUI_DrawXXX() 老式 LCD Job；原 weather_UI.c 保留在工程，
 *   但不由本文件调用。真正传输只由 LVGL 的 flush_cb 提交。
 * - LCD / AHT20 / RTC / NetworkTask / Console 的底层代码保持不变。
 * - 开机先显示 LVGL 欢迎页，再经过固定的展示时间切换到天气时钟主页。
 * - 增加 LVGL 的 Tick 推进与处理周期，动画与刷新不会依赖 Sensor/Network 事件。
 * - 保留 indexed notification 0 的应用事件；index 1、2 仍由 LCD 层使用。
 * - 保留每 60 秒的任务栈、FreeRTOS Heap 诊断，并增加 LVGL 内存池诊断。
 *
 * 五、先决条件
 * ============================================================================
 * 1. main.c 已在 App_Init() 之前成功调用 ILI9341_RTOS_Init()。
 * 2. 工程已添加 LVGL 8.3.11 及配置文件 lv_conf.h。
 * 3. 工程已添加 lvgl_port_disp.c 和 weather_lvgl_ui.c，以及对应头文件。
 * 4. 当前 FreeRTOSConfig.h 中 configTICK_RATE_HZ 为 1000；详见下方编译检查。
 * 5. 本文件由原来的 app.c **整文件替换**，不要在 Keil 中重复添加 app.c。
 *
 * 六、重要限制
 * ============================================================================
 * 本版必须先经过 Keil 编译和开发板实测才能称为完整集成；请重点观察：
 *     - 开机画面是否出现并正常进入主页；
 *     - DMA 刷新是否出现花屏、断续、卡死；
 *     - AppTask 最小剩余栈、LVGL 内存池的剩余量；
 *     - Wi-Fi 暂不可用时是否依然能进入主页、显示 RTC 与室内数据。
 */

/* ============================================================================
 * 01. 所依赖的头文件
 * ============================================================================
 */
#include "app.h"                 /* App_Status_t / App_Init() / App_NotifyEvent() */

#include <stddef.h>              /* NULL、size_t */
#include <stdint.h>              /* uint8_t、uint32_t、int32_t */

#include "FreeRTOS.h"            /* TickType_t、堆信息、pdMS_TO_TICKS 等 */
#include "task.h"                /* 创建任务、取 Tick、索引任务通知、栈监测 */

#include "lvgl.h"                /* LVGL 初始化、Tick、定时器、内存监测 */
#include "lvgl_port_disp.h"      /* LVGL_PortDisp_Init()：ILI9341 显示适配 */
#include "weather_lvgl_ui.h"     /* Boot/Home UI 以及三个数据更新接口 */

#include "rtc.h"                 /* RTC_DateTime_t / RTC_Driver_GetDateTime() */
#include "aht20.h"               /* AHT20_Measurement_t */
#include "sensor_task.h"         /* AHT20 最新数据快照 */
#include "network_task.h"        /* 网络任务初始化和天气快照 */
#include "console_log.h"         /* 线程安全串口日志 */

/* ============================================================================
 * 02. 编译期约束及参数
 * ============================================================================
 */

/*
 * 当前工程 FreeRTOS Tick 为 1 ms（1000 Hz）。
 * 下面将 FreeRTOS Tick 差值直接传给 LVGL 的 lv_tick_inc()，因此必须
 * 明确检查其单位。如果以后将内核 Tick 改为 100 Hz，不能直接照用。
 * 这样比“悄悄把 1 Tick 当作 1 ms”更安全。
 */
#if (configTICK_RATE_HZ != 1000)
#error "app.c LVGL tick integration requires configTICK_RATE_HZ == 1000"
#endif

/*
 * 原 AppTask 为 512 words（约 2 KB）。接入 LVGL 后，同一任务会调用
 * lv_timer_handler() -> 绘图 -> flush_cb -> LCD Job 等待。
 *
 * 增加至 1536 words（约 6 KB）：这是初始工程配置，不代表已经足够；
 * 实际是否足够必须观察 uxTaskGetStackHighWaterMark() 的最小余量。
 * 栈由 FreeRTOS heap_4 分配，后续需要同时监测 FreeRTOS Heap 余量。
 */
#define APP_TASK_STACK_DEPTH                 1536U

/* 与原工程一致：AppTask 比 LCD Service 低一级优先级，后者负责执行 Job。 */
#define APP_TASK_PRIORITY                    (tskIDLE_PRIORITY + 1U)

/* Indexed Notification：业务事件 index=0，LCD Job=1，DMA=2。 */
#define APP_NOTIFY_INDEX                     0U

/* 当前应用只识别这两个事件位，拒绝未知位。 */
#define APP_EVENT_ALL                         \
    (APP_EVENT_SENSOR_UPDATED | APP_EVENT_WEATHER_UPDATED)

/*
 * LVGL 最长每 10ms 获得一次运行机会。
 * 如果不设置这个上限，AppTask 可能阻塞一秒等 RTC，到时开机动画会卡顿。
 * 这个限制只控制任务的“最大休眠时间”，不保证 SPI DMA 一定每 10ms 完成。
 */
#define APP_LVGL_MAX_SLEEP_MS                 10U

/* 欢迎页面展示 2500 ms，然后切主页；不等待 Wi-Fi 联网成功。 */
#define APP_BOOT_SHOW_MS                      2500U

/* RTC 每秒检查一次，显示实际 RTC 快照而不是人为累加秒数。 */
#define APP_RTC_PERIOD_MS                     1000U

/* 事件丢失时，用此周期兜底读取最新 AHT20 快照。 */
#define APP_SENSOR_POLL_MS                    2000U

/* 天气数据过期时若没有新通知，也要定期检查失效状态。 */
#define APP_WEATHER_POLL_MS                   30000U

/* 每分钟打印四个任务的栈水位及两类堆信息。 */
#define APP_MONITOR_PERIOD_MS                 60000U

/* 剩余不足 64 个 StackType_t 时打印警告。 */
#define APP_STACK_WARNING_WORDS               64U

/* 日志开关：0=不逐秒打印 RTC，减少 USART1 轮询日志负担。 */
#define APP_LOG_RTC_EVERY_SECOND              0U

/* 应用任务句柄：创建一次，后续仅由其他任务读取用于发送通知。 */
static TaskHandle_t s_app_task_handle = NULL;

/*
 * LVGL 初始化失败代码（供 Keil Watch 观察）：
 * 0：成功；1：显示驱动注册失败；2：UI 对象创建失败。
 * volatile 仅为调试观察，不能代替同步机制。
 */
static volatile uint32_t s_lvgl_startup_error = 0UL;

/* ============================================================================
 * LVGL 首帧异常诊断变量（Keil Watch 可直接查看）
 * ============================================================================
 * 调试目的：区分“没有进入渲染”“进入渲染但没有返回”“已返回但下一步卡住”。
 *
 * g_lvgl_trace_stage：
 *   0：尚未进入第一次 LVGL 渲染；
 *  10：正在执行 lv_timer_handler()，或等待其内部的 LCD Job；
 *  11：lv_timer_handler() 已返回。
 *
 * 每次进入和退出的次数分别记录，二者相差 1 表示某次渲染尚未返回。
 * 注意：这些变量只是观测点，不能单凭 stage=10 断言是 SPI DMA 故障。
 */
volatile uint32_t g_lvgl_trace_stage = 0UL;
volatile uint32_t g_lvgl_handler_enter_count = 0UL;
volatile uint32_t g_lvgl_handler_exit_count = 0UL;
volatile uint32_t g_lvgl_handler_last_duration_ms = 0UL;
volatile uint32_t g_lvgl_handler_max_duration_ms = 0UL;


/* ============================================================================
 * 03. 私有工具函数原型
 * ============================================================================
 */
static void prvAppTask(void *argument);
static TickType_t prvTicksFromMs(uint32_t ms);
static TickType_t prvTicksRemaining(TickType_t now,
                                     TickType_t last,
                                     TickType_t period);
static uint8_t prvSameRTC(const RTC_DateTime_t *a, const RTC_DateTime_t *b);
static int32_t prvRoundedTenths(float value);
static uint8_t prvSameIndoor(const AHT20_Measurement_t *a,
                              const AHT20_Measurement_t *b);
static uint8_t prvSameWeather(const WeatherData_t *a, const WeatherData_t *b);
static void prvUpdateRTC(RTC_DateTime_t *last, uint8_t *has_last);
static void prvUpdateIndoor(AHT20_Measurement_t *last, uint8_t *has_last);

typedef struct
{
    WeatherData_t data;          /* 室外天气快照，只在 valid=1 时有效。 */
    uint8_t valid;               /* 1：有可用天气；0：无数据或已过期。 */
} App_WeatherSnapshot_t;

static void prvUpdateOutdoor(App_WeatherSnapshot_t *last, uint8_t *has_last);
static void prvUpdateUiStatus(void);
static void prvPrintStackWatermark(const char *task_name);
static void prvPrintResources(uint8_t lvgl_ready);

/* ============================================================================
 * 04. 对外接口：其他任务通知 AppTask 有新数据
 * ============================================================================
 */
/**
 * @brief 从普通任务上下文向 AppTask 发送业务事件。
 *
 * @param event_bits APP_EVENT_SENSOR_UPDATED、APP_EVENT_WEATHER_UPDATED，
 *                   或它们的按位或组合。
 *
 * @details
 * 发送的是“有新数据”的信号，不直接传递业务结构体：
 *
 *   SensorTask：先 xQueueOverwrite() 保存 AHT20 数据，再调用这里；
 *   NetworkTask：先保存完整天气快照，再调用这里；
 *   AppTask：醒来后从真正的数据拥有者那里复制最新快照。
 *
 * 为什么使用 eSetBits？
 *   它会把相同类型事件合并：多次温湿度更新只需要处理最新值，不需要为每条
 *   消息排队，否则屏幕可能一直追赶过时的天气数据。
 *
 * 注意：此函数不能在 ISR 中调用；ISR 应使用 xTaskNotify...FromISR()。
 *       Index 0 不会与 LCD 完成通知使用的 Index 1 发生混淆。
 */
void App_NotifyEvent(uint32_t event_bits)
{
    TaskHandle_t handle;

    event_bits &= APP_EVENT_ALL; /* 清除当前应用未定义的事件位。 */
    if (event_bits == 0UL)
    {
        return;
    }

    handle = s_app_task_handle;
    if (handle == NULL)
    {
        /* 调度器启动前句柄尚未建立时，不对空句柄发送任务通知。 */
        return;
    }

    (void)xTaskNotifyIndexed(handle,
                             APP_NOTIFY_INDEX,
                             event_bits,
                             eSetBits);
}

/* ============================================================================
 * 05. Tick 调度小工具
 * ============================================================================
 */
/**
 * @brief 毫秒转换为 FreeRTOS Tick；最少返回 1 Tick，防止忙循环。
 */
static TickType_t prvTicksFromMs(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks == (TickType_t)0) ? (TickType_t)1 : ticks;
}

/**
 * @brief 距离周期任务下一次执行还有多少 Tick。
 *
 * @param now 当前 Tick。
 * @param last 上一次周期的固定时间基准。
 * @param period 周期 Tick 数量，必须大于零。
 *
 * @return 0 表示已经到期；非零表示还可以休眠多久。
 *
 * 原理：TickType_t 是无符号整数，(now - last) 可在正常 Tick 回绕时保持
 * 短时间间隔的正确性。本项目所有周期远短于 32 位 Tick 的回绕周期。
 */
static TickType_t prvTicksRemaining(TickType_t now,
                                     TickType_t last,
                                     TickType_t period)
{
    TickType_t elapsed = (TickType_t)(now - last);
    return (elapsed >= period) ? (TickType_t)0 : (TickType_t)(period - elapsed);
}

/* ============================================================================
 * 06. 数据变化判断：减少无效 LVGL 更新
 * ============================================================================
 */
/**
 * @brief 按 RTC 有意义的字段比较日期时间。
 *
 * 不直接对整个结构体使用 memcmp()，因为结构体填充字节可能不一致。
 */
static uint8_t prvSameRTC(const RTC_DateTime_t *a, const RTC_DateTime_t *b)
{
    if ((a == NULL) || (b == NULL)) return 0U;

    return ((a->year == b->year) &&
            (a->month == b->month) &&
            (a->day == b->day) &&
            (a->weekday == b->weekday) &&
            (a->hour == b->hour) &&
            (a->minute == b->minute) &&
            (a->second == b->second)) ? 1U : 0U;
}

/**
 * @brief 将浮点数据按 UI 的格式化规则四舍五入到整数十分位。
 *
 * 例如：26.54 -> 265，26.56 -> 266，-2.76 -> -28。
 * 与 weather_lvgl_ui.c 内的 prvFormatOneDecimal() 使用同一舍入方法，
 * 防止显示已从 26.5 变为 26.6，但 AppTask 仍误判成“内容没有变化”。
 * 超出合理显示范围（包括 NaN）时，返回 INT32_MIN 表示无效占位。
 */
static int32_t prvRoundedTenths(float value)
{
    if (!((value >= -100.0f) && (value <= 150.0f)))
    {
        return (-2147483647L - 1L);
    }
    return (int32_t)(value * 10.0f + ((value >= 0.0f) ? 0.5f : -0.5f));
}

/**
 * @brief 判断室内温湿度显示值是否变化。
 *
 * UI 只显示小数点后一位，所以这里比较按显示规则舍入的整数十分位，
 * 避免传感器微小噪声导致同一个数字反复触发 LVGL 重绘。
 */
static uint8_t prvSameIndoor(const AHT20_Measurement_t *a,
                              const AHT20_Measurement_t *b)
{
    int32_t ta, tb, ha, hb;

    if ((a == NULL) || (b == NULL)) return 0U;

    ta = prvRoundedTenths(a->temperature_celsius);
    tb = prvRoundedTenths(b->temperature_celsius);
    ha = prvRoundedTenths(a->humidity_percent);
    hb = prvRoundedTenths(b->humidity_percent);

    return ((ta == tb) && (ha == hb)) ? 1U : 0U;
}

/**
 * @brief 判断天气快照对应的 UI 数据是否发生变化。
 *
 * 只比较实际用到的字段；不比较时间戳、无意义的结构体填充字节。
 * 断网时有效标志是否变化在调用本函数之前由 prvUpdateOutdoor() 判断。
 */
static uint8_t prvSameWeather(const WeatherData_t *a, const WeatherData_t *b)
{
    if ((a == NULL) || (b == NULL)) return 0U;

    return ((prvRoundedTenths(a->temperature) ==
             prvRoundedTenths(b->temperature)) &&
            ((int32_t)(a->wind_speed + 0.5f) ==
             (int32_t)(b->wind_speed + 0.5f)) &&
            (a->humidity == b->humidity) &&
            (a->weather_code == b->weather_code) &&
            (a->is_day == b->is_day)) ? 1U : 0U;
}

/* ============================================================================
 * 07. RTC 业务：由 AppTask 获取时间快照 -> 更新 LVGL 时钟控件
 * ============================================================================
 */
/**
 * @brief 读取 RTC 并在数据变化时更新 LVGL 数字时钟。
 *
 * 注意：本函数只在 AppTask 中调用。不访问原 weather_UI.c/GFX，也不提交
 * 老式 LCD Job；真正像素输出由未来的 lv_timer_handler() 自动触发。
 *
 * 为什么只在成功更新后缓存 last？
 *   RTC 读取失败时不能把未初始化的时间当成最新显示状态。
 */
static void prvUpdateRTC(RTC_DateTime_t *last, uint8_t *has_last)
{
    RTC_DateTime_t current;
    RTC_Status_t status;

    if ((last == NULL) || (has_last == NULL)) return;

    status = RTC_Driver_GetDateTime(&current);
    if (status != RTC_STATUS_OK)
    {
        ConsoleLog_Printf("[APP][RTC][ERR] GetDateTime=%d\r\n", (int)status);
        return;
    }

    if ((*has_last != 0U) && (prvSameRTC(&current, last) != 0U))
    {
        return; /* 时钟秒数等字段没变，避免重复更新控件。 */
    }

#if (APP_LOG_RTC_EVERY_SECOND != 0U)
    ConsoleLog_Printf("[RTC] %04u-%02u-%02u %02u:%02u:%02u\r\n",
                      (unsigned int)current.year,
                      (unsigned int)current.month,
                      (unsigned int)current.day,
                      (unsigned int)current.hour,
                      (unsigned int)current.minute,
                      (unsigned int)current.second);
#endif

    WeatherLVGL_UpdateTime(&current); /* 只修改 LVGL 控件，不立即进行 DMA。 */
    *last = current;
    *has_last = 1U;
}

/* ============================================================================
 * 08. 室内业务：从 SensorTask 长度为 1 的队列读取最新温湿度
 * ============================================================================
 */
/**
 * @brief 根据 SensorTask 的最新快照更新 LVGL 室内温湿度卡片。
 *
 * SENSOR_TASK_NO_DATA 是开机时的正常现象；界面会保留 "--.- C" 占位。
 * 其他状态说明快照接口出现异常，打印一次错误信息交给串口排查。
 */
static void prvUpdateIndoor(AHT20_Measurement_t *last, uint8_t *has_last)
{
    AHT20_Measurement_t current;
    SensorTask_Status_t status;

    if ((last == NULL) || (has_last == NULL)) return;

    status = SensorTask_GetLatestMeasurement(&current);
    if (status == SENSOR_TASK_NO_DATA)
    {
        if (*has_last == 0U)
        {
            WeatherLVGL_UpdateIndoor(NULL, 0U);
        }
        return;
    }

    if (status != SENSOR_TASK_OK)
    {
        ConsoleLog_Printf("[APP][AHT20][ERR] Snapshot=%d\r\n", (int)status);
        return;
    }

    if ((*has_last != 0U) && (prvSameIndoor(&current, last) != 0U))
    {
        return;
    }

    WeatherLVGL_UpdateIndoor(&current, 1U);
    *last = current;
    *has_last = 1U;
}

/* ============================================================================
 * 09. 室外业务：从 NetworkTask 获取有效快照，不直接发送 AT 指令
 * ============================================================================
 */
/**
 * @brief 网络天气已更新或到了 30 秒兜底时间时，更新室外卡片。
 *
 * @param last 上次已经交给 LVGL 的天气快照与有效状态。
 * @param has_last 是否至少做过一次天气有效性更新。
 *
 * 数据新鲜度由 NetworkTask_GetLatestWeather() 判定，超龄则返回无效；
 * 不允许为了让屏幕有数据而继续显示过期天气作为当前实时值。
 * Wi-Fi 断开但缓存尚在有效期内，允许继续显示最近一次有效天气。
 */
static void prvUpdateOutdoor(App_WeatherSnapshot_t *last, uint8_t *has_last)
{
    App_WeatherSnapshot_t current = {{0}, 0U};

    if ((last == NULL) || (has_last == NULL)) return;

    current.valid = NetworkTask_GetLatestWeather(&current.data, NULL);

    if ((*has_last != 0U) && (current.valid == last->valid))
    {
        if (current.valid == 0U)
        {
            return; /* 都没有数据，不重复写同一串占位字符。 */
        }
        if (prvSameWeather(&current.data, &last->data) != 0U)
        {
            return; /* 新天气的显示内容与上次相同。 */
        }
    }

    if (current.valid != 0U)
    {
        WeatherLVGL_UpdateOutdoor(&current.data, 1U);
        ConsoleLog_Printf("[APP] Weather UI updated: code=%u\r\n",
                          (unsigned int)current.data.weather_code);
    }
    else
    {
        WeatherLVGL_UpdateOutdoor(NULL, 0U);
        ConsoleLog_Printf("[APP] Weather cache unavailable/expired\r\n");
    }

    *last = current;
    *has_last = 1U;
}

/**
 * @brief 给 V3 顶栏提供真实 Wi-Fi / 天气缓存 / SNTP 三项状态。
 *
 * 只在 AppTask 调用。网络快照通过 NetworkTask_GetUiStatus() 原子读取，
 * 天气数据是否新鲜仍然由现有的 NetworkTask_GetLatestWeather() 判断。
 * 不从 AppTask 直接处理 ESP-AT 接收状态，不向 LCD Driver 直接写像素。
 *
 * 每秒查询一次，但 WeatherLVGL_UpdateStatus 会先比较状态位；
 * 状态未发生变化时，不会重新绘制状态图标。
 */
static void prvUpdateUiStatus(void)
{
    NetworkTask_UiStatus_t net = {0};
    WeatherData_t unused_weather;
    uint8_t weather_fresh;

    (void)NetworkTask_GetUiStatus(&net);
    weather_fresh = NetworkTask_GetLatestWeather(&unused_weather, NULL);
    WeatherLVGL_UpdateStatus(net.wifi_has_ip, weather_fresh, net.sntp_synced);
}

/* ============================================================================
 * 10. 运行资源诊断：四个任务的栈 / FreeRTOS Heap / LVGL Heap
 * ============================================================================
 */
/**
 * @brief 打印某个任务的历史最小剩余栈（High Water Mark）。
 *
 * FreeRTOS 返回的是 StackType_t 个数而不是字节。
 * 这里只进行诊断，不使用栈水位作为程序是否安全运行的唯一依据。
 */
static void prvPrintStackWatermark(const char *task_name)
{
    TaskHandle_t handle;
    UBaseType_t words;

    if (task_name == NULL) return;

    handle = xTaskGetHandle(task_name);
    if (handle == NULL)
    {
        ConsoleLog_Printf("[RTOS][WARN] No task: %s\r\n", task_name);
        return;
    }

    words = uxTaskGetStackHighWaterMark(handle);

    ConsoleLog_Printf((words < APP_STACK_WARNING_WORDS) ?
                      "[RTOS][WARN] %s stack-free=%lu words (%lu B)\r\n" :
                      "[RTOS] %s stack-free=%lu words (%lu B)\r\n",
                      task_name,
                      (unsigned long)words,
                      (unsigned long)words * (unsigned long)sizeof(StackType_t));
}

/**
 * @brief 每 60 秒输出各类剩余内存和任务栈水位，方便实际硬件验证。
 *
 * FreeRTOS Heap 和 LVGL Heap 是两套不同的分配器：
 *   1. FreeRTOS heap_4.c 管理任务栈、TCB、Queue、Mutex 等；
 *   2. LV_MEM_CUSTOM=0 时，LVGL 使用 lv_conf.h 中 32KB 的独立内存池
 *      管理控件、标签字符串、定时器和图形内部结构。
 *
 * 单独看 xPortGetFreeHeapSize() 不足以判断 LVGL 是否耗尽了内存。
 */
static void prvPrintResources(uint8_t lvgl_ready)
{
    size_t free_heap;
    size_t min_heap;

    prvPrintStackWatermark("AppTask");
    prvPrintStackWatermark("SensorTask");
    prvPrintStackWatermark("NetworkTask");
    prvPrintStackWatermark("lcd_svc");

    free_heap = xPortGetFreeHeapSize();
    min_heap = xPortGetMinimumEverFreeHeapSize();

    ConsoleLog_Printf("[RTOS] Heap free=%lu B, min-ever=%lu B\r\n",
                      (unsigned long)free_heap,
                      (unsigned long)min_heap);

    if (lvgl_ready != 0U)
    {
        lv_mem_monitor_t monitor;

        /* LVGL 对象及定时器均由 AppTask 独占，因此在本任务读内存统计。 */
        lv_mem_monitor(&monitor);
        ConsoleLog_Printf("[LVGL] Memory used=%u%%, free=%lu B, biggest=%lu B, frag=%u%%\r\n",
                          (unsigned int)monitor.used_pct,
                          (unsigned long)monitor.free_size,
                          (unsigned long)monitor.free_biggest_size,
                          (unsigned int)monitor.frag_pct);
    }
}

/* ============================================================================
 * 11. AppTask 入口：初始化 + Boot + Home + 周期维护
 * ============================================================================
 */
/**
 * @brief 应用主任务：本工程唯一允许修改 LVGL GUI 对象的任务。
 *
 * @param argument 创建任务时附带的用户参数；当前为 NULL。
 *
 * 初始化阶段：
 *   1. lv_init() 初始化 LVGL 软件库；
 *   2. LVGL_PortDisp_Init() 注册 320x240 显示驱动，内部委托 LCD Service；
 *   3. WeatherLVGL_Init() 创建欢迎页和正式主界面，先显示欢迎页；
 *   4. 建立 Tick 基准，准备进入循环；
 *
 * 主循环阶段：
 *   A. 按 FreeRTOS Tick 差值推进 LVGL 内部时间；
 *   B. 每秒读取真实 RTC，每 2 秒检查 AHT20，每 30 秒检查天气过期；
 *   C. 欢迎页显示 2.5 秒后切主页；
 *   D. 调用 lv_timer_handler() 驱动绘制、动画和 flush_cb；
 *   E. 每分钟输出内存和栈诊断；
 *   F. 最多阻塞 10 ms 等业务通知，再从 A 开始。
 *
 * 由于 xTaskNotifyWaitIndexed() 可因数据事件提前唤醒，所以 Sensor/Weather
 * 更新不需要等 RTC 下一个秒边界；也不会干扰 RTC 的固定周期检查。
 */
static void prvAppTask(void *argument)
{
    /* LVGL 是否完成软件、显示设备和 UI 三段初始化。 */
    uint8_t lvgl_ready = 0U;

    /* 只有首次满足 Boot 展示时间才切到 Home。 */
    uint8_t home_shown = 0U;

    /* 用于判断业务快照是否需要更新 LVGL 控件。 */
    RTC_DateTime_t last_time = {0};
    AHT20_Measurement_t last_indoor = {0};
    App_WeatherSnapshot_t last_weather = {{0}, 0U};
    uint8_t has_time = 0U;
    uint8_t has_indoor = 0U;
    uint8_t has_weather = 0U;

    /* 保存 FreeRTOS 当前 Tick，以及不同业务的周期时间基准。 */
    TickType_t now;
    TickType_t lvgl_last_tick;
    TickType_t boot_start_tick;
    TickType_t rtc_last_tick;
    TickType_t sensor_last_tick;
    TickType_t weather_last_tick;
    TickType_t monitor_last_tick;
    TickType_t wait_ticks;
    TickType_t remaining;

    /* LVGL Tick 更新时用无符号差值；1000Hz 即 1 Tick = 1ms。 */
    TickType_t elapsed_lvgl_ticks;

    /* 业务事件通过通知索引 0 传递，保留事件位直到本任务完成处理。 */
    uint32_t pending_events = APP_EVENT_SENSOR_UPDATED | APP_EVENT_WEATHER_UPDATED;
    uint32_t received_events = 0UL;

    /* 这些常量在当前 FreeRTOS 调度参数下均为非零 Tick。 */
    const TickType_t rtc_period = prvTicksFromMs(APP_RTC_PERIOD_MS);
    const TickType_t sensor_period = prvTicksFromMs(APP_SENSOR_POLL_MS);
    const TickType_t weather_period = prvTicksFromMs(APP_WEATHER_POLL_MS);
    const TickType_t monitor_period = prvTicksFromMs(APP_MONITOR_PERIOD_MS);
    const TickType_t boot_period = prvTicksFromMs(APP_BOOT_SHOW_MS);
    const TickType_t max_lvgl_sleep = prvTicksFromMs(APP_LVGL_MAX_SLEEP_MS);

    (void)argument;

    /* ------------------------------------------------------------------------
     * 11.1 初始化 LVGL 库
     * ------------------------------------------------------------------------
     * main.c 已调用 ILI9341_RTOS_Init()，同时 scheduler 此时已启动。
     * LVGL 只能在拥有它的 AppTask 初始化一次，不在 ISR / 其他任务重复初始化。
     */
    lv_init();
    ConsoleLog_Printf("[APP][LVGL] lv_init OK\r\n");

    /* ------------------------------------------------------------------------
     * 11.2 注册 LCD 显示驱动（内部经 LCD Service 设置旋转、注册 flush_cb）
     * ------------------------------------------------------------------------
     */
    if (LVGL_PortDisp_Init() == 0U)
    {
        s_lvgl_startup_error = 1UL;
        ConsoleLog_Printf("[APP][LVGL][ERROR] Display port init failed\r\n");
    }
    else if (WeatherLVGL_Init() == 0U)
    {
        s_lvgl_startup_error = 2UL;
        ConsoleLog_Printf("[APP][LVGL][ERROR] UI creation failed\r\n");
    }
    else
    {
        lvgl_ready = 1U;
        s_lvgl_startup_error = 0UL;
        ConsoleLog_Printf("[APP][LVGL] Boot UI created; LCD flush is ready\r\n");
    }

    /* ------------------------------------------------------------------------
     * 11.3 时间基准
     * ------------------------------------------------------------------------
     * 当前 Tick 是“调度器运行后的计数”。
     * LVGL 的软件时基初始为 0；从现在开始，每轮通过 lv_tick_inc(差值)
     * 累加真实经过的毫秒。不需要修改 SysTick_Handler 或 FreeRTOS Tick Hook。
     */
    now = xTaskGetTickCount();
    boot_start_tick = now;
    lvgl_last_tick = now;

    /* 让 RTC 第一次检查立即执行；后续保持固定 1 秒的节奏。 */
    rtc_last_tick = (TickType_t)(now - rtc_period);

    /* 室内、室外的第一次检查由 pending_events 触发；之后靠兜底周期。 */
    sensor_last_tick = now;
    weather_last_tick = now;
    monitor_last_tick = now;

    ConsoleLog_Printf("[APP] LVGL scheduler started, boot=%u ms\r\n",
                      (unsigned int)APP_BOOT_SHOW_MS);

    /* ------------------------------------------------------------------------
     * 11.4 主循环：始终保留业务任务和诊断，不依赖网络在线
     * ------------------------------------------------------------------------
     */
    for (;;)
    {
        now = xTaskGetTickCount();

        /* 1) 更新 LVGL 内部毫秒数（独立于业务数据事件）。
         *
         * 如果本轮刚执行了整屏首帧的多次 SPI DMA flush，可能经过几十毫秒；
         * 下一轮只需把真正经过的 Tick 一次性补进 LVGL，即可让动画不长期漂移。
         *
         * LVGL Tick 与 FreeRTOS Tick 是不同计时器：
         * FreeRTOS Tick 驱动任务调度；LVGL Tick 决定动画/绘制定时器何时到期。
         */
        if (lvgl_ready != 0U)
        {
            elapsed_lvgl_ticks = (TickType_t)(now - lvgl_last_tick);
            if (elapsed_lvgl_ticks != (TickType_t)0)
            {
                lv_tick_inc((uint32_t)elapsed_lvgl_ticks);
            }
        }
        lvgl_last_tick = now;

        /* 2) RTC 定期读取。长期 SPI 传输使任务晚醒时，跳过旧的周期点，
         *    但不重新播过去每一秒；显示真实 RTC 当前时刻。 */
        if ((TickType_t)(now - rtc_last_tick) >= rtc_period)
        {
            rtc_last_tick +=
                ((TickType_t)(now - rtc_last_tick) / rtc_period) * rtc_period;

            if (lvgl_ready != 0U)
            {
                prvUpdateRTC(&last_time, &has_time);
                /* RTC 任务每秒运行一次；同时刷新三枚真实状态图标。
                 * NetworkTask 异常或尚未启动均不会阻塞本地时钟。 */
                prvUpdateUiStatus();
            }
        }

        /* 3) 室内：事件优先、周期兜底。仅由 AppTask 调用 WeatherLVGL API。 */
        now = xTaskGetTickCount();
        if (((pending_events & APP_EVENT_SENSOR_UPDATED) != 0UL) ||
            ((TickType_t)(now - sensor_last_tick) >= sensor_period))
        {
            pending_events &= ~APP_EVENT_SENSOR_UPDATED;
            sensor_last_tick = now;

            if (lvgl_ready != 0U)
            {
                prvUpdateIndoor(&last_indoor, &has_indoor);
            }
        }

        /* 4) 室外：天气快照的新数据通知或 30 秒有效性兜底检查。 */
        now = xTaskGetTickCount();
        if (((pending_events & APP_EVENT_WEATHER_UPDATED) != 0UL) ||
            ((TickType_t)(now - weather_last_tick) >= weather_period))
        {
            pending_events &= ~APP_EVENT_WEATHER_UPDATED;
            weather_last_tick = now;

            if (lvgl_ready != 0U)
            {
                prvUpdateOutdoor(&last_weather, &has_weather);
            }
        }

        /* 5) 欢迎页展示时间满足后切换到主页；
         *    不等待 Wi-Fi 和网络天气，以免联网失败时永远卡在启动页面。 */
        now = xTaskGetTickCount();
        if ((lvgl_ready != 0U) && (home_shown == 0U) &&
            ((TickType_t)(now - boot_start_tick) >= boot_period))
        {
            WeatherLVGL_ShowHome();
            home_shown = 1U;
            ConsoleLog_Printf("[APP][LVGL] Home screen activated\r\n");
        }

        /* 6) 这是所有 LVGL 界面、动画和刷新回调真正运行的入口。
         *    LVGL 会将所有变化的对象汇总成失效区域，按 10 行缓冲区分块，
         *    在 flush_cb 中使用 LCD Service -> SPI DMA 更新屏幕。
         *
         *    必须在 AppTask 中执行，不能交给 LCD Service 再调用 LVGL。
         */
        if (lvgl_ready != 0U)
        {
            /* ------------------------------------------------------------
             * 诊断点：LVGL 首帧绘制及 DMA 刷新
             * ------------------------------------------------------------
             * lv_timer_handler() 内部不仅进行 UI 绘制，还会间接调用：
             *   flush_cb -> ILI9341_RTOS_Execute() -> LCD Service -> SPI DMA
             *
             * 它既可能正常返回，也可能因为底层异常被永久阻塞。
             * 因此我们在调用前后分别放一条观察标记。
             *
             * 首两轮打印完整的进入/退出日志；后续每 1000 次再报告一次，
             * 避免每 10 ms 打印串口导致系统被日志本身拖慢。
             */
            TickType_t handler_start_tick;
            TickType_t handler_elapsed_ticks;
            uint32_t call_id;

            handler_start_tick = xTaskGetTickCount();
            call_id = ++g_lvgl_handler_enter_count;
            g_lvgl_trace_stage = 10UL;

            if ((call_id <= 2UL) || ((call_id % 1000UL) == 0UL))
            {
                ConsoleLog_Printf(
                    "[APP][LVGL][TRACE] handler ENTER #%lu\r\n",
                    (unsigned long)call_id
                );
            }

            /* 真正进入 LVGL：如果执行被卡住，下面的 EXIT 日志就不会出现。 */
            (void)lv_timer_handler();

            g_lvgl_trace_stage = 11UL;
            ++g_lvgl_handler_exit_count;

            handler_elapsed_ticks =
                (TickType_t)(xTaskGetTickCount() - handler_start_tick);

            /* 本工程 configTICK_RATE_HZ = 1000，即 1 Tick = 1 ms。 */
            g_lvgl_handler_last_duration_ms =
                (uint32_t)handler_elapsed_ticks;

            if ((uint32_t)handler_elapsed_ticks >
                g_lvgl_handler_max_duration_ms)
            {
                g_lvgl_handler_max_duration_ms =
                    (uint32_t)handler_elapsed_ticks;
            }

            /* 超过 250 ms 的刷新额外记录耗时，便于识别严重的绘制延迟。 */
            if ((call_id <= 2UL) ||
                ((call_id % 1000UL) == 0UL) ||
                (handler_elapsed_ticks >= pdMS_TO_TICKS(250U)))
            {
                ConsoleLog_Printf(
                    "[APP][LVGL][TRACE] handler EXIT  #%lu, cost=%lu ms\r\n",
                    (unsigned long)call_id,
                    (unsigned long)handler_elapsed_ticks
                );
            }
        }

        /* 7) 每 60 秒打印资源信息。
         *    UI 初始化和首帧渲染之后，LVGL 的内存占用可能明显增大；
         *    lv_mem_monitor 可以帮助检查 32KB LVGL 内存池是否足够。 */
        now = xTaskGetTickCount();
        if ((TickType_t)(now - monitor_last_tick) >= monitor_period)
        {
            monitor_last_tick = now;
            prvPrintResources(lvgl_ready);
            if (has_time != 0U)
            {
                ConsoleLog_Printf("[APP][RTC] local=%04u-%02u-%02u %02u:%02u:%02u\r\n",
                    (unsigned int)last_time.year,
                    (unsigned int)last_time.month,
                    (unsigned int)last_time.day,
                    (unsigned int)last_time.hour,
                    (unsigned int)last_time.minute,
                    (unsigned int)last_time.second);
            }
            else
            {
                ConsoleLog_Printf("[APP][RTC][WARN] No valid RTC snapshot yet\r\n");
            }
        }

        /* 8) 下次任务等待时长：先取 LVGL 最大休眠，再与其他周期比较。
         *    即使传感器、网络均不更新，也最多等 10ms，动画可继续运行。
         *    这是 FreeRTOS 阻塞等待，不是 while 忙轮询。 */
        now = xTaskGetTickCount();
        wait_ticks = (lvgl_ready != 0U) ? max_lvgl_sleep : rtc_period;

        remaining = prvTicksRemaining(now, rtc_last_tick, rtc_period);
        if (remaining < wait_ticks) wait_ticks = remaining;

        remaining = prvTicksRemaining(now, sensor_last_tick, sensor_period);
        if (remaining < wait_ticks) wait_ticks = remaining;

        remaining = prvTicksRemaining(now, weather_last_tick, weather_period);
        if (remaining < wait_ticks) wait_ticks = remaining;

        remaining = prvTicksRemaining(now, monitor_last_tick, monitor_period);
        if (remaining < wait_ticks) wait_ticks = remaining;

        if ((lvgl_ready != 0U) && (home_shown == 0U))
        {
            remaining = prvTicksRemaining(now, boot_start_tick, boot_period);
            if (remaining < wait_ticks) wait_ticks = remaining;
        }

        if (wait_ticks == (TickType_t)0)
        {
            /* 某个定时工作已到期：马上进入下一轮，不丢掉时间事件。 */
            continue;
        }

        /* 9) 等业务通知或下一次定时到期。
         *    注意 xTaskNotifyWaitIndexed 下标必须为 0；不能与 LCD Job index 1、
         *    DMA index 2 混用。等待提前被业务事件打断，不会重置 RTC 基准。 */
        received_events = 0UL;
        if (xTaskNotifyWaitIndexed(APP_NOTIFY_INDEX,
                                   0UL,          /* 等待前不清除已有事件位 */
                                   APP_EVENT_ALL,/* 读出之后清除本轮取到的事件位 */
                                   &received_events,
                                   wait_ticks) == pdTRUE)
        {
            pending_events |= (received_events & APP_EVENT_ALL);
        }
    }
}

/* ============================================================================
 * 12. App_Init()：主调度器启动前创建三个业务任务
 * ============================================================================
 */
/**
 * @brief 创建 AppTask / SensorTask / NetworkTask，保持旧 main.c 的调用方式。
 *
 * @return APP_OK：全部任务创建成功；APP_ERROR_TASK_CREATE：某个任务创建失败。
 *
 * main.c 中的启动顺序应保持：
 *   board_init() -> RTC_Driver_Init() -> ILI9341_RTOS_Init() -> App_Init()
 *   -> vTaskStartScheduler()
 *
 * AppTask 句柄在 xTaskCreate() 成功后写入 s_app_task_handle。
 * 网络和传感器任务即使比 AppTask 优先级高，也不会在 scheduler 之前运行，
 * 所以原工程的任务创建顺序可以保持不变。
 */
App_Status_t App_Init(void)
{
    BaseType_t result;
    SensorTask_Status_t sensor_status;
    NetworkTask_Status_t network_status;

    if (s_app_task_handle != NULL)
    {
        /* 防止意外创建多个 AppTask 竞争同一组 LVGL 全局对象。 */
        return APP_ERROR_TASK_CREATE;
    }

    result = xTaskCreate(prvAppTask,
                         "AppTask",
                         APP_TASK_STACK_DEPTH,
                         NULL,
                         APP_TASK_PRIORITY,
                         &s_app_task_handle);

    if (result != pdPASS)
    {
        s_app_task_handle = NULL;
        return APP_ERROR_TASK_CREATE;
    }

    sensor_status = SensorTask_Init();
    if (sensor_status != SENSOR_TASK_OK)
    {
        ConsoleLog_Printf("[APP][ERROR] SensorTask_Init=%d\r\n", (int)sensor_status);
        return APP_ERROR_TASK_CREATE;
    }

    network_status = NetworkTask_Init();
    if (network_status != NETWORK_TASK_OK)
    {
        ConsoleLog_Printf("[APP][ERROR] NetworkTask_Init=%d\r\n", (int)network_status);
        return APP_ERROR_TASK_CREATE;
    }

    return APP_OK;
}


