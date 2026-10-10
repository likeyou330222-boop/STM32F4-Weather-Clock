#ifndef __APP_H__
#define __APP_H__

#include <stdint.h>  /* uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 保留你原来使用的 App_Status_t 返回状态，不修改现有状态值。 */
typedef enum
{
    APP_OK = 0,
    APP_ERROR_TASK_CREATE
} App_Status_t;

/* ==========================================================================
 * AppTask 接收的业务事件：每一个事件占一个二进制位。
 * ========================================================================== */

/* SensorTask 写入最新测量 Queue 后发送这个事件。 */
#define APP_EVENT_SENSOR_UPDATED     (1UL << 0)

/* NetworkTask 天气快照更新后发送这个事件；室外 LCD 显示下一阶段接入。 */
#define APP_EVENT_WEATHER_UPDATED    (1UL << 1)

/* 创建现有的 AppTask、SensorTask、NetworkTask；系统启动时调用一次。 */
App_Status_t App_Init(void);

/* 从普通 FreeRTOS 任务上下文发送事件。参数可以用 | 同时组合多个标志。 */
void App_NotifyEvent(uint32_t event_bits);

#ifdef __cplusplus
}
#endif

#endif /* __APP_H__ */


