#ifndef ESP_AT_H
#define ESP_AT_H

/**
 * @file esp_at.h
 * @brief ESP32-C3 ESP-AT 协议层公共接口（不直接操作 USART/DMA）。
 *
 * 使用说明：
 *   本头文件与 esp_at.c 成对更新；Port 层 esp_at_port.h/.c 保持不变。
 *   原来的基础 API 全部保留；新增天气结构体、联网状态与超时隔离处理。
 */

#include <stdint.h>                   /* uint8_t、uint16_t */

#ifdef __cplusplus
extern "C" {
#endif



/* ========================================================================== */
/* 01. Core 函数执行状态：函数本身有没有成功完成请求                         */
/* ========================================================================== */

typedef enum
{
    ESP_AT_OK = 0,                    /* 操作正常完成或 DMA 已成功启动 */
    ESP_AT_ERROR_NOT_INITIALIZED,     /* Core 还没有完成 ESP_AT_Init() */
    ESP_AT_ERROR_BUSY,                /* 上一条命令未结束，或 TX DMA 正在工作 */
    ESP_AT_ERROR_PORT,                /* 物理 Port 返回错误 */
    ESP_AT_ERROR_LINE_TOO_LONG,       /* AT 普通文本响应行过长 */
    ESP_AT_ERROR_INVALID_COMMAND,     /* NULL、空命令，或正文包含 CR/LF */
    ESP_AT_ERROR_COMMAND_TOO_LONG,    /* 超过静态 DMA TX 缓冲区长度 */
    ESP_AT_ERROR_RECOVERY_REQUIRED    /* 前一命令超时、尚未收到迟到终止响应 */
} ESP_AT_Status_t;



/* ========================================================================== */
/* 02. AT 命令生命周期：由接收状态机根据终止响应更新                         */
/* ========================================================================== */

typedef enum
{
    ESP_AT_COMMAND_IDLE = 0,          /* 尚未发送命令，或超时旧命令已被清理 */
    ESP_AT_COMMAND_WAITING,           /* 已发送，等待本次命令最终结果 */
    ESP_AT_COMMAND_OK,                /* 收到本次命令最终 OK */
    ESP_AT_COMMAND_ERROR,             /* 收到本次命令最终 ERROR / FAIL */
    ESP_AT_COMMAND_TIMEOUT            /* 等待超时：隔离状态，不能直接发下一条 */
} ESP_AT_CommandState_t;



/* ========================================================================== */
/* 03. 网络时间与天气数据                                                    */
/* ========================================================================== */

/**
 * @brief 从 +CIPSNTPTIME 响应中解析出的日期时间。
 * weekday：星期一=1，星期日=7，与现有 RTC 驱动一致。
 */
typedef struct
{
    uint16_t year;                   /* 四位年份，例如 2026 */
    uint8_t month;                   /* 1～12 */
    uint8_t day;                     /* 1～31，由解析器按月份检验 */
    uint8_t hour;                    /* 0～23 */
    uint8_t minute;                  /* 0～59 */
    uint8_t second;                  /* 0～59 */
    uint8_t weekday;                 /* 1～7 */
} ESP_AT_NetworkTime_t;

/**
 * @brief 来自 Open-Meteo current 对象的一份完整实时天气记录。
 *
 * 只保存业务数据；不保存 JSON 字符串、网络状态或 LCD 显示属性。
 * 如果未来增加天气时间戳、城市配置，应按接口版本更新此结构体。
 */
typedef struct
{
    float temperature;               /* 室外气温：摄氏度 °C */
    uint8_t humidity;                /* 室外相对湿度：0～100 %RH */
    uint8_t weather_code;            /* WMO weather_code：当前接受 0～99 */
    float wind_speed;                /* 10 米高度风速：km/h */
    uint8_t is_day;                  /* 1=白天，0=夜晚 */
} WeatherData_t;



/* ========================================================================== */
/* 04. 初始化、命令发送、状态轮询                                           */
/* ========================================================================== */

/** @brief 初始化 Core 及其底层 Port。重复调用不会重置运行中的状态。 */
ESP_AT_Status_t ESP_AT_Init(void);

/**
 * @brief 仅供首次 AT 握手失败后重试：在保护等待并清理迟到响应后重整协议状态。
 * 禁止用于已建立 Wi-Fi/HTTP 会话的随意超时恢复；并非 ESP 硬件复位。
 */
ESP_AT_Status_t ESP_AT_RearmStartupSession(void);


/**
 * @brief 启动一条异步 AT 命令。
 * @param command 命令正文，不含 CRLF。内部复制到静态 DMA 缓冲区。
 * @return ESP_AT_OK 仅表示发送已启动，不代表 ESP32 执行成功。
 */
ESP_AT_Status_t ESP_AT_StartCommand(const char *command);

/** @brief 兼容原接口：等价于 ESP_AT_StartCommand("AT")。 */
ESP_AT_Status_t ESP_AT_StartBasicTest(void);

/** @brief 处理 Port 接收的数据；在 NetworkTask 上下文中调用，非 ISR。 */
ESP_AT_Status_t ESP_AT_Process(void);

/** @brief 返回当前命令状态；不清除该状态。 */
ESP_AT_CommandState_t ESP_AT_GetCommandState(void);

/**
 * @brief 标记当前等待中的 AT 命令为超时，阻止直接发送下一条命令。
 * Core 将继续消费迟到的响应；收到旧命令最终 OK/ERROR/FAIL 后恢复 IDLE。
 * 如果不再收到终止帧，则需要外部受控重启设备，不能盲目强制复位状态。
 */
void ESP_AT_MarkTimeout(void);



/* ========================================================================== */
/* 05. 查询本次命令所产生的结果                                              */
/* ========================================================================== */

/** @brief 查询 Station 模式（0～3），-1 表示还没有有效查询结果。 */
int ESP_AT_GetWiFiMode(void);

/**
 * @brief 是否已收到 WIFI GOT IP 且之后没有 WIFI DISCONNECT。
 * 这是被动观测到的状态，并不代表互联网一定可用。
 */
uint8_t ESP_AT_HasWiFiIP(void);

/** @brief 本次 SNTP 命令 OK 且时间解析成功时，复制输出时间，返回 1。 */
uint8_t ESP_AT_GetSNTPTime(ESP_AT_NetworkTime_t *time);

/** @brief 兼容原温度接口：本次天气五字段有效时输出温度，返回 1。 */
uint8_t ESP_AT_GetOutdoorTemperature(float *temperature_c);

/**
 * @brief 本次 HTTP 命令 OK 且五字段均合法时，整体复制 WeatherData_t。
 * @return 1=成功；0=不可用，失败时不更改调用者的输出对象。
 * 不能跨任务同时调用 Core；NetworkTask 是它的唯一操作者。
 */
uint8_t ESP_AT_GetWeatherData(WeatherData_t *weather);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AT_H */


