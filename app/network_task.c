/**
 * @file    network_task.c
 * @brief   天气时钟网络任务：ESP32-C3 初始化、Wi-Fi 连接、网络校时及天气查询。
 *
 * @details
 * 本文件属于应用任务层（NetworkTask），不直接操作 USART2、DMA 或 ESP32-C3 寄存器。
 *
 * 模块之间的职责划分：
 *   1. NetworkTask（本文件）
 *      - 决定按什么顺序执行 AT 命令、设置每条命令的等待时间；
 *      - 连接指定 Wi-Fi、获取网络时间并更新 STM32 RTC；
 *      - 请求秦皇岛五字段天气，按需发布给 AppTask 使用。
 *   2. ESP-AT Core（esp_at.c）
 *      - 组织 AT 命令、维护命令状态、解析 Wi-Fi / SNTP / HTTP 响应；
 *      - 提供 ESP_AT_GetSNTPTime()、ESP_AT_GetWeatherData() 等接口。
 *   3. ESP-AT Port（esp_at_port.c）
 *      - 管理 USART2 发送 DMA、接收中断和 RX Ring Buffer（环形缓冲区）。
 *   4. RTC 驱动（rtc.c）
 *      - 负责读写 STM32F407 内部 RTC，NetworkTask 不访问 RTC 硬件寄存器。
 *
 * 当前版本的完整工作流程：
 *   系统上电
 *      -> 等待 ESP32-C3 启动
 *      -> ESP_AT_Init()
 *      -> AT / ATE0 / AT+GMR
 *      -> 设置并检查 Station 模式
 *      -> 连接 Wi-Fi，查询 IP
 *      -> 配置 SNTP，获取时间，设置 STM32 RTC
 *      -> HTTPS 查询秦皇岛实时天气五字段
 *      -> 进入周期服务：15 分钟天气更新、断线重连、24 小时网络校时。
 *
 * 重要约束：
 *   - 本版实现周期天气更新和保守的错误恢复，不新建其他后台任务。
 *   - 同一时间只执行一条 AT 命令；等待期间周期调用 ESP_AT_Process()。
 *   - 普通命令等待 2 秒；连接 Wi-Fi、HTTPS 请求允许更长时间。
 *   - RTC 校时失败不代表 RTC 硬件停止运行；原有 RTC 可以继续计时。
 *   - 天气 HTTP 请求失败不代表 ESP-AT / Wi-Fi 初始化失败；两者分开记录。
 *   - 天气数据先解析到 WeatherData_t，再在临界区中发布线程安全快照。
 *   - TIMEOUT 后禁止发新 AT，直到旧响应真正结束；不盲目清空缓冲重发。
 *   - Wi-Fi 凭据从本地 wifi_credentials.h 读取，不提交至 Git。
 *
 * 注释约定：
 *   - 文件按功能分区，公有接口与私有辅助函数分开；
 *   - 每个函数描述功能、参数、返回结果及调用背景；
 *   - 对“为什么这样写”的关键设计给出说明，不为每个标点重复注释。
 */

/* ========================================================================== */
/* 01. 头文件：模块接口、FreeRTOS、ESP-AT、RTC 和 C 标准库                  */
/* ========================================================================== */
#include "wifi_credentials.h" /* 本地 Wi-Fi 凭据：从 example 复制并填写；已被 Git 忽略 */
#include "network_task.h"   /* NetworkTask_Init()、天气快照等业务接口 */
#include "app.h"            /* 天气成功发布后通知 AppTask */
#include "esp_at.h"           /* ESP-AT Core：发送命令、解析结果、命令状态 */
#include "rtc.h"              /* RTC 驱动：RTC_DateTime_t、读写日期时间 */

#include "FreeRTOS.h"         /* pdTRUE、pdFALSE、pdPASS、pdMS_TO_TICKS 等 */
#include "task.h"             /* xTaskCreate()、vTaskDelay()、xTaskGetTickCount() */

#include <stdint.h>            /* uint32_t */
#include <stddef.h>            /* size_t、NULL */
#include <stdio.h>             /* ConsoleLog_Printf()、snprintf() */
#include "console_log.h"  /* FreeRTOS Mutex：整条 USART1 日志互斥输出 */

/* ========================================================================== */
/* 02. 配置参数：任务属性、等待时间、命令缓冲区                              */
/* ========================================================================== */

/* FreeRTOS 任务栈深度，单位是 StackType_t（不是字节）。
 * 保留原工程验证过的 512，后续可结合 uxTaskGetStackHighWaterMark() 实测调整。 */
#define NETWORK_TASK_STACK_DEPTH             512U

/* 任务优先级。保留原配置 2，与目前能够正常运行的系统保持一致。 */
#define NETWORK_TASK_PRIORITY                2U

/* STM32 与 ESP32-C3 近似同时上电。先等待 5 秒，避免 ESP-AT 固件尚未启动完毕。 */
#define NETWORK_TASK_ESP_BOOT_WAIT_MS        7000U

/* 发送 AT 命令后的接收轮询周期；等待期间使用 vTaskDelay() 主动让出 CPU。 */
#define NETWORK_TASK_PROCESS_PERIOD_MS       10U

/* 首次 AT 配置失败后，给 UART 足够长的保护间隔再进行软件重握手。 */
#define NETWORK_TASK_STARTUP_RETRY_GUARD_MS   8000U

/* 简单 AT 命令（AT、ATE0、版本、模式、IP、SNTP）最长等待时间。 */
#define NETWORK_TASK_AT_TIMEOUT_MS           4000U

/* 连接 AP 涉及扫描、认证、DHCP 等过程，等待时间需要比普通 AT 命令长。 */
#define NETWORK_TASK_WIFI_TIMEOUT_MS         20000U

/* HTTPS 请求可能包含 DNS、TCP 连接、TLS 握手、服务器处理与接收数据。 */
#define NETWORK_TASK_HTTP_TIMEOUT_MS         20000U

/* 开启 SNTP 后，暂时留给 ESP32-C3 进行首次网络时间同步的等待时间。 */
#define NETWORK_TASK_SNTP_WAIT_MS            5000U

/* 在任务栈上临时构造 AT+CWJAP 命令的缓冲区大小。 */
#define NETWORK_TASK_WIFI_COMMAND_SIZE       128U

/* 定时工作采用同一个 NetworkTask，不创建专门的天气刷新任务。 */
#define NETWORK_TASK_SERVICE_PERIOD_MS        200U

/* 天气更新成功后每 15 分钟检查一次；失败后 1 分钟再尝试。 */
#define NETWORK_TASK_WEATHER_PERIOD_MS        (15UL * 60UL * 1000UL)
#define NETWORK_TASK_WEATHER_FAIL_RETRY_MS    (60UL * 1000UL)

/* 一次业务请求最多发送两次，且仅在上一条命令已经完整结束后才能再试。 */
#define NETWORK_TASK_WEATHER_MAX_ATTEMPTS     2U
#define NETWORK_TASK_WEATHER_RETRY_DELAY_MS   3000U

/* Wi-Fi 无 IP 时，避免高频 CWJAP 冲击热点。 */
#define NETWORK_TASK_WIFI_RETRY_MS            (60UL * 1000UL)

/* RTC 成功校时后 24 小时重新校时；失败则 1 小时后再尝试。 */
#define NETWORK_TASK_SNTP_PERIOD_MS           (24UL * 60UL * 60UL * 1000UL)
#define NETWORK_TASK_SNTP_FAIL_RETRY_MS       (60UL * 60UL * 1000UL)

/* 对 AppTask 提供的天气快照若超过 2 小时仍未刷新，则认为已过期。 */
#define NETWORK_TASK_WEATHER_MAX_AGE_MS       (2UL * 60UL * 60UL * 1000UL)

/* 默认仅打印简短结果；若需要五字段调试，可切换为 1U。 */
#define NETWORK_TASK_VERBOSE_WEATHER          0U

/* FreeRTOS Tick 需要能够表示 24 小时，这里使用 STM32 工程的 32 位 Tick。 */
#if defined(configUSE_16_BIT_TICKS) && (configUSE_16_BIT_TICKS == 1)
#error "NetworkTask periodic 24-hour scheduling requires 32-bit FreeRTOS ticks"
#endif

/* 为便于调试，所有日志都采用相同前缀；便于从交错的串口输出中识别来源。 */
#define NETWORK_LOG_PREFIX                   "[NetworkTask] "
#define WEATHER_LOG_PREFIX                   "[Weather] "

/* ========================================================================== */
/* 03. 配置数据：Wi-Fi 凭据与秦皇岛天气接口                                  */
/* ========================================================================== */

/* 在 wifi_credentials.h 中定义 WEATHER_WIFI_SSID 与 WEATHER_WIFI_PASSWORD。
 * ESP-AT 使用 2.4 GHz Wi-Fi；密码包含引号、反斜杠时，仍需按协议转义。 */
static const char s_wifi_ssid[]     = WEATHER_WIFI_SSID;
static const char s_wifi_password[] = WEATHER_WIFI_PASSWORD;

/* 秦皇岛市区附近参考位置：北纬 39.9354°，东经 119.6005°。
 * HTTPCLIENT=2,0：HTTPS GET 请求中使用的请求方式、内容类型参数；
 * 最后的 2：通过 TLS 进行 HTTPS 传输。
 * 相邻 C 字符串常量在编译期自动拼接，不会增加空格或换行；
 * ESP_AT_StartCommand() 会统一在命令末尾添加 \r\n。
 *
 * 五个 current 字段：温度、相对湿度、天气代码、风速、昼夜状态。
 * 注意：ESP-AT 以逗号划分命令参数，即使位于引号内部，URL 的数据逗号
 * 也应写成 \,；在 C 字符串中必须写为 "\\,"，发送后才是一个反斜杠加逗号。
 * 最末尾 ,,,2 是真正的 AT 参数分隔符，不可转义。
 * Core 的 ESP_AT_TX_BUFFER_SIZE 应设置为至少 180 字节，此版本使用 256。 */
static const char s_qhd_weather_command[] =
    "AT+HTTPCLIENT=2,0,\"https://api.open-meteo.com/v1/forecast"
    "?latitude=39.9354&longitude=119.6005"
    "&current=temperature_2m\\,relative_humidity_2m\\,weather_code\\,wind_speed_10m\\,is_day\",,,2";

/* SNTP 配置：启用网络时间同步，设置时区 UTC+8（北京时间），指定两个服务器。 */
static const char s_sntp_config_command[] =
    "AT+CIPSNTPCFG=1,8,\"cn.ntp.org.cn\",\"ntp.sjtu.edu.cn\"";

/* ========================================================================== */
/* 04. 私有函数声明：上层流程、业务动作、通用命令执行                        */
/* ========================================================================== */

/* 任务入口：所有网络操作按顺序在此任务内执行，禁止多个任务同时发 AT 命令。 */
static void prvNetworkTask(void *argument);

/* 通用 AT 命令执行器：发送一次、轮询接收、等待 OK / ERROR / 超时。 */
static BaseType_t prvExecuteATCommand(const char *command, const char *log_name, uint32_t timeout_ms);

/* 基础 ESP-AT 初始化：检查 AT 通信、关闭回显、查询版本和设置 Station 模式。 */
static BaseType_t prvInitializeATCommands(const char **failed_step);

/* 首次握手失败后，在任务上下文消费迟到 RX，并防止反复发送产生错位响应。 */
static void prvWaitStartupGuard(void);

/* Wi-Fi：组装连接命令、加入热点、读取 Station 的 IP 配置。 */
static BaseType_t prvConnectWiFi(const char **failed_step);

/* SNTP + RTC：读取北京时间、更新 STM32 RTC，并读回确认。 */
static BaseType_t prvSynchronizeRTC(void);

/* 天气：向 Open-Meteo 请求秦皇岛的五项实时天气，并读取完整结构体。 */
static BaseType_t prvFetchQinhuangdaoWeather(void);

/* 仅基础 AT 无法初始化等严重故障时，长期阻塞并保留任务。 */
static void prvIdleForever(void);

/* 周期网络服务：Wi-Fi 恢复、时间校准、天气更新，单个 Task 串行执行。 */
static void prvRunPeriodicService(BaseType_t initial_rtc_ok, BaseType_t initial_weather_ok);

/* 后台 3 秒等待期间依然消费 URC，防止 Port 环形缓冲区积压。 */
static void prvWaitAndProcess(uint32_t delay_ms);

/* 发布天气结构体时短时间进入 FreeRTOS 临界区，避免 AppTask 读取到一半更新。 */
static WeatherData_t s_last_weather;
static TickType_t s_last_weather_tick = (TickType_t)0;
static uint8_t s_last_weather_valid = 0U;

/* --------------------------------------------------------------------------
 * V3 顶栏状态快照（由 NetworkTask 独占写，AppTask 读取）。
 *
 * 不让 AppTask 直接调用 ESP_AT_Process()/ESP_AT_HasWiFiIP()：
 * ESP-AT Core 与 RX 解析由 NetworkTask 独占，避免潜在的状态竞争。
 * Wi-Fi 标志只说明获得 IP；同步标志说明“本次上电至少校时一次”。
 * -------------------------------------------------------------------------- */
static volatile uint8_t s_ui_wifi_has_ip = 0U;
static volatile uint8_t s_ui_sntp_synced = 0U;

/** @brief 由 NetworkTask 调用；最新 URC 已经经 ESP_AT_Process 消费。 */
static void prvPublishUiLinkState(void)
{
    uint8_t has_ip = (ESP_AT_HasWiFiIP() != 0U) ? 1U : 0U;
    taskENTER_CRITICAL();
    s_ui_wifi_has_ip = has_ip;
    taskEXIT_CRITICAL();
}

/** @brief 只有 RTC 真正校时成功后才能置 1，不能凭 Wi-Fi 在线推断。 */
static void prvPublishUiSntpSynced(void)
{
    taskENTER_CRITICAL();
    s_ui_sntp_synced = 1U;
    taskEXIT_CRITICAL();
}

uint8_t NetworkTask_GetUiStatus(NetworkTask_UiStatus_t *out)
{
    if (out == NULL) return 0U;
    taskENTER_CRITICAL();
    out->wifi_has_ip = s_ui_wifi_has_ip;
    out->sntp_synced = s_ui_sntp_synced;
    taskEXIT_CRITICAL();
    return 1U;
}


/* ========================================================================== */
/* 05. 对外接口：创建 NetworkTask                                             */
/* ========================================================================== */

/**
 * @brief  创建负责 ESP32-C3 通信的 FreeRTOS 任务。
 *
 * @return NETWORK_TASK_OK：创建成功；
 *         NETWORK_TASK_ERROR_TASK_CREATE：FreeRTOS 无法分配任务资源。
 *
 * @note   本函数只创建任务，不会立刻阻塞等待 Wi-Fi；真正的网络流程
 *         在 FreeRTOS 调度器开始运行后，由 prvNetworkTask() 执行。
 *         为避免创建多个实例，调用者应在系统初始化阶段只调用一次。
 */
NetworkTask_Status_t NetworkTask_Init(void)
{
    BaseType_t create_result;

    create_result = xTaskCreate(prvNetworkTask, "NetworkTask", NETWORK_TASK_STACK_DEPTH, NULL, NETWORK_TASK_PRIORITY, NULL);

    if (create_result != pdPASS)
    {
        return NETWORK_TASK_ERROR_TASK_CREATE;
    }

    return NETWORK_TASK_OK;
}

/* ========================================================================== */
/* 06. 任务主流程：只保留业务步骤和失败边界，避免深层 if 嵌套                 */
/* ========================================================================== */

/**
 * @brief NetworkTask 的主入口。
 *
 * @param argument FreeRTOS 传入的任务参数，本项目暂不需要，显式忽略。
 *
 * 执行策略：
 *   1. ESP-AT 硬件和基础 AT 初始化失败：停止后续网络操作；
 *   2. Wi-Fi 连接失败：不查询网络时间与天气；
 *   3. SNTP / RTC 校时失败：保持旧 RTC 正常走时，本轮不继续天气请求；
 *   4. 天气请求失败：仅报告天气失败，不把 Wi-Fi 初始化误报为失败；
 *   5. 本轮结束后休眠，保留该任务供以后加入周期天气刷新。
 *
 * 各步骤被拆为私有函数，方便单独阅读、断点调试和以后功能扩展。
 */
static void prvNetworkTask(void *argument)
{
    ESP_AT_Status_t at_status;
    const char *failed_step = "Unknown";
    BaseType_t wifi_ok;
    BaseType_t rtc_ok = pdFALSE;
    BaseType_t weather_ok = pdFALSE;

    (void)argument;

    /* 1. 两块 MCU 可能同时上电，等待 ESP32-C3 固件进入 AT 命令状态。 */
    ConsoleLog_Printf(NETWORK_LOG_PREFIX "Waiting for ESP32-C3 boot...\r\n");
    vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_ESP_BOOT_WAIT_MS));

    /* 2. Core 内部调用 Port_Init()，本文件不访问 DMA / USART 寄存器。 */
    at_status = ESP_AT_Init();
    if (at_status != ESP_AT_OK)
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "FATAL: ESP-AT init failed: %d\r\n", (int)at_status);
        prvIdleForever();
    }
    ConsoleLog_Printf(NETWORK_LOG_PREFIX "ESP-AT init OK\r\n");

    /* 3. 基础 AT 配置失败属于可能恢复的启动异常：
     *    不再让 NetworkTask 永久停止。每次失败后等待 8 秒并消费旧 RX，
     *    再由 Core 校验 TX 是否空闲，重整初始软件协议状态并从 AT 重试。
     *    注意：真正的电源故障仍需 ESP 硬件复位/电气排查。 */
    for (;;)
    {
        if (prvInitializeATCommands(&failed_step) == pdTRUE)
        {
            ConsoleLog_Printf(NETWORK_LOG_PREFIX "ESP-AT setup recovered/verified\r\n");
            break;
        }

        ConsoleLog_Printf(NETWORK_LOG_PREFIX
            "Startup AT setup failed at %s; waiting %u ms before retry\r\n",
            failed_step, (unsigned int)NETWORK_TASK_STARTUP_RETRY_GUARD_MS);
        prvWaitStartupGuard();
        do
        {
            at_status = ESP_AT_RearmStartupSession();
            if (at_status != ESP_AT_OK)
            {
                ConsoleLog_Printf(NETWORK_LOG_PREFIX
                    "Startup rearm deferred (state/Port error=%d); keep guarding\r\n",
                    (int)at_status);
                /* 端口 TX 若持续 BUSY/ERROR，不能覆盖 DMA 仍在读取的缓冲。 */
                prvWaitStartupGuard();
            }
        } while (at_status != ESP_AT_OK);
    }

    /* 4. 首次联网。失败不是致命故障，周期服务将低频重试连接。 */
    wifi_ok = prvConnectWiFi(&failed_step);
    prvPublishUiLinkState();   /* 记录已解析的连接状态，供主界面顶栏显示。 */
    if (wifi_ok != pdTRUE)
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "Wi-Fi setup failed at %s; retry later\r\n", failed_step);
    }
    else
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "ESP-AT and Wi-Fi setup PASSED\r\n");

        /* 5. 首次校时：失败不会破坏 RTC 当前保持的时间。 */
        rtc_ok = prvSynchronizeRTC();
        if (rtc_ok == pdTRUE) prvPublishUiSntpSynced();
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "RTC synchronization %s\r\n",
               (rtc_ok == pdTRUE) ? "PASSED" : "FAILED; keep previous RTC time");

        /* 6. 首次天气：即使 SNTP 本轮失败，也允许尝试天气并独立记录。 */
        if (ESP_AT_GetCommandState() != ESP_AT_COMMAND_TIMEOUT)
        {
            weather_ok = prvFetchQinhuangdaoWeather();
            ConsoleLog_Printf(WEATHER_LOG_PREFIX "Qinhuangdao weather update %s\r\n",
                   (weather_ok == pdTRUE) ? "PASSED" : "FAILED");
        }
    }

    /* 7. 进入正常的周期维护循环，仍由同一个 NetworkTask 管理。 */
    prvRunPeriodicService(rtc_ok, weather_ok);
}

/**
 * @brief 启动阶段的有界保护等待；只由 NetworkTask 单线程操作 ESP-AT。
 *
 * 避免 AT 超时后立即发送新命令，把迟到的旧 OK 错认成新命令的响应。
 * 等待 8 秒期间继续解析 UART RX，应用程序/RTC/LCD 任务不受阻塞。
 */
static void prvWaitStartupGuard(void)
{
    TickType_t started = xTaskGetTickCount();
    const TickType_t guard_ticks = pdMS_TO_TICKS(NETWORK_TASK_STARTUP_RETRY_GUARD_MS);
    const TickType_t poll_ticks = pdMS_TO_TICKS(NETWORK_TASK_SERVICE_PERIOD_MS);
    uint8_t reported_error = 0U;

    while ((TickType_t)(xTaskGetTickCount() - started) < guard_ticks)
    {
        if (ESP_AT_Process() != ESP_AT_OK)
        {
            if (reported_error == 0U)
            {
                ConsoleLog_Printf(NETWORK_LOG_PREFIX
                    "Startup guard: invalid RX, continue draining\r\n");
                reported_error = 1U;
            }
            ESP_AT_MarkTimeout();
        }
        vTaskDelay((poll_ticks > 0U) ? poll_ticks : (TickType_t)1U);
    }
}

/* ========================================================================== */
/* 07. 通用 AT 命令执行：启动一次异步发送，并在任务中等待最终响应           */
/* ========================================================================== */

/**
 * @brief  执行一条 AT 命令，周期处理 ESP-AT 接收数据，直至成功或失败。
 *
 * @param  command    真正交给 ESP_AT_StartCommand() 的 AT 正文，不带 CRLF。
 * @param  log_name   调试日志中的简短名称，可避免打印 Wi-Fi 密码或过长 URL。
 * @param  timeout_ms 允许等待的最大时间，单位毫秒。
 * @return pdTRUE     收到该命令的最终 OK。
 *         pdFALSE    命令启动失败、收包解析失败、收到 ERROR/FAIL 或超时。
 *
 * @details
 * ESP_AT_StartCommand() 使用 TX DMA 异步发送，调用成功只代表开始发送，
 * 并不代表 ESP32-C3 已处理完毕。因此必须循环执行 ESP_AT_Process()，
 * 读取由 USART2 RX 中断放入环形缓冲区的字节，检查最终命令状态。
 *
 * 这里所谓“等待”，仅阻塞 NetworkTask 本身；每轮 vTaskDelay() 都会让出
 * CPU 给 AppTask / SensorTask / LCD 任务，不会停止 FreeRTOS 调度。
 *
 * TIMEOUT 注意事项：超时后调用 ESP_AT_MarkTimeout()，Core 将禁止
 * 启动新命令，以免旧命令迟到的 OK 被误认为下一条命令的响应。
 * 本版不提供自动恢复，因此调用者应在失败时结束本轮网络操作。
 */
static BaseType_t prvExecuteATCommand(const char *command, const char *log_name, uint32_t timeout_ms)
{
    ESP_AT_Status_t at_status;
    ESP_AT_CommandState_t command_state;
    TickType_t start_tick;
    TickType_t timeout_ticks;
    const TickType_t poll_ticks = pdMS_TO_TICKS(NETWORK_TASK_PROCESS_PERIOD_MS);

    /* 同时校验日志名称和命令指针，避免 ConsoleLog_Printf("%s") 解引用空指针。 */
    if ((command == NULL) || (log_name == NULL) || (timeout_ms == 0U))
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "Invalid AT command arguments\r\n");
        return pdFALSE;
    }

    /* 将毫秒转换为 RTOS Tick；若时钟节拍很粗，也至少等待一个 Tick。 */
    timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == (TickType_t)0)
    {
        timeout_ticks = (TickType_t)1;
    }

    /* 这一步只启动 DMA 发送。命令正文由 Core 复制到静态 TX 缓冲区。 */
    at_status = ESP_AT_StartCommand(command);
    if (at_status != ESP_AT_OK)
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "Start %s failed: %d\r\n", log_name, (int)at_status);
        return pdFALSE;
    }

    ConsoleLog_Printf(NETWORK_LOG_PREFIX "TX -> %s\r\n", log_name);
    start_tick = xTaskGetTickCount();

    for (;;)
    {
        /* 非阻塞地取出 USART RX Ring Buffer 中的原始字节并交给 Core 解析。 */
        at_status = ESP_AT_Process();
        if (at_status != ESP_AT_OK)
        {
            ConsoleLog_Printf(NETWORK_LOG_PREFIX "%s RX process failed: %d\r\n", log_name, (int)at_status);

            /* 解析流程发生错误，当前 AT 命令的结果已不可信；不继续下一条。 */
            ESP_AT_MarkTimeout();
            return pdFALSE;
        }

        /* 只有最终 OK 才算成功，中间的 +CIPSTA / +HTTPCLIENT 等不是终止响应。 */
        command_state = ESP_AT_GetCommandState();
        if (command_state == ESP_AT_COMMAND_OK)
        {
            ConsoleLog_Printf(NETWORK_LOG_PREFIX "%s -> OK\r\n", log_name);
            return pdTRUE;
        }
        if (command_state == ESP_AT_COMMAND_ERROR)
        {
            ConsoleLog_Printf(NETWORK_LOG_PREFIX "%s -> ERROR\r\n", log_name);
            return pdFALSE;
        }

        /* TickType_t 是无符号计数，差值写法允许系统 Tick 计数自然回绕。 */
        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks)
        {
            ConsoleLog_Printf(NETWORK_LOG_PREFIX "%s response timeout (%lu ms)\r\n", log_name, (unsigned long)timeout_ms);
            ESP_AT_MarkTimeout();
            return pdFALSE;
        }

        /* 其他任务仍可运行；当前文件保留原来的 10 ms 轮询周期。 */
        vTaskDelay((poll_ticks > (TickType_t)0) ? poll_ticks : (TickType_t)1);
    }
}

/* ========================================================================== */
/* 08. ESP-AT 基础初始化：AT、ATE0、版本检查、Station 模式                  */
/* ========================================================================== */

/**
 * @brief  验证 AT 通信并设置 Station 工作模式。
 * @param  failed_step 输出发生失败的命令名称，仅指向静态字符串常量。
 * @return pdTRUE：基础设置和 Station 模式验证通过；否则 pdFALSE。
 *
 * 说明：ESP_AT_GetWiFiMode() 是 Core 已解析的数据；
 *       单纯的 AT+CWMODE? -> OK 并不等于 mode 已经是 1。
 */
static BaseType_t prvInitializeATCommands(const char **failed_step)
{
    const char *command;
    int wifi_mode;

    if (failed_step == NULL)
    {
        return pdFALSE;
    }

    /* 1. 基础通信测试：ESP32-C3 已经准备好处理 AT 命令。 */
    command = "AT";
    *failed_step = command;
    if (prvExecuteATCommand(command, command, NETWORK_TASK_AT_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    /* 2. 关闭命令回显，避免后续响应中混入发送的命令正文。 */
    command = "ATE0";
    *failed_step = command;
    if (prvExecuteATCommand(command, command, NETWORK_TASK_AT_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    /* 3. 查询固件版本，确认运行的是支持相关命令的 ESP-AT 固件。 */
    command = "AT+GMR";
    *failed_step = command;
    if (prvExecuteATCommand(command, command, NETWORK_TASK_AT_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    /* 4. 设置 Station：设备作为 Wi-Fi 客户端连接热点，而不是创建 SoftAP。 */
    command = "AT+CWMODE=1";
    *failed_step = command;
    if (prvExecuteATCommand(command, command, NETWORK_TASK_AT_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    /* 5. 读取实际工作模式，再确认返回了 +CWMODE:1。 */
    command = "AT+CWMODE?";
    *failed_step = command;
    if (prvExecuteATCommand(command, command, NETWORK_TASK_AT_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    wifi_mode = ESP_AT_GetWiFiMode();
    if (wifi_mode != 1)
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "Unexpected Wi-Fi mode: %d (expected Station=1)\r\n", wifi_mode);
        return pdFALSE;
    }

    ConsoleLog_Printf(NETWORK_LOG_PREFIX "Wi-Fi mode verified: Station\r\n");
    return pdTRUE;
}

/* ========================================================================== */
/* 09. Wi-Fi 网络连接：构造 CWJAP、等待联网、查询 DHCP 地址                  */
/* ========================================================================== */

/**
 * @brief  加入指定 Wi-Fi，并读取 Station 当前 IP、网关与子网掩码。
 * @param  failed_step 输出失败的具体阶段：AT+CWJAP 或 AT+CIPSTA?。
 * @return pdTRUE：加入热点成功、IP 查询 AT 命令返回 OK；否则 pdFALSE。
 *
 * 说明：
 *   - Wi-Fi SSID 和密码在本函数中组装进任务栈上的 wifi_command。
 *   - ESP_AT_StartCommand() 会将正文复制到 Core 的静态双 TX 缓冲区，
 *     因此发送 DMA 不会在本函数返回之后继续使用此局部字符串。
 *   - snprintf() 检查命令的输出容量，避免拼接后覆盖栈上内存。
 *   - 当前用户名、密码是普通 ASCII，无需增加 ESP-AT 特殊字符转义。
 */
static BaseType_t prvConnectWiFi(const char **failed_step)
{
    char wifi_command[NETWORK_TASK_WIFI_COMMAND_SIZE];
    int command_length;
    const char *command;

    if (failed_step == NULL)
    {
        return pdFALSE;
    }

    /* AT+CWJAP="SSID","PASSWORD"：请求 ESP32-C3 关联指定热点。 */
    *failed_step = "AT+CWJAP";
    command_length = snprintf(wifi_command, sizeof(wifi_command), "AT+CWJAP=\"%s\",\"%s\"", s_wifi_ssid, s_wifi_password);

    if ((command_length < 0) || ((size_t)command_length >= sizeof(wifi_command)))
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "Wi-Fi command buffer too small\r\n");
        return pdFALSE;
    }

    /* 连接热点可能需要扫描、认证和等待 DHCP，使用 20 秒专用超时。 */
    if (prvExecuteATCommand(wifi_command, "AT+CWJAP", NETWORK_TASK_WIFI_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    /* 查询已分配的 IP、默认网关和子网掩码；结果由 Core 打印。 */
    command = "AT+CIPSTA?";
    *failed_step = command;
    if (prvExecuteATCommand(command, command, NETWORK_TASK_AT_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    return pdTRUE;
}

/* ========================================================================== */
/* 10. 网络校时：SNTP 查询、网络时间转换、STM32 RTC 写入及验证              */
/* ========================================================================== */

/**
 * @brief  从 ESP32-C3 获取北京时间并更新 STM32 内部 RTC。
 * @return pdTRUE：SNTP 时间有效，RTC 写入及读回成功；否则 pdFALSE。
 *
 * 步骤：
 *   1. AT+CIPSNTPCFG=1,8,... 启动 SNTP 并使用 UTC+8；
 *   2. 等待一段时间，便于 ESP32-C3 完成首次网络同步；
 *   3. AT+CIPSNTPTIME? 取得当前网络日期时间；
 *   4. ESP_AT_GetSNTPTime() 检查 Core 是否成功解析日期；
 *   5. 拷贝到 RTC_DateTime_t，并调用 RTC_Driver_SetDateTime()；
 *   6. RTC_Driver_GetDateTime() 读回当前日期时间以确认硬件写入。
 *
 * 注意：固定等待 5 秒只是当前验证策略，不保证所有网络都在 5 秒内完成
 * SNTP 同步；未来可以结合 +TIME_UPDATED 事件完善等待策略。
 */
static BaseType_t prvSynchronizeRTC(void)
{
    ESP_AT_NetworkTime_t network_time;
    RTC_DateTime_t rtc_time;
    RTC_DateTime_t rtc_readback;
    RTC_Status_t rtc_status;
    const char *command;

    /* 第一步：配置 SNTP 服务和北京时间 UTC+8。 */
    if (prvExecuteATCommand(s_sntp_config_command, "AT+CIPSNTPCFG", NETWORK_TASK_AT_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    /* 第二步：ESP-AT 返回配置 OK 不代表已经收到时间；给服务器同步留出时间。 */
    ConsoleLog_Printf(NETWORK_LOG_PREFIX "Waiting for SNTP synchronization...\r\n");
    vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_SNTP_WAIT_MS));

    /* 第三步：请求当前 SNTP 时间；Core 按固定格式解析英文月份和星期。 */
    command = "AT+CIPSNTPTIME?";
    if (prvExecuteATCommand(command, command, NETWORK_TASK_AT_TIMEOUT_MS) != pdTRUE)
    {
        return pdFALSE;
    }

    /* 第四步：命令 OK 只是 AT 层成功，还必须确认获取了合理的时间字段。 */
    if (ESP_AT_GetSNTPTime(&network_time) == 0U)
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "SNTP time is invalid or not synchronized\r\n");
        return pdFALSE;
    }
    ConsoleLog_Printf(NETWORK_LOG_PREFIX "SNTP time parsed successfully\r\n");

    /* 第五步：Core 的日期结构与 RTC 驱动结构彼此独立，逐字段转换即可。
     * year 使用完整公历年份，如 2026；RTC 驱动自行转换为硬件寄存器格式。
     * weekday：周一=1，周二=2，…，周日=7；无需再次转换。 */
    rtc_time.year = network_time.year;
    rtc_time.month = network_time.month;
    rtc_time.day = network_time.day;
    rtc_time.hour = network_time.hour;
    rtc_time.minute = network_time.minute;
    rtc_time.second = network_time.second;
    rtc_time.weekday = network_time.weekday;

    rtc_status = RTC_Driver_SetDateTime(&rtc_time);
    if (rtc_status != RTC_STATUS_OK)
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "RTC set failed: %d\r\n", (int)rtc_status);
        return pdFALSE;
    }
    ConsoleLog_Printf(NETWORK_LOG_PREFIX "RTC set successfully\r\n");

    /* 第六步：读回硬件时间；只检查驱动返回状态，不强制秒数完全相同，
     * 因为在“写入”到“读回”之间 RTC 可能自然跨过了下一秒。 */
    rtc_status = RTC_Driver_GetDateTime(&rtc_readback);
    if (rtc_status != RTC_STATUS_OK)
    {
        ConsoleLog_Printf(NETWORK_LOG_PREFIX "RTC readback failed: %d\r\n", (int)rtc_status);
        return pdFALSE;
    }

    ConsoleLog_Printf(NETWORK_LOG_PREFIX "RTC readback: %04u-%02u-%02u %02u:%02u:%02u weekday=%u\r\n",
           (unsigned int)rtc_readback.year, (unsigned int)rtc_readback.month, (unsigned int)rtc_readback.day,
           (unsigned int)rtc_readback.hour, (unsigned int)rtc_readback.minute,
           (unsigned int)rtc_readback.second, (unsigned int)rtc_readback.weekday);

    ConsoleLog_Printf(NETWORK_LOG_PREFIX "Date: %04u-%02u-%02u\r\n",
           (unsigned int)network_time.year, (unsigned int)network_time.month, (unsigned int)network_time.day);
    ConsoleLog_Printf(NETWORK_LOG_PREFIX "Time: %02u:%02u:%02u, weekday=%u\r\n",
           (unsigned int)network_time.hour, (unsigned int)network_time.minute,
           (unsigned int)network_time.second, (unsigned int)network_time.weekday);

    return pdTRUE;
}

/* ========================================================================== */
/* 11. 天气查询：HTTPS GET -> 五字段 WeatherData_t -> 串口验证              */
/* ========================================================================== */

/**
 * @brief 通过 ESP32-C3 获取秦皇岛当前实时天气，并打印全部字段。
 * @return pdTRUE：收到 HTTP 最终 OK，且五个字段均已通过 Core 的有效性检查；
 *         pdFALSE：网络请求失败、JSON 字段缺失或某个数值超出范围。
 *
 * @details
 *   1. prvExecuteATCommand() 使用 Core 发送 DMA 命令、收取完整 HTTP 正文；
 *   2. Core 在 HTTP 最终 OK 时解析 WeatherData_t，且五项全部有效才提交；
 *   3. ESP_AT_GetWeatherData() 读取 Core 中的本次完整天气结构体；
 *   4. 当前阶段只打印验证；不在 NetworkTask 中直接操作 LCD。
 *
 * @note 现在仍然只在系统开机后请求一次，后续再加入周期刷新机制。
 */
static BaseType_t prvFetchQinhuangdaoWeather(void)
{
    uint8_t attempt;
    WeatherData_t weather;

    for (attempt = 1U; attempt <= NETWORK_TASK_WEATHER_MAX_ATTEMPTS; ++attempt)
    {
        /*
         * 仅上一次 AT 命令完整结束后才允许再次发送。
         * 若发生 TIMEOUT，必须等待旧的最终响应被 Core 吸收；不能立刻重试。
         */
        if (ESP_AT_GetCommandState() == ESP_AT_COMMAND_TIMEOUT)
        {
            ConsoleLog_Printf(WEATHER_LOG_PREFIX "AT timeout quarantine; defer weather\r\n");
            return pdFALSE;
        }

        ConsoleLog_Printf(WEATHER_LOG_PREFIX "Weather request attempt %u/%u\r\n",
               (unsigned int)attempt, (unsigned int)NETWORK_TASK_WEATHER_MAX_ATTEMPTS);

        if (prvExecuteATCommand(s_qhd_weather_command, "AT+HTTPCLIENT",
                                NETWORK_TASK_HTTP_TIMEOUT_MS) == pdTRUE)
        {
            /* 只有 HTTP 收齐且五个字段全部有效时，才发布完整结构体。 */
            if (ESP_AT_GetWeatherData(&weather) != 0U)
            {
                /*
                 * WeatherData_t 由本任务独占写，AppTask 可能同时读取。
                 * 把整体拷贝及时间戳放在同一个短临界区，保证快照一致性。
                 * 这里不包含 HTTP、printf 或 vTaskDelay 等阻塞操作。
                 */
                taskENTER_CRITICAL();
                s_last_weather = weather;
                s_last_weather_tick = xTaskGetTickCount();
                s_last_weather_valid = 1U;
                taskEXIT_CRITICAL();

                /*
                 * 数据已完整发布，现在通知 AppTask。
                 * 通知只携带“天气更新”的事件位，实际 WeatherData_t
                 * 仍通过 NetworkTask_GetLatestWeather() 获取。
                 * 不在临界区内发送通知，保持共享数据锁定时间尽可能短。
                 */
                App_NotifyEvent(APP_EVENT_WEATHER_UPDATED);

#if (NETWORK_TASK_VERBOSE_WEATHER != 0U)
                ConsoleLog_Printf(WEATHER_LOG_PREFIX "T=%.1f C, RH=%u%%, code=%u, wind=%.1f km/h, day=%u\r\n",
                       (double)weather.temperature, (unsigned int)weather.humidity,
                       (unsigned int)weather.weather_code, (double)weather.wind_speed,
                       (unsigned int)weather.is_day);
#else
                ConsoleLog_Printf(WEATHER_LOG_PREFIX "Five-field weather parsed and cached\r\n");
#endif
                return pdTRUE;
            }
            ConsoleLog_Printf(WEATHER_LOG_PREFIX "HTTP OK but weather JSON invalid\r\n");
        }

        /* TIMEOUT 一定不重试；ERROR 已得到明确终止结果，允许下一轮。 */
        if (ESP_AT_GetCommandState() == ESP_AT_COMMAND_TIMEOUT)
        {
            return pdFALSE;
        }
        if (attempt < NETWORK_TASK_WEATHER_MAX_ATTEMPTS)
        {
            ConsoleLog_Printf(WEATHER_LOG_PREFIX "Retrying weather after %u ms\r\n",
                   (unsigned int)NETWORK_TASK_WEATHER_RETRY_DELAY_MS);
            prvWaitAndProcess(NETWORK_TASK_WEATHER_RETRY_DELAY_MS);
        }
    }

    /* 不主动清空 s_last_weather：读接口按年龄判断是否过期。 */
    return pdFALSE;
}

/* ========================================================================== */
/* 12. 公共天气快照接口：供 AppTask、天气 UI 等任务读取                       */
/* ========================================================================== */

/**
 * @brief 读取最近一次成功更新的五字段天气快照。
 *
 * @param[out] weather 非 NULL 的结构体地址；成功时整体复制。
 * @param[out] age_seconds 可为 NULL；否则写入距离上次更新经过的秒数。
 * @return 1=数据存在且不超过 2 小时；0=尚未获得或天气已过期。
 *
 * 注意：
 *   1. 本函数不发 AT 命令、不访问 LCD，可由普通 FreeRTOS 任务调用；
 *   2. 短临界区确保读取到一份完整的天气快照，而非混杂的新旧成员；
 *   3. 不能在 ISR 中调用，它使用的是任务版本的 FreeRTOS 临界区宏；
 *   4. 返回 0 时不修改输出 weather，避免 UI 无意读取到无效数据。
 */
uint8_t NetworkTask_GetLatestWeather(WeatherData_t *weather, uint32_t *age_seconds)
{
    WeatherData_t snapshot;
    TickType_t stamp;
    TickType_t now;
    TickType_t age_ticks;
    uint8_t valid;

    if (weather == NULL)
    {
        return 0U;
    }

    /* 先取同一时刻的“数据 + 写入时 Tick + 有效标志”。 */
    taskENTER_CRITICAL();
    snapshot = s_last_weather;
    stamp = s_last_weather_tick;
    valid = s_last_weather_valid;
    now = xTaskGetTickCount();
    taskEXIT_CRITICAL();

    if (valid == 0U)
    {
        return 0U;
    }

    /* TickType_t 无符号差值处理一次自然溢出；要求 32-bit tick。 */
    age_ticks = now - stamp;
    if (age_ticks > pdMS_TO_TICKS(NETWORK_TASK_WEATHER_MAX_AGE_MS))
    {
        return 0U;
    }

    *weather = snapshot;
    if (age_seconds != NULL)
    {
        *age_seconds = (uint32_t)((uint64_t)age_ticks * (uint64_t)portTICK_PERIOD_MS / 1000ULL);
    }
    return 1U;
}



/* ========================================================================== */
/* 13. 周期后台服务：持续处理 URC、重连 Wi-Fi、更新天气与 RTC                 */
/* ========================================================================== */

/**
 * @brief 等待一定时长，并周期消费 Port 中的异步 AT 消息。
 *
 * 若连接断开期间出现 WIFI DISCONNECT 等通知，不能让 Ring Buffer 积压。
 * 在等待网络请求重试时，也要周期调用 ESP_AT_Process()。
 */
static void prvWaitAndProcess(uint32_t delay_ms)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(delay_ms);
    TickType_t pause = pdMS_TO_TICKS(NETWORK_TASK_SERVICE_PERIOD_MS);
    if (pause == 0U)
    {
        pause = 1U;
    }

    while ((xTaskGetTickCount() - started) < duration)
    {
        if (ESP_AT_Process() != ESP_AT_OK)
        {
            ConsoleLog_Printf(NETWORK_LOG_PREFIX "Background AT RX parsing error\r\n");
            ESP_AT_MarkTimeout();
            return;
        }
        vTaskDelay(pause);
    }
}

/**
 * @brief 维护无线网络与时间、天气业务；永远在 NetworkTask 内部执行。
 *
 * 调度原则：
 *   - 每 200 ms 处理异步接收；
 *   - Wi-Fi 没 IP：每 60 秒尝试一次 CWJAP，成功后重新校时和天气；
 *   - 天气成功：15 分钟后再次请求；失败：1 分钟后重试；
 *   - RTC 成功：24 小时后校时；失败：1 小时后再试；
 *   - AT 超时：冻结发送，继续接收，等待旧命令终止。若始终不返回，
 *     需要设备级重启 ESP，不能仅清空软件变量冒险发送下一条命令。
 *
 * @note 每次周期以“上次完成时刻”计时，HTTPS 耗时不会导致密集请求。
 */
static void prvRunPeriodicService(BaseType_t initial_rtc_ok,
                                  BaseType_t initial_weather_ok)
{
    TickType_t last_weather_tick = xTaskGetTickCount();
    TickType_t last_rtc_tick = last_weather_tick;
    TickType_t last_wifi_tick = last_weather_tick;
    TickType_t now;
    TickType_t weather_period;
    TickType_t rtc_period;
    BaseType_t rtc_ok = initial_rtc_ok;
    BaseType_t weather_ok = initial_weather_ok;
    uint8_t was_quarantined = 0U;
    const char *failed_step = "Unknown";

    ConsoleLog_Printf(NETWORK_LOG_PREFIX "Periodic service started: weather 15min, RTC 24h\r\n");

    for (;;)
    {
        /* 不持有任何 Core 状态锁；NetworkTask 串行操作协议收发。 */
        if (ESP_AT_Process() != ESP_AT_OK)
        {
            ConsoleLog_Printf(NETWORK_LOG_PREFIX "Background AT process error (checking session state)\r\n");
            /* 仅当有命令正在等待时才进入隔离；空闲期间的坏行只报告。 */
            ESP_AT_MarkTimeout();
        }
        /* 新收到的 WIFI DISCONNECT/GOT IP 通知在此处发布。 */
        prvPublishUiLinkState();

        if (ESP_AT_GetCommandState() == ESP_AT_COMMAND_TIMEOUT)
        {
            if (was_quarantined == 0U)
            {
                ConsoleLog_Printf(NETWORK_LOG_PREFIX "TIMEOUT: waiting late terminal response; AT TX suspended\r\n");
                was_quarantined = 1U;
            }
            vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_SERVICE_PERIOD_MS));
            continue;
        }

        if (was_quarantined != 0U)
        {
            ConsoleLog_Printf(NETWORK_LOG_PREFIX "AT session recovered after late terminal response\r\n");
            was_quarantined = 0U;
            weather_ok = pdFALSE;
            last_weather_tick = xTaskGetTickCount() -
                                pdMS_TO_TICKS(NETWORK_TASK_WEATHER_FAIL_RETRY_MS);
        }

        now = xTaskGetTickCount();

        /* 离线时先恢复 Wi-Fi；不能再尝试依赖互联网的 SNTP / HTTP。 */
        if (ESP_AT_HasWiFiIP() == 0U)
        {
            if ((now - last_wifi_tick) >= pdMS_TO_TICKS(NETWORK_TASK_WIFI_RETRY_MS))
            {
                ConsoleLog_Printf(NETWORK_LOG_PREFIX "Wi-Fi has no IP, reconnecting...\r\n");
                if (prvConnectWiFi(&failed_step) == pdTRUE)
                {
                    ConsoleLog_Printf(NETWORK_LOG_PREFIX "Wi-Fi reconnected\r\n");
                    /* 恢复后尽快刷新 RTC/天气；不在本轮继续使用旧 now。 */
                    rtc_ok = pdFALSE;
                    weather_ok = pdFALSE;
                    last_rtc_tick = xTaskGetTickCount() - pdMS_TO_TICKS(NETWORK_TASK_SNTP_FAIL_RETRY_MS);
                    last_weather_tick = xTaskGetTickCount() - pdMS_TO_TICKS(NETWORK_TASK_WEATHER_FAIL_RETRY_MS);
                }
                else
                {
                    ConsoleLog_Printf(NETWORK_LOG_PREFIX "Wi-Fi reconnect failed at %s\r\n", failed_step);
                }
                last_wifi_tick = xTaskGetTickCount();
                prvPublishUiLinkState();
            }
        }
        else
        {
            /* 更新间隔由上次是否成功决定，失败时使用退避间隔。 */
            rtc_period = pdMS_TO_TICKS((rtc_ok == pdTRUE) ?
                         NETWORK_TASK_SNTP_PERIOD_MS : NETWORK_TASK_SNTP_FAIL_RETRY_MS);
            weather_period = pdMS_TO_TICKS((weather_ok == pdTRUE) ?
                             NETWORK_TASK_WEATHER_PERIOD_MS : NETWORK_TASK_WEATHER_FAIL_RETRY_MS);

            if ((now - last_rtc_tick) >= rtc_period)
            {
                ConsoleLog_Printf(NETWORK_LOG_PREFIX "Periodic RTC synchronization...\r\n");
                rtc_ok = prvSynchronizeRTC();
                if (rtc_ok == pdTRUE) prvPublishUiSntpSynced();
                ConsoleLog_Printf(NETWORK_LOG_PREFIX "Periodic RTC sync %s\r\n",
                       (rtc_ok == pdTRUE) ? "PASSED" : "FAILED");
                last_rtc_tick = xTaskGetTickCount();
            }

            if (ESP_AT_GetCommandState() != ESP_AT_COMMAND_TIMEOUT)
            {
                now = xTaskGetTickCount();
                if ((now - last_weather_tick) >= weather_period)
                {
                    weather_ok = prvFetchQinhuangdaoWeather();
                    ConsoleLog_Printf(WEATHER_LOG_PREFIX "Periodic weather update %s\r\n",
                           (weather_ok == pdTRUE) ? "PASSED" : "FAILED");
                    last_weather_tick = xTaskGetTickCount();
                }
            }
        }

        /* 当前任务进入 Blocked，其它任务照常刷新时钟和室内传感器。 */
        vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_SERVICE_PERIOD_MS));
    }
}



/* ========================================================================== */
/* 14. 初始化不可恢复故障：任务休眠但不占 CPU                                 */
/* ========================================================================== */

static void prvIdleForever(void)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}
