#ifndef __RTC_H__
#define __RTC_H__


#include <stdint.h>


/*
 * ============================================================================
 * RTC Driver 返回状态(状态机)
 * ============================================================================
 */
typedef enum
{
    RTC_STATUS_OK = 0,//操作成功

    RTC_STATUS_INVALID_ARGUMENT, //调用函数时传入了非法参数

    RTC_STATUS_LSE_TIMEOUT, //32.768 kHz LSE 晶振启动超时

    RTC_STATUS_SYNC_ERROR, //RTC 与 APB 总线同步失败

    RTC_STATUS_INIT_ERROR, //RTC 外设初始化失败

    RTC_STATUS_SET_ERROR //日期或者时间写入失败

} RTC_Status_t;


/*
 * ============================================================================
 * 日期时间结构体
 * ============================================================================
 * 以后整个天气时钟统一使用这个结构表示时间。
 * 例如：
 * 2026-09-22 17:30:25 Tuesday  就可以表示成：
 * year    = 2026    month   = 9
 * day     = 22      hour    = 17
 * minute  = 30      second  = 25   weekday = 2
 */
typedef struct
{
    uint16_t year;  //完整年份

    uint8_t month;  //月份

    uint8_t day;  //日

    uint8_t hour;  //小时

    uint8_t minute;  //分

    uint8_t second;  //秒

    uint8_t weekday;  //星期

} RTC_DateTime_t;




/*
 * ============================================================================
 * RTC Driver API
 * ============================================================================
 */
RTC_Status_t RTC_Driver_Init(void);  //初始化 STM32 内部 RTC

uint8_t RTC_Driver_HasValidDateTime(void);  //检查 RTC 是否已经保存过有效日期时间 返回1设置过了 返回0则是未设置

RTC_Status_t RTC_Driver_SetDateTime(const RTC_DateTime_t *date_time);  //设置 RTC 日期和时间

RTC_Status_t RTC_Driver_GetDateTime(RTC_DateTime_t *date_time);  //读取当前 RTC 日期和时间



#endif /* __RTC_H__ */


