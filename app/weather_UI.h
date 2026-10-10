#ifndef __WEATHER_UI_H__
#define __WEATHER_UI_H__



#include "ili9341.h"

#include "rtc.h"

#include "aht20.h"

#include "esp_at.h"    /* WeatherData_t：天气数据结构体，UI 不直接发 AT 命令。 */


/*
 * ============================================================================
 * Weather UI 对外接口
 * ============================================================================
 */
ILI9341_Status_t WeatherUI_DrawBootScreen(void);//绘制天气时钟的基础启动界面 这个函数最终应该在 LCD Service Task 中执行，不要由普通 Application Task 直接调用。

ILI9341_Status_t WeatherUI_DrawTime(const RTC_DateTime_t *date_time);//更新屏幕上的当前时间

ILI9341_Status_t WeatherUI_DrawTemperatureHumidity(const AHT20_Measurement_t *measurement);




/*
 * 绘制室外天气区域（仅在 LCD Service Task 中调用）。
 *
 * weather：valid=1 时必须指向已通过 Core 校验的 WeatherData_t。
 * valid：  1=显示天气信息；0=清除过时天气并显示“Unavailable”。
 *
 * 当前 UI 使用已有 8x16 ASCII 字库；不需要新增汉字字库或天气图片。
 */
ILI9341_Status_t WeatherUI_DrawOutdoorWeather(const WeatherData_t *weather, uint8_t valid);


#endif /*__WEATHER_UI_H__*/


