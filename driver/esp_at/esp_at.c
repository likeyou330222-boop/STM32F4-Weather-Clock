/**
 * @file    esp_at.c
 * @brief   ESP32-C3 ESP-AT 协议核心层（Core）
 * @author  Weather Clock Project
 *
 * @details
 * 本文件负责“理解 ESP-AT 协议”，不直接操作 USART / DMA 寄存器。
 *
 * 模块分工：
 *   NetworkTask（应用层）：决定何时发 AT 命令、等待多久，以及如何使用结果。
 *   ESP-AT Core（本文件）：组装命令、识别响应、解析 Wi-Fi / SNTP / 天气数据。
 *   ESP-AT Port（底层）：USART2、TX DMA、RX 中断及 RX Ring Buffer。
 *
 * 正常数据流：
 *   发送：NetworkTask -> ESP_AT_StartCommand() -> Port_WriteAsync() -> ESP32-C3
 *   接收：ESP32-C3 -> USART2 RX ISR -> Port Ring Buffer -> ESP_AT_Process()
 *                             -> prvConsumeByte() -> 协议解析 -> 查询接口
 *
 * 约束：
 *   1. 本模块由 NetworkTask 串行调用；一次只处理一条等待响应的 AT 命令。
 *   2. ESP_AT_StartCommand() 仅启动异步 DMA 发送，不阻塞等待回复。
 *   3. 两个静态发送缓冲区交替使用，保证 DMA 传输期间源内存有效。
 *   4. 普通 AT 响应按 CRLF 分行；HTTP 正文按 +HTTPCLIENT:<长度>, 逐字节接收。
 *   5. HTTP 正文使用固定 1024 字节缓存，主要支持当前 Open-Meteo JSON。
 *   6. 超时后进入 TIMEOUT 隔离态：等待原命令迟到的终止响应后才解锁。
 *      如果原命令始终没有终止响应，则不冒险发送新命令；需要重启 ESP。
 *   7. 天气解析从 current 对象读取五个标量字段；不是通用 JSON 库。
 *   8. 识别 WIFI DISCONNECT/WIFI GOT IP 异步事件，供 NetworkTask 决定重连。
 *
 * 说明：程序状态变量全部为 private static，不向其他源文件直接暴露。
 */

/* ========================================================================== */
/* 1. 头文件：本模块接口、底层接口、C 标准库                               */
/* ========================================================================== */
#include "esp_at.h"           /* Core 对外接口、状态码、网络时间结构体 */
#include "esp_at_port.h"      /* USART2 / DMA / RX Ring Buffer 硬件抽象 */

#include <stddef.h>            /* size_t、NULL */
#include <stdint.h>            /* uint8_t、uint16_t */
#include <stdio.h>             /* ConsoleLog_Printf()、sscanf() */
#include <stdlib.h>            /* strtoul()、strtod() */
#include <string.h>            /* memcpy()、strcmp()、strstr() 等 */
#include "console_log.h"  /* FreeRTOS Mutex：整条 USART1 日志互斥输出 */

/* ========================================================================== */
/* 2. 配置常量：集中管理可调整的容量                                         */
/* ========================================================================== */

/* ESP_AT_Process() 每次最多从底层接收环形缓冲区取出多少字节。 */
#define ESP_AT_RX_CHUNK_SIZE        64U

/* 仅用于普通 AT 响应行，包含最后的字符串终止符 '\0'。 */
#define ESP_AT_LINE_BUFFER_SIZE     512U

/* 单个 TX DMA 缓冲区：命令正文 + '\r' + '\n' + '\0'。 */
#define ESP_AT_TX_BUFFER_SIZE       256U

/* HTTP 正文缓存：允许保存至多 1023 字节，并预留 1 字节作为 '\0'。 */
#define ESP_AT_HTTP_BUFFER_SIZE     1024U

/* AT 协议中要识别的固定前缀；集中定义，避免到处重复填写前缀长度。 */
#define ESP_AT_WIFI_MODE_PREFIX     "+CWMODE:"
#define ESP_AT_SNTP_TIME_PREFIX     "+CIPSNTPTIME:"
#define ESP_AT_HTTP_DATA_PREFIX     "+HTTPCLIENT:"
#define ESP_AT_HTTP_COMMAND_PREFIX  "AT+HTTPCLIENT="

/* ========================================================================== */
/* 3. 内部类型与状态变量：按用途归类                                         */
/* ========================================================================== */

/**
 * @brief RX 字节解析模式。
 *
 * LINE：正常 AT 响应，使用 CRLF 判断一行结束。
 * HTTP_DATA：已经识别数据块长度，后续指定数量的字节属于 HTTP 正文；
 *            此时包括 '\r' 和 '\n' 在内的所有字节都不可当作行结束符。
 */
typedef enum
{
    ESP_AT_RX_MODE_LINE = 0,
    ESP_AT_RX_MODE_HTTP_DATA
} ESP_AT_RxMode_t;

/* ----- 3.1 核心命令状态 ------------------------------------------------- */
static uint8_t s_initialized = 0U;  /* ESP_AT_Init() 是否成功完成 */
static ESP_AT_CommandState_t s_command_state = ESP_AT_COMMAND_IDLE;

/* ----- 3.2 普通 AT 行接收 ------------------------------------------------ */
static char s_line_buffer[ESP_AT_LINE_BUFFER_SIZE];
static size_t s_line_length = 0U;  /* 已保存的实际字符数，不包含 '\0' */
static ESP_AT_RxMode_t s_rx_mode = ESP_AT_RX_MODE_LINE;

/* ----- 3.3 命令发送：两块静态 DMA 缓冲区交替使用 ------------------------- */
static uint8_t s_tx_buffers[2][ESP_AT_TX_BUFFER_SIZE];
static uint8_t s_next_tx_index = 0U;  /* 只能是 0 或 1，发送启动成功后切换 */

/* ----- 3.4 Wi-Fi Station 模式查询 ---------------------------------------- */
/* -1：尚无有效查询结果；0～3：ESP-AT 定义的 Wi-Fi 模式值。 */
static int s_wifi_mode = -1;
/* 收到 WIFI GOT IP 记为 1；收到 WIFI DISCONNECT 记为 0。
 * 本标志反映 ESP-AT 已报告的状态，不代替真正的联网探测。 */
static uint8_t s_wifi_has_ip = 0U;

/* ----- 3.5 SNTP 网络时间查询 --------------------------------------------- */
static ESP_AT_NetworkTime_t s_sntp_time;
static uint8_t s_sntp_query_active = 0U; /* 当前命令是否为 AT+CIPSNTPTIME? */
static uint8_t s_sntp_valid = 0U;        /* 响应是否解析出合法日期时间 */

/* ----- 3.6 HTTP 响应正文及天气数据 -------------------------------------- */
static char s_http_body[ESP_AT_HTTP_BUFFER_SIZE];
static size_t s_http_length = 0U;           /* 已保存的 HTTP 正文字节总数 */
static unsigned long s_http_remaining = 0UL;/* 当前块还剩多少字节未接收 */
static uint8_t s_http_active = 0U;         /* 当前命令是否是 AT+HTTPCLIENT */
static uint8_t s_http_seen = 0U;           /* 是否至少收到过一个数据块头 */
static uint8_t s_http_overflow = 0U;       /* 数据溢出或遇到不支持的 NUL 字节 */

/* 最近一次完整天气记录。仅在五个字段全部解析成功后整体更新。 */
static WeatherData_t s_weather_data;
static uint8_t s_weather_valid = 0U;       /* 本次 HTTP 的五字段天气是否有效 */

/* ========================================================================== */
/* 4. 私有函数声明：从函数名称即可看出职责                                   */
/* ========================================================================== */

/* 接收状态清理 */
static void prvResetLineBuffer(void);
static void prvResetHTTPContext(void);

/* 数据解析 */
static uint8_t prvParseSNTPTime(const char *line, ESP_AT_NetworkTime_t *time);
static uint8_t prvParseWeatherNumber(const char *begin, const char *end,
                                     const char *key, double *value);
static uint8_t prvParseWeatherData(const char *json, WeatherData_t *weather);

/* AT / HTTP 收包状态机 */
static uint8_t prvTryStartHTTPBlock(void);
static void prvReceiveHTTPDataByte(uint8_t byte);
static ESP_AT_Status_t prvConsumeByte(uint8_t byte);
static void prvHandleLine(const char *line);

/* ========================================================================== */
/* 5. 公用的内部小工具：重置缓存与状态                                       */
/* ========================================================================== */

/**
 * @brief 清空尚未完成的普通 AT 响应行。
 *
 * @note s_line_length 保存字符数，s_line_buffer 的最后一个字节始终预留给
 *       '\0'。清零后，即使下一次没有接收任何字符，它也是有效的空字符串。
 */
static void prvResetLineBuffer(void)
{
    s_line_length = 0U;
    s_line_buffer[0] = '\0';
}

/**
 * @brief 为下一条命令重置 HTTP 相关的临时数据。
 *
 * @note 这里只清空 HTTP 缓存“使用长度”和标志，不需要 memset() 整个 1024
 *       字节数组。因为后续代码只读取 [0, s_http_length) 范围的数据，
 *       并始终在有效正文末尾补上 '\0'。
 */
static void prvResetHTTPContext(void)
{
    s_http_length = 0U;
    s_http_remaining = 0UL;
    s_http_seen = 0U;
    s_http_overflow = 0U;
    s_http_body[0] = '\0';
    s_rx_mode = ESP_AT_RX_MODE_LINE;
}

/* ========================================================================== */
/* 6. SNTP 响应解析：英文日期 -> 适合 STM32 RTC 的字段                       */
/* ========================================================================== */

/**
 * @brief 解析 AT+CIPSNTPTIME? 的一个完整响应行。
 *
 * 示例：+CIPSNTPTIME:Fri Oct  9 09:25:36 2026
 * 结果：year=2026, month=10, day=9, hour=9, minute=25, second=36, weekday=5。
 *
 * @param line  完整、以 '\0' 结尾的 SNTP 响应行。
 * @param time  由调用者提供的输出结构体地址。
 * @return 1=解析成功且字段合法；0=参数或内容无效。
 *
 * 检查项：字段数量、非法额外字符、年份范围、时分秒范围、闰年、月天数。
 * 星期编号：周一=1，周日=7，与 STM32 RTC 一致。
 */
static uint8_t prvParseSNTPTime(const char *line, ESP_AT_NetworkTime_t *time)
{
    static const char *const months[12] =
    {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    static const char *const weekdays[7] =
    {
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
    };
    static const uint8_t month_days[12] =
    {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };

    char weekday_text[4];
    char month_text[4];
    unsigned int year;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    uint8_t month = 0U;
    uint8_t weekday = 0U;
    uint8_t max_day;
    size_t index;
    int parsed_count;
    int consumed = 0;

    if ((line == NULL) || (time == NULL))
    {
        return 0U;
    }

    /*
     * %3s：最多读取三个字母，避免溢出大小为 4 的本地数组。
     * %u ：提取无符号十进制整数。
     * %n ：仅记录到目前为止已读取多少字符，不计入 parsed_count。
     * 如果恰好识别七项，且 %n 后就是 '\0'，说明没有混入其他字符。
     */
    parsed_count = sscanf(line, "+CIPSNTPTIME:%3s %3s %u %u:%u:%u %u%n",
                          weekday_text, month_text, &day, &hour, &minute,
                          &second, &year, &consumed);

    if ((parsed_count != 7) || (consumed <= 0) || (line[consumed] != '\0'))
    {
        return 0U;
    }

    /* 将英文月份映射为 1～12；找不到时 month 保持为 0，随后拒绝。 */
    for (index = 0U; index < 12U; ++index)
    {
        if (strcmp(month_text, months[index]) == 0)
        {
            month = (uint8_t)(index + 1U);
            break;
        }
    }

    /* 将英文星期映射为 1～7（周一～周日）。 */
    for (index = 0U; index < 7U; ++index)
    {
        if (strcmp(weekday_text, weekdays[index]) == 0)
        {
            weekday = (uint8_t)(index + 1U);
            break;
        }
    }

    /* RTC 驱动使用 2000～2099 的完整年份、24 小时时间制。 */
    if ((year < 2000U) || (year > 2099U) ||
        (month == 0U) || (weekday == 0U) ||
        (hour > 23U) || (minute > 59U) || (second > 59U))
    {
        return 0U;
    }

    /* 先取普通年份该月份的最大天数，再对闰年二月单独修正。 */
    max_day = month_days[month - 1U];
    if ((month == 2U) && (year % 4U == 0U) &&
        ((year % 100U != 0U) || (year % 400U == 0U)))
    {
        max_day = 29U;
    }

    if ((day < 1U) || (day > max_day))
    {
        return 0U;
    }

    /* 全部验证通过后才写输出，避免出现“部分字段被更新”的情况。 */
    time->year = (uint16_t)year;
    time->month = month;
    time->day = (uint8_t)day;
    time->hour = (uint8_t)hour;
    time->minute = (uint8_t)minute;
    time->second = (uint8_t)second;
    time->weekday = weekday;
    return 1U;
}

/* ========================================================================== */
/* 7. 天气 JSON 解析：current 对象中的五个实时天气字段                       */
/* ========================================================================== */

/**
 * @brief 从 current 对象中安全查找一个数字字段。
 *
 * @param begin current 对象 '{' 后的第一个字符位置。
 * @param end   current 对象的结束 '}' 位置，不包括结束花括号。
 * @param key   要匹配的带双引号字段名，例如 "\"temperature_2m\""。
 * @param value 解析成功后写入双精度临时结果。
 * @return 1=成功找到合法数字；0=字段不存在、格式错误或超出对象范围。
 *
 * 与直接 strstr(json, "temperature_2m") 不同：
 *   1. 只在 current 对象内查找，避免误读取 current_units 中的同名字段；
 *   2. 必须存在 ':' 分隔符，且数字后只能为逗号或对象结尾；
 *   3. 支持字段之间的空格、回车、换行、制表符；
 *   4. 只读取 JSON 数字，不接受 null、NaN、Infinity 或引号字符串。
 *
 * 这是针对 Open-Meteo 的轻量化字段解析，不是通用 JSON 词法分析器；
 * 如果以后 current 内加入嵌套对象或复杂字符串，需要换成正式 JSON 解析器。
 */
static uint8_t prvParseWeatherNumber(const char *begin, const char *end,
                                     const char *key, double *value)
{
    const char *field;
    const char *number_begin;
    char *number_end;
    double number;

    if ((begin == NULL) || (end == NULL) || (key == NULL) ||
        (value == NULL) || (begin >= end))
    {
        return 0U;
    }

    /* strstr() 在 NUL 结尾的正文中查找，结果必须落在 current 对象范围内。 */
    field = strstr(begin, key);
    if ((field == NULL) || (field >= end) ||
        ((size_t)(end - field) < strlen(key)))
    {
        return 0U;
    }

    field += strlen(key);
    while ((field < end) && ((*field == ' ') || (*field == '\r') ||
                              (*field == '\n') || (*field == '\t')))
    {
        ++field;
    }
    if ((field >= end) || (*field != ':'))
    {
        return 0U;
    }

    /* 冒号之后跳过空白，得到真正数值的首字符。 */
    ++field;
    while ((field < end) && ((*field == ' ') || (*field == '\r') ||
                              (*field == '\n') || (*field == '\t')))
    {
        ++field;
    }
    if ((field >= end) || ((*field != '-') &&
        ((*field < '0') || (*field > '9'))))
    {
        return 0U;
    }

    number_begin = field;
    number = strtod(number_begin, &number_end);
    if ((number_end == number_begin) || (number_end > end))
    {
        return 0U;
    }

    /* 数字结束位置必须位于字段分隔符之前。 */
    while ((number_end < end) && ((*number_end == ' ') ||
           (*number_end == '\r') || (*number_end == '\n') ||
           (*number_end == '\t')))
    {
        ++number_end;
    }
    if ((number_end != end) && (*number_end != ','))
    {
        return 0U;
    }

    /* 范围检查会拒绝无穷大；自反性检查会拒绝 NaN。 */
    if ((number != number) || (number < -1000000.0) || (number > 1000000.0))
    {
        return 0U;
    }

    *value = number;
    return 1U;
}

/**
 * @brief 将完整的 Open-Meteo HTTP JSON 正文解析成 WeatherData_t。
 *
 * @param json    以 '\0' 结束的完整天气 JSON 文本；必须来自已收齐的 HTTP 数据。
 * @param weather 调用者提供的输出结构体指针。
 * @return 1=五项天气数据全部有效；0=字段缺失、格式错误或数值越界。
 *
 * 更新策略：
 *   先把所有字段写入局部 parsed，检查全部通过后才一次性赋给 *weather。
 *   失败时保持调用者原有结构体不变，避免产生混合新旧字段的伪天气。
 *
 * Open-Meteo current 对象目前包括：
 *   temperature_2m        摄氏温度，允许负数；
 *   relative_humidity_2m   百分比，应为 0～100 的整数；
 *   weather_code           WMO 天气代码，当前按 0～99 的整数接受；
 *   wind_speed_10m         风速，默认 km/h，不能为负数；
 *   is_day                 白天 1、夜间 0。
 */
static uint8_t prvParseWeatherData(const char *json, WeatherData_t *weather)
{
    const char *current;
    const char *current_end;
    size_t json_length;
    double temperature;
    double humidity;
    double weather_code;
    double wind_speed;
    double is_day;
    WeatherData_t parsed;

    if ((json == NULL) || (weather == NULL))
    {
        return 0U;
    }

    /* HTTP 正文必须看起来是完整的 JSON 对象。 */
    json_length = strlen(json);
    if ((json_length < 2U) || (json[0] != '{') ||
        (json[json_length - 1U] != '}'))
    {
        return 0U;
    }

    /* 必须匹配带双引号的 "current"，而不是 "current_units"。 */
    current = strstr(json, "\"current\"");
    if (current == NULL)
    {
        return 0U;
    }
    current += sizeof("\"current\"") - 1U;

    /* 容许字段名与对象花括号之间出现空白字符。 */
    while ((*current == ' ') || (*current == '\r') ||
           (*current == '\n') || (*current == '\t'))
    {
        ++current;
    }
    if (*current != ':')
    {
        return 0U;
    }
    ++current;
    while ((*current == ' ') || (*current == '\r') ||
           (*current == '\n') || (*current == '\t'))
    {
        ++current;
    }
    if (*current != '{')
    {
        return 0U;
    }
    ++current;

    /* 本接口的 current 对象是扁平标量对象，结束位置即其第一个 '}'。 */
    current_end = strchr(current, '}');
    if (current_end == NULL)
    {
        return 0U;
    }

    /*
     * 逐个读取，故意不把五个条件压成一个 if：
     * 某一项失败时能直接在串口看到字段名，现场调试不需要反复打印整段 JSON。
     */
    if (prvParseWeatherNumber(current, current_end, "\"temperature_2m\"", &temperature) == 0U)
    {
        ConsoleLog_Printf("[ESP-AT] Weather JSON invalid: temperature_2m\r\n");
        return 0U;
    }
    if (prvParseWeatherNumber(current, current_end, "\"relative_humidity_2m\"", &humidity) == 0U)
    {
        ConsoleLog_Printf("[ESP-AT] Weather JSON invalid: relative_humidity_2m\r\n");
        return 0U;
    }
    if (prvParseWeatherNumber(current, current_end, "\"weather_code\"", &weather_code) == 0U)
    {
        ConsoleLog_Printf("[ESP-AT] Weather JSON invalid: weather_code\r\n");
        return 0U;
    }
    if (prvParseWeatherNumber(current, current_end, "\"wind_speed_10m\"", &wind_speed) == 0U)
    {
        ConsoleLog_Printf("[ESP-AT] Weather JSON invalid: wind_speed_10m\r\n");
        return 0U;
    }
    if (prvParseWeatherNumber(current, current_end, "\"is_day\"", &is_day) == 0U)
    {
        ConsoleLog_Printf("[ESP-AT] Weather JSON invalid: is_day\r\n");
        return 0U;
    }

    /* 温度和风速是浮点数；湿度、天气代码、昼夜标记必须是整数。 */
    if ((temperature < -100.0) || (temperature > 70.0) ||
        (humidity < 0.0) || (humidity > 100.0) ||
        (humidity != (double)(uint8_t)humidity) ||
        (weather_code < 0.0) || (weather_code > 99.0) ||
        (weather_code != (double)(uint8_t)weather_code) ||
        (wind_speed < 0.0) || (wind_speed > 300.0) ||
        ((is_day != 0.0) && (is_day != 1.0)))
    {
        ConsoleLog_Printf("[ESP-AT] Weather JSON numeric range/type invalid\r\n");
        return 0U;
    }

    parsed.temperature = (float)temperature;
    parsed.humidity = (uint8_t)humidity;
    parsed.weather_code = (uint8_t)weather_code;
    parsed.wind_speed = (float)wind_speed;
    parsed.is_day = (uint8_t)is_day;

    /* 全部有效才提交结果，不提前修改 *weather。 */
    *weather = parsed;
    return 1U;
}

/* ========================================================================== */
/* 8. HTTP 长度数据块处理：头部检测与正文保存                                 */
/* ========================================================================== */

/**
 * @brief 尝试把当前行缓存解释为“尚未收到逗号”的 HTTP 数据块头。
 *
 * 调用时机：s_line_buffer 中已保存 +HTTPCLIENT:数字，当前新字节为逗号。
 * 示例：+HTTPCLIENT:323, 后面紧跟 323 个正文原始字节。
 *
 * @return 1=成功识别并处理 HTTP 块头；0=不是合法块头，继续普通行解析。
 *
 * 这里不能用 strlen() 判断正文长度，因为正文可能有换行或分多次到达。
 */
static uint8_t prvTryStartHTTPBlock(void)
{
    const char *number_text;
    char *endptr;
    unsigned long block_size;
    size_t prefix_length = sizeof(ESP_AT_HTTP_DATA_PREFIX) - 1U;

    /* 只有 HTTP 命令正在等待回复时，才允许识别 HTTP 数据块。 */
    if ((s_http_active == 0U) ||
        ((s_command_state != ESP_AT_COMMAND_WAITING) &&
         (s_command_state != ESP_AT_COMMAND_TIMEOUT)) ||
        (s_line_length <= prefix_length) ||
        (strncmp(s_line_buffer, ESP_AT_HTTP_DATA_PREFIX, prefix_length) != 0))
    {
        return 0U;
    }

    /* 普通 AT 行缓冲区仍有空间存放结束符，临时形成 C 字符串。 */
    s_line_buffer[s_line_length] = '\0';
    number_text = s_line_buffer + prefix_length;

    if ((*number_text < '0') || (*number_text > '9'))
    {
        return 0U;
    }

    /* 将数据块头的数字部分转换为本块的字节数。 */
    block_size = strtoul(number_text, &endptr, 10);
    if ((endptr == number_text) || (*endptr != '\0'))
    {
        return 0U;
    }

    s_http_seen = 1U;
    s_http_remaining = block_size;
    prvResetLineBuffer(); /* 头部不属于 JSON 正文，不能写进 HTTP body */

    /* 0 字节数据块不需要进入正文模式；其余情况开始按字节计数。 */
    if (s_http_remaining > 0UL)
    {
        s_rx_mode = ESP_AT_RX_MODE_HTTP_DATA;
    }
    return 1U;
}

/**
 * @brief 保存一个 HTTP 正文字节，并更新当前数据块剩余长度。
 *
 * @param byte 从 Port Ring Buffer 读取的原始字节。
 *
 * 本函数仅由 HTTP_DATA 模式调用，保证 s_http_remaining > 0。
 * 对于 '\r'、'\n'：按普通数据保存，不能直接丢弃。
 * 对于超过缓存的字节：只记录溢出并继续消耗字节，确保接收状态不失步。
 * 对于 '\0'：当前只处理文本 JSON，标记无效，但仍消费该字节。
 */
static void prvReceiveHTTPDataByte(uint8_t byte)
{
    if ((s_http_overflow == 0U) &&
        (s_http_length < (ESP_AT_HTTP_BUFFER_SIZE - 1U)) && (byte != 0U))
    {
        s_http_body[s_http_length] = (char)byte;
        ++s_http_length;
    }
    else
    {
        s_http_overflow = 1U;
    }

    --s_http_remaining;
    if (s_http_remaining == 0UL)
    {
        /* 本数据块恰好接收完毕；后续字节重新按 AT 行或下一块头处理。 */
        s_http_body[s_http_length] = '\0';
        s_rx_mode = ESP_AT_RX_MODE_LINE;
    }
}

/* ========================================================================== */
/* 9. 完整 AT 响应行的处理：先处理数据行，再处理 OK / ERROR                   */
/* ========================================================================== */

/**
 * @brief 处理一条完整的普通 AT 文本响应行。
 *
 * @param line 以 '\0' 结束的完整行（不包括 CRLF）。
 *
 * 分支顺序：
 *   1. 普通日志（包括 +TIME_UPDATED 这类异步通知）。
 *   2. Wi-Fi 模式查询结果 +CWMODE:n。
 *   3. SNTP 查询结果 +CIPSNTPTIME:...
 *   4. OK：若为 HTTP 请求，先解析已完整接收的 JSON，再置命令完成。
 *   5. ERROR / FAIL：置命令失败。
 *
 * 注意：HTTP 正文不通过本函数，避免 JSON 内部换行打断正文。
 */
static void prvHandleLine(const char *line)
{
    size_t prefix_length;

    /*
     * ESP32-C3 启动时 UART 可能出现非文本二进制内容（例如 ROM 启动信息）。
     * 不把非打印字符原样发往调试串口，避免污染日志和终端显示。
     * 一旦 AT 固件就绪，正常响应应为可打印的 ASCII 及合法 UTF-8 文本。
     * 当前代码只对 C0 控制字符做过滤，不尝试识别中文编码。
     */
    {
        const unsigned char *scan = (const unsigned char *)line;
        uint8_t printable = 1U;
        while (*scan != 0U)
        {
            if (((*scan < 0x20U) && (*scan != (unsigned char)'\t')) || (*scan == 0x7FU))
            {
                printable = 0U;
                break;
            }
            ++scan;
        }
        if (printable != 0U)
        {
            ConsoleLog_Printf("[ESP-AT RX] %s\r\n", line);
        }
        else
        {
            ConsoleLog_Printf("[ESP-AT] Ignored non-text response line\r\n");
        }
    }

    /*
     * Wi-Fi 异步事件（URC）不一定在 AT 命令等待期间发生。
     * 必须在 WAITING 状态检查之前处理，才不会漏掉运行中的掉线通知。
     */
    if (strcmp(line, "WIFI DISCONNECT") == 0)
    {
        s_wifi_has_ip = 0U;
    }
    else if (strcmp(line, "WIFI GOT IP") == 0)
    {
        s_wifi_has_ip = 1U;
    }

    /*
     * TIMEOUT 隔离：旧命令真正返回终止帧后才恢复 IDLE。
     * 不把迟到的 OK/ERROR 当成下一条命令的结果。
     * 若永远没有终止帧，隔离态持续，不能盲目重试。
     */
    if (s_command_state == ESP_AT_COMMAND_TIMEOUT)
    {
        if ((strcmp(line, "OK") == 0) || (strcmp(line, "ERROR") == 0) ||
            (strcmp(line, "FAIL") == 0))
        {
            s_command_state = ESP_AT_COMMAND_IDLE;
            s_http_active = 0U;
            s_sntp_query_active = 0U;
            s_weather_valid = 0U;
            s_sntp_valid = 0U;
            prvResetHTTPContext();
            ConsoleLog_Printf("[ESP-AT] Late terminal response drained; session ready\r\n");
        }
        return;
    }

    /* 空闲时可报告 URC，但不能当作某条命令的应答。 */
    if (s_command_state != ESP_AT_COMMAND_WAITING)
    {
        return;
    }

    /* ----- 9.1 解析 AT+CWMODE? 的 Station / SoftAP 模式 ------------------ */
    prefix_length = sizeof(ESP_AT_WIFI_MODE_PREFIX) - 1U;
    if (strncmp(line, ESP_AT_WIFI_MODE_PREFIX, prefix_length) == 0)
    {
        if ((line[prefix_length] >= '0') && (line[prefix_length] <= '3') &&
            (line[prefix_length + 1U] == '\0'))
        {
            s_wifi_mode = (int)(line[prefix_length] - '0');
        }
        return;
    }

    /* ----- 9.2 解析 SNTP 时间查询数据行 --------------------------------- */
    prefix_length = sizeof(ESP_AT_SNTP_TIME_PREFIX) - 1U;
    if ((s_sntp_query_active != 0U) &&
        (strncmp(line, ESP_AT_SNTP_TIME_PREFIX, prefix_length) == 0))
    {
        s_sntp_valid = prvParseSNTPTime(line, &s_sntp_time);
        return;
    }

    /* ----- 9.3 收到 OK，标志一条 AT 命令完成 ---------------------------- */
    if (strcmp(line, "OK") == 0)
    {
        if (s_http_active != 0U)
        {
            /*
             * 必须已接收过 HTTP 数据块、未发生溢出，且所有正文已收齐。
             * 先验证 HTTP 数据，再公布命令 OK，避免 NetworkTask 过早读结果。
             */
            s_weather_valid = 0U;
            if ((s_http_seen != 0U) && (s_http_overflow == 0U) &&
                (s_rx_mode == ESP_AT_RX_MODE_LINE) &&
                (s_http_remaining == 0UL) && (s_http_length > 0U))
            {
                s_weather_valid = prvParseWeatherData(s_http_body, &s_weather_data);
            }

            ConsoleLog_Printf("[ESP-AT] HTTP body bytes: %u, weather valid: %u, overflow: %u\r\n",
                   (unsigned int)s_http_length,
                   (unsigned int)s_weather_valid,
                   (unsigned int)s_http_overflow);
        }

        s_command_state = ESP_AT_COMMAND_OK;
        return;
    }

    /* ----- 9.4 ERROR / FAIL，标志命令失败 ------------------------------- */
    if ((strcmp(line, "ERROR") == 0) || (strcmp(line, "FAIL") == 0))
    {
        s_weather_valid = 0U;
        s_command_state = ESP_AT_COMMAND_ERROR;
        return;
    }
}

/* ========================================================================== */
/* 10. RX 逐字节状态机：统一入口，只做分发                                   */
/* ========================================================================== */

/**
 * @brief 处理一个从底层环形缓冲区取出的字节。
 *
 * @param byte 原始接收字节。
 * @return ESP_AT_OK=已处理；ESP_AT_ERROR_LINE_TOO_LONG=普通响应行超长。
 *
 * 与旧代码的关键区别：
 *   旧：任何数据都先判断 CR/LF；HTTP JSON 被错误地当作一整行。
 *   新：如果进入 HTTP_DATA 模式，优先按长度保存正文，CR/LF 不特殊处理。
 *
 * 本函数在 NetworkTask 上下文中运行，不在 USART2 ISR 中执行。
 */
static ESP_AT_Status_t prvConsumeByte(uint8_t byte)
{
    if (s_rx_mode == ESP_AT_RX_MODE_HTTP_DATA)
    {
        prvReceiveHTTPDataByte(byte);
        return ESP_AT_OK;
    }

    /*
     * 普通 AT 响应中，检查“逗号”是否结束了 +HTTPCLIENT:<size> 数据头。
     * 注意：此时逗号本身是头部和正文的分隔符，不属于 JSON 正文。
     */
    if ((byte == ',') && (prvTryStartHTTPBlock() != 0U))
    {
        return ESP_AT_OK;
    }

    /* 普通 AT 文本行采用 CRLF 结束；CR 直接忽略。 */
    if (byte == '\r')
    {
        return ESP_AT_OK;
    }

    if (byte == '\n')
    {
        /* 忽略 AT 帧之间的空行，如 \r\n\r\n。 */
        if (s_line_length == 0U)
        {
            return ESP_AT_OK;
        }

        s_line_buffer[s_line_length] = '\0';
        prvHandleLine(s_line_buffer);
        prvResetLineBuffer();
        return ESP_AT_OK;
    }

    /* 最后始终给字符串终止符 '\0' 预留一个位置，防止越界写入。 */
    if (s_line_length >= (ESP_AT_LINE_BUFFER_SIZE - 1U))
    {
        prvResetLineBuffer();
        return ESP_AT_ERROR_LINE_TOO_LONG;
    }

    s_line_buffer[s_line_length] = (char)byte;
    ++s_line_length;
    return ESP_AT_OK;
}

/* ========================================================================== */
/* 11. 初始化与命令发送：应用层最常调用的核心操作                             */
/* ========================================================================== */

/**
 * @brief 初始化 ESP-AT Core，并调用 Port 初始化 USART2/DMA。
 *
 * @return ESP_AT_OK=初始化成功；ESP_AT_ERROR_PORT=底层初始化失败。
 *
 * 重复调用时直接返回 OK，不清除已经工作的状态机；
 * 如未来需要重新初始化，应单独设计 Deinit/Reset，不混在 Init 内。
 */
ESP_AT_Status_t ESP_AT_Init(void)
{
    ESP_AT_PortStatus_t port_status;

    if (s_initialized != 0U)
    {
        return ESP_AT_OK;
    }

    port_status = ESP_AT_Port_Init();
    if (port_status != ESP_AT_PORT_OK)
    {
        return ESP_AT_ERROR_PORT;
    }

    prvResetLineBuffer();
    prvResetHTTPContext();
    s_command_state = ESP_AT_COMMAND_IDLE;
    s_wifi_mode = -1;
    s_wifi_has_ip = 0U;
    s_sntp_query_active = 0U;
    s_sntp_valid = 0U;
    s_http_active = 0U;
    s_weather_valid = 0U;
    s_next_tx_index = 0U;
    s_initialized = 1U;

    return ESP_AT_OK;
}

/**
 * @brief 初始 AT 配置阶段的受控软件重握手入口。
 *
 * 仅在首次联网、尚未开始 Wi-Fi/SNTP/HTTP 请求时由 NetworkTask 调用。
 * 调用方必须先等待足够长的保护间隔，在此期间持续 ESP_AT_Process()
 * 消费上一次可能迟到的终止帧；不要在收到超时的瞬间调用本函数。
 *
 * 无法从软件上证明 ESP32 外部状态已复位，因此此接口不是硬件复位替代品。
 * 若长时间无响应，需要检查 ESP32 电源、EN/RST、串口电气和波特率。
 */
ESP_AT_Status_t ESP_AT_RearmStartupSession(void)
{
    ESP_AT_PortTxState_t tx_state;

    if (s_initialized == 0U)
    {
        return ESP_AT_ERROR_NOT_INITIALIZED;
    }
    if (s_command_state == ESP_AT_COMMAND_WAITING)
    {
        return ESP_AT_ERROR_BUSY;
    }
    /* 只允许初始 AT 配置阶段使用。禁止在有 IP 或 HTTP 事务时破坏会话。 */
    if ((s_wifi_has_ip != 0U) || (s_http_active != 0U))
    {
        return ESP_AT_ERROR_RECOVERY_REQUIRED;
    }
    tx_state = ESP_AT_Port_GetTxState();
    if ((tx_state == ESP_AT_PORT_TX_BUSY) || (tx_state == ESP_AT_PORT_TX_ERROR))
    {
        return ESP_AT_ERROR_PORT;
    }

    /* 上层已经在保护间隔内消费接收数据；从干净的行状态开始新握手。 */
    prvResetLineBuffer();
    prvResetHTTPContext();
    s_command_state = ESP_AT_COMMAND_IDLE;
    s_wifi_mode = -1;
    s_sntp_valid = 0U;
    s_sntp_query_active = 0U;
    s_weather_valid = 0U;
    s_http_active = 0U;
    return ESP_AT_OK;
}

/**
 * @brief 发送一条不包含 CRLF 的 AT 命令，使用 Port 的异步 TX DMA。
 *
 * @param command 例如 "AT"、"AT+CWMODE?"、"AT+CIPSNTPTIME?"。
 * @return ESP_AT_OK=DMA 已成功启动（不代表 ESP 返回 OK）；
 *         ESP_AT_ERROR_BUSY=已有未完成命令或底层 TX 忙；
 *         ESP_AT_ERROR_RECOVERY_REQUIRED=上一条命令超时，不能直接继续；
 *         ESP_AT_ERROR_INVALID_COMMAND=命令为空或包含 CR/LF；
 *         ESP_AT_ERROR_COMMAND_TOO_LONG=超过静态 TX 缓冲区限制；
 *         ESP_AT_ERROR_PORT=其他底层错误。
 *
 * DMA 内存安全：
 *   双缓冲区使用 s_next_tx_index 选择；只有 DMA 确实成功启动才切换索引。
 *   本函数将调用者的命令复制到静态数组，因此上层可以传入局部字符串。
 */
ESP_AT_Status_t ESP_AT_StartCommand(const char *command)
{
    ESP_AT_PortStatus_t port_status;
    size_t command_length;
    size_t index;
    uint8_t *tx_buffer;

    if (s_initialized == 0U)
    {
        return ESP_AT_ERROR_NOT_INITIALIZED;
    }
    if ((command == NULL) || (command[0] == '\0'))
    {
        return ESP_AT_ERROR_INVALID_COMMAND;
    }
    if (s_command_state == ESP_AT_COMMAND_WAITING)
    {
        return ESP_AT_ERROR_BUSY;
    }
    if (s_command_state == ESP_AT_COMMAND_TIMEOUT)
    {
        /* 防止上一条迟到的响应被错误归到新命令，不进行隐式重试。 */
        return ESP_AT_ERROR_RECOVERY_REQUIRED;
    }

    command_length = strlen(command);
    if (command_length > (ESP_AT_TX_BUFFER_SIZE - 3U))
    {
        return ESP_AT_ERROR_COMMAND_TOO_LONG;
    }

    /* 不允许命令正文含 CR/LF；统一由 Core 添加且只添加一次。 */
    for (index = 0U; index < command_length; ++index)
    {
        if ((command[index] == '\r') || (command[index] == '\n'))
        {
            return ESP_AT_ERROR_INVALID_COMMAND;
        }
    }

    /* 选定不会随函数返回而消失的 TX 缓冲区。 */
    tx_buffer = s_tx_buffers[s_next_tx_index];
    memcpy(tx_buffer, command, command_length);
    tx_buffer[command_length] = '\r';
    tx_buffer[command_length + 1U] = '\n';
    tx_buffer[command_length + 2U] = '\0';  /* 仅供调试，不发送此字节 */

    /* 真正交给 DMA 发送的是：命令正文 + CRLF，共 command_length+2 字节。 */
    port_status = ESP_AT_Port_WriteAsync(tx_buffer, command_length + 2U);
    if (port_status == ESP_AT_PORT_ERROR_BUSY)
    {
        return ESP_AT_ERROR_BUSY;
    }
    if (port_status != ESP_AT_PORT_OK)
    {
        return ESP_AT_ERROR_PORT;
    }

    /*
     * 只有硬件发送确实启动，才清理上一条命令的解析结果并切换缓冲区。
     * 这样在底层 BUSY 或失败时，不会意外破坏上一条命令的状态。
     */
    prvResetLineBuffer();
    prvResetHTTPContext();
    s_sntp_valid = 0U;
    s_weather_valid = 0U;
    s_sntp_query_active = (strcmp(command, "AT+CIPSNTPTIME?") == 0) ? 1U : 0U;
    s_http_active = (strncmp(command, ESP_AT_HTTP_COMMAND_PREFIX,
                              sizeof(ESP_AT_HTTP_COMMAND_PREFIX) - 1U) == 0) ? 1U : 0U;

    if (strcmp(command, "AT+CWMODE?") == 0)
    {
        s_wifi_mode = -1;
    }

    s_command_state = ESP_AT_COMMAND_WAITING;
    s_next_tx_index ^= 1U;  /* 0 -> 1 -> 0，下一次使用另一块缓冲区 */
    return ESP_AT_OK;
}

/**
 * @brief 与 ESP32-C3 的最基础握手，等价于 ESP_AT_StartCommand("AT")。
 * @return 与 ESP_AT_StartCommand() 相同的协议状态码。
 */
ESP_AT_Status_t ESP_AT_StartBasicTest(void)
{
    return ESP_AT_StartCommand("AT");
}

/* ========================================================================== */
/* 12. 数据轮询：把 Port 收到的原始字节交给 Core 状态机                      */
/* ========================================================================== */

/**
 * @brief 非阻塞处理一批 USART2 接收到的数据。
 *
 * NetworkTask 通常每隔约 10 ms 调用一次：
 *     Port_Read() -> rx_chunk[] -> prvConsumeByte() -> 更新 AT 命令状态。
 *
 * 注意：这里不直接调用 vTaskDelay()，也不等待网络；调用频率由上层控制。
 * 如果一次 Port_Read() 返回 0 字节，本函数正常返回 ESP_AT_OK。
 *
 * @return ESP_AT_OK=处理成功；其他状态=Port 读取或普通行解析错误。
 */
ESP_AT_Status_t ESP_AT_Process(void)
{
    ESP_AT_PortStatus_t port_status;
    ESP_AT_Status_t parse_status;
    uint8_t rx_chunk[ESP_AT_RX_CHUNK_SIZE];
    size_t read_length = 0U;
    size_t index;
    uint8_t batch;

    if (s_initialized == 0U)
    {
        return ESP_AT_ERROR_NOT_INITIALIZED;
    }

    /*
     * 尽量一次读空 Port RX Ring Buffer，降低 HTTP 短时突发数据溢出的风险。
     * 每批 64 字节，最多 32 批（2048 字节），保证一次调用有确定工作上限。
     * Port_Read() 返回 0 字节，说明当前没有可读数据，立即返回。
     */
    for (batch = 0U; batch < 32U; ++batch)
    {
        read_length = 0U;
        port_status = ESP_AT_Port_Read(rx_chunk, sizeof(rx_chunk), &read_length);
        if (port_status != ESP_AT_PORT_OK)
        {
            return ESP_AT_ERROR_PORT;
        }

        for (index = 0U; index < read_length; ++index)
        {
            parse_status = prvConsumeByte(rx_chunk[index]);
            if (parse_status != ESP_AT_OK)
            {
                return parse_status;
            }
        }

        if (read_length == 0U)
        {
            break;
        }
    }
    return ESP_AT_OK;
}

/* ========================================================================== */
/* 13. 结果查询与超时标记：供 NetworkTask 调用                                */
/* ========================================================================== */

/**
 * @brief 查询当前命令状态；不会自动清除状态。
 * @return IDLE / WAITING / OK / ERROR / TIMEOUT（具体枚举定义见 esp_at.h）。
 */
ESP_AT_CommandState_t ESP_AT_GetCommandState(void)
{
    return s_command_state;
}

/**
 * @brief 将正在等待的命令标记为超时。
 *
 * 调用者：NetworkTask 的超时检测分支。
 * 后续 ESP32-C3 可能仍返回迟到数据，因此本函数不自行恢复或重发命令。
 * 下次 ESP_AT_StartCommand() 会返回 ESP_AT_ERROR_RECOVERY_REQUIRED。
 */
void ESP_AT_MarkTimeout(void)
{
    if ((s_initialized != 0U) && (s_command_state == ESP_AT_COMMAND_WAITING))
    {
        s_command_state = ESP_AT_COMMAND_TIMEOUT;
        s_weather_valid = 0U;
        s_sntp_valid = 0U;
    }
}

/**
 * @brief 获取最近一次 AT+CWMODE? 的 Wi-Fi 模式。
 * @return -1=未查询成功；0=Null；1=Station；2=SoftAP；3=Station+SoftAP。
 */
int ESP_AT_GetWiFiMode(void)
{
    return s_wifi_mode;
}

/**
 * @brief 获取 ESP-AT 最近通过 URC 报告的 IP 联网标志。
 * @return 1=收到过 WIFI GOT IP 且之后未收到 WIFI DISCONNECT；0=否则。
 *
 * 注意：判断基于 URC，不会主动发网络探测命令。
 */
uint8_t ESP_AT_HasWiFiIP(void)
{
    return s_wifi_has_ip;
}

/**
 * @brief 获取本次 AT+CIPSNTPTIME? 成功解析出的网络时间。
 * @param time 非 NULL 的输出结构体地址；调用者提供存储空间。
 * @return 1=本次命令已返回 OK 且时间有效；0=无有效结果。
 *
 * 注意：时间值只在本次 SNTP 命令完成后有效；下一条命令成功启动时清零标记。
 */
uint8_t ESP_AT_GetSNTPTime(ESP_AT_NetworkTime_t *time)
{
    if ((time == NULL) || (s_sntp_query_active == 0U) ||
        (s_sntp_valid == 0U) || (s_command_state != ESP_AT_COMMAND_OK))
    {
        return 0U;
    }

    *time = s_sntp_time;  /* 结构体整体赋值，一次复制全部日期与时间成员 */
    return 1U;
}

/**
 * @brief 兼容旧接口：从本次完整天气记录中读取室外气温。
 * @param temperature_c 非 NULL 的 float 指针，成功时写入摄氏温度。
 * @return 1=HTTP 命令已完成且温度合法；0=没有有效温度。
 *
 * 这里不仅检查命令 OK，还检查 HTTP 数据完整性及天气字段解析结果。
 * 这样不会把上一次缓存的温度错误当作本次请求结果。
 */
uint8_t ESP_AT_GetOutdoorTemperature(float *temperature_c)
{
    if ((temperature_c == NULL) || (s_http_active == 0U) ||
        (s_weather_valid == 0U) || (s_command_state != ESP_AT_COMMAND_OK))
    {
        return 0U;
    }

    /* 兼容旧 API：温度与完整 WeatherData_t 来自同一次 HTTP 响应。 */
    *temperature_c = s_weather_data.temperature;
    return 1U;
}

/**
 * @brief 获取本次 HTTP 请求已经验证的五字段天气数据。
 * @param weather 非 NULL 的输出结构体；函数仅在有效时写入其全部成员。
 * @return 1=HTTP 命令收到 OK 且五项天气解析成功；0=无本次有效记录。
 *
 * @note 仅由串行执行 AT 命令的 NetworkTask 调用，不提供跨任务同步保证。
 *       下一条 AT 命令一旦成功启动，有效标记会清零。
 */
uint8_t ESP_AT_GetWeatherData(WeatherData_t *weather)
{
    if ((weather == NULL) || (s_http_active == 0U) ||
        (s_weather_valid == 0U) || (s_command_state != ESP_AT_COMMAND_OK))
    {
        return 0U;
    }

    *weather = s_weather_data;
    return 1U;
}
