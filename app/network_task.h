#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H

/**
 * @file network_task.h
 * @brief NetworkTask 对 App 层的最小公共接口。
 *
 * 注意：只让 NetworkTask 与 ESP-AT Core 交互；其他任务读取已发布的
 * WeatherData_t 快照，不直接调用 ESP_AT_StartCommand()。
 */

#include <stdint.h>
#include "esp_at.h"             /* WeatherData_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 创建天气网络任务时的返回状态。保留原工程的两项状态码。 */
typedef enum
{
    NETWORK_TASK_OK = 0,
    NETWORK_TASK_ERROR_TASK_CREATE
} NetworkTask_Status_t;

/** @brief 初始化并创建 FreeRTOS NetworkTask，系统启动时调用一次。 */
NetworkTask_Status_t NetworkTask_Init(void);

/**
 * @brief 获取最近一次成功获取、且尚未过期的室外天气快照。
 * @param[out] weather 提供输出地址；不能为 NULL。
 * @param[out] age_seconds 可为 NULL；非 NULL 时输出数据已经存在的秒数。
 * @return 1：存在新鲜快照；0：从未获取成功或数据超过 2 小时。
 *
 * 供 AppTask 或显示任务调用，不会发送 AT 命令，不能在中断上下文使用。
 */
uint8_t NetworkTask_GetLatestWeather(WeatherData_t *weather, uint32_t *age_seconds);


/**
 * @brief 网络任务对 UI 的只读状态快照（不直接读取 Core，避免跨任务竞争）。
 *
 * wifi_has_ip: 收到 WIFI GOT IP，且尚未观测到 WIFI DISCONNECT；
 *              不等同于互联网服务器可用。
 * sntp_synced: 本次 MCU 启动后，至少有一次 SNTP 校时成功写入 RTC。
 *              网络掉线不会让已经走时的 RTC 停止；此位保留同步历史。
 */
typedef struct
{
    uint8_t wifi_has_ip;
    uint8_t sntp_synced;
} NetworkTask_UiStatus_t;

/**
 * @brief 从 NetworkTask 发布的同步状态中原子复制快照。
 * @param[out] out 非 NULL 的结构体输出地址。
 * @return 1=成功；0=输出指针为空。
 * @note 只访问几个字节的已发布状态，不发 AT、不等待 UART、不触碰 LVGL。
 */
uint8_t NetworkTask_GetUiStatus(NetworkTask_UiStatus_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_TASK_H */

