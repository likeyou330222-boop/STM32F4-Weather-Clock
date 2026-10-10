#include "weather_UI.h"

#include <stdio.h>
#include <stdint.h>

#include "ili9341_gfx.h"

#include "ili9341_assets.h"


/*
 * ============================================================================
 * 绘制基础启动界面
 * ============================================================================
 */
ILI9341_Status_t WeatherUI_DrawBootScreen(void)
{
    ILI9341_Status_t status;
    /*
     * ------------------------------------------------------------------------
     * 设置横屏
     * ------------------------------------------------------------------------
     */
    status = ILI9341_SetRotation(ILI9341_ROTATION_LANDSCAPE);
    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * ------------------------------------------------------------------------
     * 清屏
     * ------------------------------------------------------------------------
     */
    status = ILI9341_GFX_FillScreen(ILI9341_GFX_COLOR_BLACK);
    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * ------------------------------------------------------------------------
     * 显示标题
     * ------------------------------------------------------------------------
     */
    status = ILI9341_GFX_DrawString(
            10, 10,
            "WEATHER CLOCK",
            &ILI9341_ASSET_FONT_8X16,
            ILI9341_GFX_COLOR_GREEN,
            ILI9341_GFX_COLOR_BLACK,
            ILI9341_GFX_MODE_OPAQUE
        );

    return status;
}


/*
 * ============================================================================
 * 更新时间显示
 * ============================================================================
 */
ILI9341_Status_t WeatherUI_DrawTime(const RTC_DateTime_t *date_time)
{
    /*
     * 时间字符串：HH:MM:SS  一共 8 个可见字符。 C 字符串最后还需要： '\0' 所以数组至少需要： 8 + 1 = 9 Byte
     */
    char time_text[9];

    /*
     * 防止空指针。
     */
    if (date_time == NULL)
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * 原版使用 char time_text[9] 恰好保存 HH:MM:SS + '\0'。
     * 为了防止异常 RTC 数据产生三位数并截断显示，先严格验证时分秒范围。
     */
    if ((date_time->hour > 23U) ||
        (date_time->minute > 59U) ||
        (date_time->second > 59U))
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * 把：hour minute second 转换成： "12:03:08"
     */
    /*
     * 格式固定为 HH:MM:SS，直接填入 8 个 ASCII 字符 + '\0'。
     * 前面已验证 hour<=23/minute<=59/second<=59，因此这里不会溢出。
     * 相比 snprintf()，也减少了每秒格式化字符串的运行开销。
     */
    time_text[0] = (char)('0' + date_time->hour / 10U);
    time_text[1] = (char)('0' + date_time->hour % 10U);
    time_text[2] = ':';
    time_text[3] = (char)('0' + date_time->minute / 10U);
    time_text[4] = (char)('0' + date_time->minute % 10U);
    time_text[5] = ':';
    time_text[6] = (char)('0' + date_time->second / 10U);
    time_text[7] = (char)('0' + date_time->second % 10U);
    time_text[8] = '\0';


    /*
     * ------------------------------------------------------------------------
     * 更新时间区域
     * ------------------------------------------------------------------------
     */
    return ILI9341_GFX_DrawString(
        10,
        40,
        time_text,
        &ILI9341_ASSET_FONT_8X16,
        ILI9341_GFX_COLOR_WHITE,
        ILI9341_GFX_COLOR_BLACK,
        ILI9341_GFX_MODE_OPAQUE
    );
}


ILI9341_Status_t WeatherUI_DrawTemperatureHumidity(const AHT20_Measurement_t *measurement)
{
    ILI9341_Status_t status;

    char temperature_text[24];
    char humidity_text[24];

    int32_t temperature_x10;
    uint32_t humidity_x10;

    if (measurement == NULL)  //参数检查
    {
        return ILI9341_ERROR_PARAM;
    }

    temperature_x10 = (int32_t)(measurement->temperature_celsius * 10.0f); //将浮点温湿度转换成放大 10 倍的整数
    humidity_x10 = (uint32_t)(measurement->humidity_percent * 10.0f);

    if (temperature_x10 < 0)  //生成温度字符串
    {
        snprintf(
            temperature_text,
            sizeof(temperature_text),
            "Temp: -%ld.%01ld C",
            (long)(-temperature_x10 / 10),
            (long)(-temperature_x10 % 10)
        );
    }
    else
    {
        snprintf(
            temperature_text,
            sizeof(temperature_text),
            "Temp: %ld.%01ld C",
            (long)(temperature_x10 / 10),
            (long)(temperature_x10 % 10)
        );
    }

    snprintf(      //生成湿度字符串
        humidity_text,
        sizeof(humidity_text),
        "Humidity: %lu.%01lu %%",
        (unsigned long)(humidity_x10 / 10U),
        (unsigned long)(humidity_x10 % 10U)
    );

    status = ILI9341_GFX_DrawString(   //绘制温度
        10,
        90,
        temperature_text,
        &ILI9341_ASSET_FONT_12X24,
        ILI9341_GFX_COLOR_YELLOW,
        ILI9341_GFX_COLOR_BLACK,
        ILI9341_GFX_MODE_OPAQUE
    );

    if (status != ILI9341_OK)
    {
        return status;
    }

    status = ILI9341_GFX_DrawString(   //绘制湿度
        10,
        125,
        humidity_text,
        &ILI9341_ASSET_FONT_12X24,
        ILI9341_GFX_COLOR_CYAN,
        ILI9341_GFX_COLOR_BLACK,
        ILI9341_GFX_MODE_OPAQUE
    );

    if (status != ILI9341_OK)
    {
        return status;
    }


    return ILI9341_OK;
}




/* ========================================================================== */
/* 04. 室外天气数据的显示辅助函数                                             */
/* ========================================================================== */

/**
 * @brief 将 Open-Meteo 的 WMO 天气代码转换成短英文说明。
 *
 * 由于当前字库是 ASCII 英文字库，这里先采用短英文词组。
 * 以后增加汉字或天气图标时，只改 UI/Assets，不需要改 NetworkTask。
 *
 * 常见代码：0 晴天；1~3 多云；45/48 雾；51~67 降水；
 *          71~77 雪；80~86 阵雨/阵雪；95~99 雷暴。
 */
static const char *prvWeatherCodeText(uint8_t code)
{
    switch (code)
    {
        case 0U:  return "Clear";
        case 1U:  return "Mainly clear";
        case 2U:  return "Partly cloudy";
        case 3U:  return "Overcast";
        case 45U:
        case 48U: return "Fog";
        case 51U:
        case 53U:
        case 55U:
        case 56U:
        case 57U: return "Drizzle";
        case 61U:
        case 63U:
        case 65U:
        case 66U:
        case 67U: return "Rain";
        case 71U:
        case 73U:
        case 75U:
        case 77U: return "Snow";
        case 80U:
        case 81U:
        case 82U: return "Rain showers";
        case 85U:
        case 86U: return "Snow showers";
        case 95U:
        case 96U:
        case 99U: return "Thunderstorm";
        default: return "Unknown";
    }
}


/**
 * @brief 将 float 转换为 1 位小数的简短 ASCII 字符串。
 *
 * 不使用 printf 的 %f，减少不同 Keil C 运行库/浮点格式化配置带来的移植问题。
 * 与 AppTask 变化比较一致：乘 10 后转换为有符号整数，即向零截断。
 *
 * 此函数只接受已经经过 WeatherData_t 解析器范围校验的天气数值。
 */
static void prvFormatTenths(char *buffer, size_t length, float value)
{
    int32_t tenths;
    uint32_t magnitude;

    if ((buffer == NULL) || (length == 0U))
    {
        return;
    }

    tenths = (int32_t)(value * 10.0f);
    magnitude = (tenths < 0) ? (uint32_t)(-tenths) : (uint32_t)tenths;

    (void)snprintf(buffer, length, "%s%lu.%01lu",
                   (tenths < 0) ? "-" : "",
                   (unsigned long)(magnitude / 10U),
                   (unsigned long)(magnitude % 10U));
}


/* ========================================================================== */
/* 05. 绘制室外天气：只占用现有 320x240 横屏下方区域                          */
/* ========================================================================== */

/**
 * @brief 在屏幕下方显示秦皇岛室外天气。
 *
 * 当前工程已有的内容：
 *   标题：        y=10，  8x16 字体；
 *   时钟：        y=40，  8x16 字体；
 *   室内温度：    y=90， 12x24 字体；
 *   室内湿度：    y=125，12x24 字体。
 *
 * 因此将室外天气放到 y=158～239，完全不覆盖原有区域。
 * 使用 8x16 ASCII 字体，单行最多 40 字符（320/8）；字符串控制在 38 字符内。
 *
 * 每次天气真正变化时才调用本函数；先清理自己的显示区域，防止旧文本残留。
 * 无数据/过期时，同样清空该区域并显示不可用状态。
 *
 * 注意：只允许通过 LCD Service Job 来调用，不能从 NetworkTask 直接调用。
 */
ILI9341_Status_t WeatherUI_DrawOutdoorWeather(const WeatherData_t *weather, uint8_t valid)
{
    ILI9341_Status_t status;
    char line[40];
    char temp_text[16];
    char wind_text[16];

    if ((valid != 0U) && (weather == NULL))
    {
        return ILI9341_ERROR_PARAM;
    }

    /* 只清室外显示区（不是整屏 FillScreen），避免让 RTC/室内区域闪烁。 */
    status = ILI9341_GFX_FillRect(0, 158, 320U, 82U, ILI9341_GFX_COLOR_BLACK);
    if (status != ILI9341_OK)
    {
        return status;
    }

    status = ILI9341_GFX_DrawString(10, 158, "QHD OUTDOOR",
                                    &ILI9341_ASSET_FONT_8X16,
                                    ILI9341_GFX_COLOR_GREEN,
                                    ILI9341_GFX_COLOR_BLACK,
                                    ILI9341_GFX_MODE_OPAQUE);
    if (status != ILI9341_OK)
    {
        return status;
    }

    if (valid == 0U)
    {
        /* weather 已过期时不再显示旧温度，避免给人一种天气仍然有效的印象。 */
        return ILI9341_GFX_DrawString(10, 183, "Weather unavailable",
                                      &ILI9341_ASSET_FONT_8X16,
                                      ILI9341_GFX_COLOR_LIGHT_GRAY,
                                      ILI9341_GFX_COLOR_BLACK,
                                      ILI9341_GFX_MODE_OPAQUE);
    }

    /* 1. 温度和湿度：单位与 Open-Meteo 五字段 current 请求保持一致。 */
    prvFormatTenths(temp_text, sizeof(temp_text), weather->temperature);
    (void)snprintf(line, sizeof(line), "Temp:%s C  RH:%u%%",
                   temp_text, (unsigned int)weather->humidity);
    status = ILI9341_GFX_DrawString(10, 177, line,
                                    &ILI9341_ASSET_FONT_8X16,
                                    ILI9341_GFX_COLOR_YELLOW,
                                    ILI9341_GFX_COLOR_BLACK,
                                    ILI9341_GFX_MODE_OPAQUE);
    if (status != ILI9341_OK)
    {
        return status;
    }

    /* 2. 风速和昼夜；数字转文本时不用 %f，减少 C 库配置依赖。 */
    prvFormatTenths(wind_text, sizeof(wind_text), weather->wind_speed);
    (void)snprintf(line, sizeof(line), "Wind:%s km/h  %s",
                   wind_text, (weather->is_day != 0U) ? "DAY" : "NIGHT");
    status = ILI9341_GFX_DrawString(10, 196, line,
                                    &ILI9341_ASSET_FONT_8X16,
                                    ILI9341_GFX_COLOR_CYAN,
                                    ILI9341_GFX_COLOR_BLACK,
                                    ILI9341_GFX_MODE_OPAQUE);
    if (status != ILI9341_OK)
    {
        return status;
    }

    /* 3. 根据 WMO weather_code 显示简短天气描述。 */
    (void)snprintf(line, sizeof(line), "Sky:%s (%u)",
                   prvWeatherCodeText(weather->weather_code),
                   (unsigned int)weather->weather_code);
    return ILI9341_GFX_DrawString(10, 215, line,
                                  &ILI9341_ASSET_FONT_8X16,
                                  ILI9341_GFX_COLOR_WHITE,
                                  ILI9341_GFX_COLOR_BLACK,
                                  ILI9341_GFX_MODE_OPAQUE);
}

