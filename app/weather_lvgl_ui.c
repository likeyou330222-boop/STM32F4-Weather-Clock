/**
 * @file    weather_lvgl_ui.c
 * @brief   天气时钟 LVGL V3 视觉界面，320x240 横屏，LVGL 8.3.11。
 *
 * --------------------------- 分层及线程安全 ------------------------------
 * 本文件属于“显示表现层”，绝不访问 SPI/DMA、RTC 寄存器、ESP AT 或传感器。
 * 所有公开 WeatherLVGL_* 接口只允许从 AppTask（同一个 LVGL 所有者）调用。
 * NetworkTask 与 SensorTask 只发布原始数据，不能从它们的任务中操作 lv_obj。
 *
 * --------------------------- V3 相比 V2 的改变 --------------------------
 * 1. 顶部用三枚独立状态图标表示 Wi-Fi、天气缓存和本次开机的 SNTP 校时。
 * 2. 屏幕中心使用真正的 48px 字体显示 HH:MM；秒、日期退到次要层级。
 * 3. 室内和室外使用带房子/水滴/天气现象的轻量卡片。
 * 4. 地址定位、风速有明确的矢量风格透明图标。
 * 5. 图标数据位于 weather_icons.c 的 const Flash 区，颜色由 LVGL recolor 控制。
 * 6. 数据更新仅更新已存在的控件，不循环创建控件，不在刷新回调里阻塞。
 *
 * ------------------------------ RAM 注意 -------------------------------
 * 不启用大背景图、阴影、渐变、不透明叠层动画；减少 STM32F407 软件渲染开销。
 * 屏幕对象一次性建立，主时钟仅在分钟改变时更新（秒标签每秒更新）。
 * 显示资源创建失败会返回 0，使上层打印现有 LVGL 初始化错误日志。
 *
 * ------------------------------ Keil 要求 ------------------------------
 * 需要编译：本文件、weather_icons.c、lv_font_montserrat_14.c、
 *          lv_font_montserrat_24.c、lv_font_montserrat_48.c。
 * lv_conf.h：LV_COLOR_DEPTH=16，Montserrat 14/24/48 三项均为 1。
 */

#include "weather_lvgl_ui.h"
#include "weather_icons.h"
#include "lvgl.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define WC_SCREEN_WIDTH           320
#define WC_SCREEN_HEIGHT          240
#define WC_BOOT_ANIM_MS           240U

/* 所有颜色均为标准 RGB888 十六进制，LVGL 会根据 LV_COLOR_DEPTH 转成 RGB565。 */
#define WC_BG                     0x071521UL /* 极深海军蓝背景：突出大号时间 */
#define WC_CARD                   0x132B3DUL /* 信息卡片底色 */
#define WC_CARD_CLOCK             0x172F44UL /* 时间卡片略明亮，形成主次层次 */
#define WC_BORDER                 0x29475BUL /* 低对比度分割线 */
#define WC_MAIN                   0xF4FAFFUL /* 时间与主数值 */
#define WC_MUTED                  0x9DB5C7UL /* 日期及辅助信息 */
#define WC_TEAL                   0x55E6CEUL /* 正常工作状态 / 室内 */
#define WC_SKY                    0x78CFFFUL /* 风速 / 湿度 */
#define WC_SUN                    0xFFCC78UL /* 室外天气 */
#define WC_OFF                    0x567185UL /* 未就绪：不能伪称正在联网 */
#define WC_WARN                   0xFFAB75UL /* 缓存或天气暂无数据 */

/* 对象指针只在 AppTask 中使用，无需 volatile 或互斥锁。 */
static lv_obj_t *s_boot_screen;
static lv_obj_t *s_home_screen;
static lv_obj_t *s_boot_dots[3];
static lv_timer_t *s_boot_timer;
static uint8_t s_boot_phase;

static lv_obj_t *s_icon_wifi;
static lv_obj_t *s_icon_weather_status;
static lv_obj_t *s_icon_sync;
static lv_obj_t *s_time;
static lv_obj_t *s_seconds;
static lv_obj_t *s_date;
static lv_obj_t *s_indoor_temp;
static lv_obj_t *s_indoor_humi;
static lv_obj_t *s_outdoor_temp;
static lv_obj_t *s_outdoor_desc;
static lv_obj_t *s_outdoor_icon;
static lv_obj_t *s_wind;

static uint8_t s_last_hour = 255U;
static uint8_t s_last_minute = 255U;
static uint8_t s_has_status = 0U;
static uint8_t s_last_wifi = 0U;
static uint8_t s_last_weather = 0U;
static uint8_t s_last_sync = 0U;
static uint8_t s_ready = 0U;
static uint8_t s_home_shown = 0U;

/* -------------------------------------------------------------------------
 * 01. 低层控件构造助手
 * ------------------------------------------------------------------------- */

/**
 * @brief 创建一个不参与滚动、不带 LVGL 默认边框的纯色圆角矩形。
 * @param parent 父控件；所有坐标都相对于该父控件左上角。
 * @return 新对象指针，创建失败返回 NULL。
 *
 * 背景小装饰也使用这个函数，避免复用额外的复杂 Widget。
 */
static lv_obj_t *prvRect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h,
                         uint32_t rgb, lv_coord_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    if (o == NULL) return NULL;
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/**
 * @brief 创建一个固定宽度、禁止溢出滚动的 14px 标签。
 * @note 字体与布局在初次创建后保持不变。数值更新只调用 lv_label_set_text。
 */
static lv_obj_t *prvLabel(lv_obj_t *parent, const char *str,
                          lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          uint32_t color, lv_text_align_t alignment)
{
    lv_obj_t *o = lv_label_create(parent);
    if (o == NULL) return NULL;
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(o, alignment, LV_PART_MAIN);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    lv_label_set_text(o, str);
    return o;
}

/**
 * @brief 建立 Alpha-8 图标，RGB 颜色通过 LVGL 图片 recolor 设置。
 *
 * Alpha-8 每像素只有透明度，图标资源不包含背景，也不会在屏幕上显示白方块。
 * LVGL 的软件渲染器将图片叠加至正在绘制的 RGB565 区域。
 */
static lv_obj_t *prvIcon(lv_obj_t *parent, const lv_img_dsc_t *source,
                         lv_coord_t x, lv_coord_t y, uint32_t color)
{
    lv_obj_t *o = lv_img_create(parent);
    if (o == NULL) return NULL;
    lv_img_set_src(o, source);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_img_recolor(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/** @brief 控件状态切换仅修改图标颜色，不删除和重新创建图标。 */
static void prvIconColor(lv_obj_t *o, uint32_t color)
{
    if (o != NULL)
        lv_obj_set_style_img_recolor(o, lv_color_hex(color), LV_PART_MAIN);
}

/**
 * @brief 减少文本更新造成的动态分配及无意义重绘。
 * @note lv_label_set_text() 会复制文本；局部 char[] 可以安全地在调用后销毁。
 */
static void prvTextIfDifferent(lv_obj_t *o, const char *str)
{
    if ((o != NULL) && (str != NULL) &&
        (strcmp(lv_label_get_text(o), str) != 0))
    {
        lv_label_set_text(o, str);
    }
}

/**
 * @brief ARMCC5 不一定支持 printf("%f")：统一转换为整数十分位。
 * @param[out] dst 最少建议 12 字节；无法表示时输出 "--.-"。
 */
static void prvOneDecimal(char *dst, size_t size, float v)
{
    int32_t q;
    uint32_t mag;
    if ((dst == NULL) || (size == 0U)) return;
    /* 范围检查也能够挡住 IEEE754 NaN。 */
    if (!((v >= -100.0f) && (v <= 150.0f)))
    {
        (void)snprintf(dst, size, "--.-");
        return;
    }
    q = (int32_t)(v * 10.0f + ((v < 0.0f) ? -0.5f : 0.5f));
    mag = (uint32_t)((q < 0) ? -q : q);
    (void)snprintf(dst, size, "%s%lu.%lu", (q < 0) ? "-" : "",
                   (unsigned long)(mag / 10UL),
                   (unsigned long)(mag % 10UL));
}

/**
 * @brief WMO 天气代码映射。只返回短字符串以适配 320x240 小屏幕。
 * @note 图片与文字的映射统一在 UI 层，网络协议层只保持真实 WMO 整数。
 */
static const char *prvWeatherText(uint8_t code, uint8_t is_day)
{
    if (code == 0U) return is_day ? "SUNNY" : "CLEAR NIGHT";
    if (code == 1U) return "MOSTLY CLEAR";
    if (code == 2U) return "PART CLOUDY";
    if (code == 3U) return "CLOUDY";
    if ((code == 45U) || (code == 48U)) return "FOGGY";
    if ((code >= 51U) && (code <= 57U)) return "DRIZZLE";
    if ((code >= 61U) && (code <= 67U)) return "RAINY";
    if ((code >= 71U) && (code <= 77U)) return "SNOWY";
    if ((code >= 80U) && (code <= 82U)) return "SHOWERS";
    if ((code == 85U) || (code == 86U)) return "SNOW SHOWERS";
    if ((code >= 95U) && (code <= 99U)) return "THUNDER";
    return "UNKNOWN";
}

/**
 * @brief 天气主图标和图标色。夜间晴朗会显示月亮而非白天太阳。
 * @param[out] color 对应晴/雨/雪的强调色；由调用者写入到 lv_img recolor。
 */
static const lv_img_dsc_t *prvWeatherPicture(uint8_t code, uint8_t is_day,
                                             uint32_t *color)
{
    if (color == NULL) return &wc_img_cloud;
    if (code == 0U)
    {
        *color = is_day ? WC_SUN : WC_SKY;
        return is_day ? &wc_img_sun : &wc_img_moon;
    }
    if ((code == 1U) || (code == 2U))
    {
        *color = WC_SUN;
        return &wc_img_partly;
    }
    if (code == 3U) { *color = WC_MUTED; return &wc_img_cloud; }
    if ((code == 45U) || (code == 48U))
    { *color = WC_MUTED; return &wc_img_fog; }
    if (((code >= 71U) && (code <= 77U)) || (code == 85U) || (code == 86U))
    { *color = WC_SKY; return &wc_img_snow; }
    if (code >= 95U) { *color = WC_SUN; return &wc_img_storm; }
    *color = WC_SKY;
    return &wc_img_rain;
}

/* -------------------------------------------------------------------------
 * 02. 开机页 —— 保持简短、不阻塞系统联网，不伪造任何在线状态
 * ------------------------------------------------------------------------- */

/** @brief 220~240ms 一次的纯颜色切换动画，不会做大型图片 alpha 混合。 */
static void prvBootAnimation(lv_timer_t *timer)
{
    uint8_t i;
    (void)timer;
    for (i = 0U; i < 3U; ++i)
        if (s_boot_dots[i] != NULL)
            lv_obj_set_style_bg_color(s_boot_dots[i],
                lv_color_hex(i == s_boot_phase ? WC_TEAL : WC_BORDER), LV_PART_MAIN);
    s_boot_phase = (uint8_t)((s_boot_phase + 1U) % 3U);
}

/** @brief 构造启动页；由上层现有 2.5 秒开机定时控制何时进入主页。 */
static void prvCreateBoot(void)
{
    lv_obj_t *logo;
    lv_obj_t *title;
    s_boot_screen = lv_scr_act();
    if (s_boot_screen == NULL) return;
    lv_obj_clean(s_boot_screen);
    lv_obj_set_style_bg_color(s_boot_screen, lv_color_hex(WC_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_boot_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_boot_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_boot_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_boot_screen, LV_OBJ_FLAG_SCROLLABLE);

    (void)prvRect(s_boot_screen, 0, 0, 320, 3, WC_TEAL, 0);
    (void)prvLabel(s_boot_screen, "ATMOS", 18, 19, 135, WC_MAIN, LV_TEXT_ALIGN_LEFT);
    (void)prvLabel(s_boot_screen, "STM32F407", 190, 19, 110, WC_MUTED, LV_TEXT_ALIGN_RIGHT);
    logo = prvRect(s_boot_screen, 116, 50, 88, 79, WC_CARD_CLOCK, 19);
    if (logo != NULL)
    {
        (void)prvIcon(logo, &wc_img_partly, 23, 16, WC_SUN);
        (void)prvRect(logo, 13, 68, 62, 2, WC_BORDER, 1);
    }
    title = prvLabel(s_boot_screen, "WEATHER CLOCK", 5, 142, 310, WC_MAIN, LV_TEXT_ALIGN_CENTER);
    if (title != NULL)
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    (void)prvLabel(s_boot_screen, "A clearer view of your day", 0, 176, 320,
                   WC_MUTED, LV_TEXT_ALIGN_CENTER);
    (void)prvRect(s_boot_screen, 23, 198, 274, 1, WC_BORDER, 0);
    (void)prvLabel(s_boot_screen, "STARTING DISPLAY", 23, 211, 200, WC_MUTED,
                   LV_TEXT_ALIGN_LEFT);
    s_boot_dots[0] = prvRect(s_boot_screen, 264, 216, 6, 6, WC_TEAL, 3);
    s_boot_dots[1] = prvRect(s_boot_screen, 277, 216, 6, 6, WC_BORDER, 3);
    s_boot_dots[2] = prvRect(s_boot_screen, 290, 216, 6, 6, WC_BORDER, 3);
    s_boot_phase = 0U;
    s_boot_timer = lv_timer_create(prvBootAnimation, WC_BOOT_ANIM_MS, NULL);
}

/* -------------------------------------------------------------------------
 * 03. 主界面：绝对坐标以保证屏幕可用面积不被 flex/grid 内边距吞掉
 * ------------------------------------------------------------------------- */

/**
 * @brief 创建 V3 主屏：顶栏 0~35，时间卡片 41~136，双环境卡 145~214，底栏 221~239。
 *
 * 重要坐标说明：卡片内部子对象使用“相对于卡片”的坐标。单页所有对象
 * 保持在 320×240 以内，不需要触摸、滚动条或背景缓冲。
 */
static void prvCreateHome(void)
{
    lv_obj_t *clock;
    lv_obj_t *indoor;
    lv_obj_t *outdoor;
    lv_obj_t *icon_background;

    s_home_screen = lv_obj_create(NULL);
    if (s_home_screen == NULL) return;
    lv_obj_set_style_bg_color(s_home_screen, lv_color_hex(WC_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_home_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_home_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_home_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_home_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶栏左侧：独立品牌小图案和名称。 */
    (void)prvRect(s_home_screen, 0, 0, 320, 3, WC_TEAL, 0);
    (void)prvRect(s_home_screen, 12, 10, 18, 18, WC_TEAL, 6);
    /* 品牌图案直接使用天气透明图标，避免简易圆环看起来像数字 0。 */
    (void)prvIcon(s_home_screen, &wc_img_cloud_s, 13, 11, WC_BG);
    (void)prvLabel(s_home_screen, "ATMOS", 38, 9, 92, WC_MAIN, LV_TEXT_ALIGN_LEFT);
    (void)prvLabel(s_home_screen, "WEATHER CLOCK", 112, 9, 125, WC_MUTED,
                   LV_TEXT_ALIGN_LEFT);

    /* 右侧三个状态灯各占 20px（居中 16x16 图标），状态颜色在 UpdateStatus 中变化。
     * 状态图标语义（左->右）：Wi-Fi 有 IP / 天气缓存有效 / 本次开机 SNTP 已校时。 */
    icon_background = prvRect(s_home_screen, 241, 8, 23, 23, WC_CARD, 7);
    if (icon_background != NULL)
        s_icon_wifi = prvIcon(icon_background, &wc_img_wifi, 3, 3, WC_OFF);
    icon_background = prvRect(s_home_screen, 267, 8, 23, 23, WC_CARD, 7);
    if (icon_background != NULL)
        s_icon_weather_status = prvIcon(icon_background, &wc_img_cloud_s, 3, 3, WC_OFF);
    icon_background = prvRect(s_home_screen, 293, 8, 23, 23, WC_CARD, 7);
    if (icon_background != NULL)
        s_icon_sync = prvIcon(icon_background, &wc_img_sync, 3, 3, WC_OFF);
    (void)prvRect(s_home_screen, 12, 35, 296, 1, WC_BORDER, 0);

    /* 主时钟：刻意居中大号时分，把秒数下移，避免把 16:25 读成 16:25:13。 */
    clock = prvRect(s_home_screen, 10, 39, 300, 100, WC_CARD_CLOCK, 12);
    if (clock != NULL)
    {
        (void)prvRect(clock, 13, 12, 3, 11, WC_TEAL, 2);
        (void)prvLabel(clock, "LOCAL TIME / 24H", 25, 8, 201, WC_MUTED,
                       LV_TEXT_ALIGN_LEFT);
        /* 时钟主值在 x20～255；秒数被固定在下方右侧，不属于主值。 */
        s_time = prvLabel(clock, "--:--", 16, 28, 268, WC_MAIN, LV_TEXT_ALIGN_CENTER);
        if (s_time != NULL)
            lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, LV_PART_MAIN);
        /* 大号时钟字形的墨迹距分隔线约 6px，日期不会顶到主数字。 */
        (void)prvRect(clock, 13, 76, 274, 1, WC_BORDER, 0);
        s_date = prvLabel(clock, "WAITING FOR RTC", 16, 79, 220, WC_MUTED,
                          LV_TEXT_ALIGN_LEFT);
        s_seconds = prvLabel(clock, "-- s", 243, 79, 43, WC_TEAL,
                             LV_TEXT_ALIGN_RIGHT);
    }

    /* 室内卡片：房子 + 温度 + 水滴/湿度；底部行与室外天气对齐。 */
    indoor = prvRect(s_home_screen, 10, 145, 146, 70, WC_CARD, 10);
    if (indoor != NULL)
    {
        (void)prvIcon(indoor, &wc_img_house, 10, 8, WC_TEAL);
        (void)prvLabel(indoor, "INDOOR", 32, 7, 109, WC_MUTED, LV_TEXT_ALIGN_LEFT);
        s_indoor_temp = prvLabel(indoor, "--.-", 10, 24, 99, WC_MAIN,
                                 LV_TEXT_ALIGN_LEFT);
        if (s_indoor_temp != NULL)
            lv_obj_set_style_text_font(s_indoor_temp, &lv_font_montserrat_24, LV_PART_MAIN);
        (void)prvLabel(indoor, "\xC2\xB0" "C", 106, 30, 32, WC_TEAL, LV_TEXT_ALIGN_LEFT);
        (void)prvIcon(indoor, &wc_img_drop, 11, 52, WC_SKY);
        s_indoor_humi = prvLabel(indoor, "--% RH", 30, 50, 107, WC_MUTED,
                                 LV_TEXT_ALIGN_LEFT);
    }

    /* 室外卡片：天气图标采用独立 Image，与文字分离。
     * 图标不会随每一次 LVGL 刷新重建，只在 WMO code 变化后换图。 */
    outdoor = prvRect(s_home_screen, 164, 145, 146, 70, WC_CARD, 10);
    if (outdoor != NULL)
    {
        (void)prvIcon(outdoor, &wc_img_cloud_s, 10, 8, WC_SUN);
        (void)prvLabel(outdoor, "OUTDOOR", 32, 7, 109, WC_MUTED, LV_TEXT_ALIGN_LEFT);
        s_outdoor_temp = prvLabel(outdoor, "--.-", 10, 24, 85, WC_MAIN,
                                  LV_TEXT_ALIGN_LEFT);
        if (s_outdoor_temp != NULL)
            lv_obj_set_style_text_font(s_outdoor_temp, &lv_font_montserrat_24, LV_PART_MAIN);
        (void)prvLabel(outdoor, "\xC2\xB0" "C", 89, 30, 26, WC_SUN, LV_TEXT_ALIGN_LEFT);
        s_outdoor_icon = prvIcon(outdoor, &wc_img_cloud, 103, 24, WC_OFF);
        s_outdoor_desc = prvLabel(outdoor, "NO DATA", 10, 50, 132, WC_MUTED,
                                  LV_TEXT_ALIGN_LEFT);
    }

    /* 底部信息栏：城市定位 + 风速；保留背景通透感而非新增一整块卡片。 */
    (void)prvIcon(s_home_screen, &wc_img_pin, 14, 222, WC_TEAL);
    (void)prvLabel(s_home_screen, "QINHUANGDAO", 33, 220, 155, WC_MUTED,
                   LV_TEXT_ALIGN_LEFT);
    (void)prvIcon(s_home_screen, &wc_img_wind, 185, 222, WC_SKY);
    s_wind = prvLabel(s_home_screen, "-- km/h", 206, 220, 103, WC_MUTED,
                      LV_TEXT_ALIGN_RIGHT);
}

/* -------------------------------------------------------------------------
 * 04. 公共接口：初始化、切屏、状态、时间、传感器、网络天气
 * ------------------------------------------------------------------------- */

uint8_t WeatherLVGL_Init(void)
{
    if (s_ready != 0U) return 1U;
    prvCreateBoot();
    if (s_boot_screen == NULL) return 0U;
    prvCreateHome();
    if (s_home_screen == NULL) return 0U;

    /* 对核心数据控件做集中校验：避免 UI 不完整却被 AppTask 当作成功启动。 */
    if ((s_time == NULL) || (s_seconds == NULL) || (s_date == NULL) ||
        (s_indoor_temp == NULL) || (s_indoor_humi == NULL) ||
        (s_outdoor_temp == NULL) || (s_outdoor_desc == NULL) ||
        (s_outdoor_icon == NULL) || (s_wind == NULL) ||
        (s_icon_wifi == NULL) || (s_icon_weather_status == NULL) ||
        (s_icon_sync == NULL)) return 0U;
    s_ready = 1U;
    return 1U;
}

void WeatherLVGL_ShowHome(void)
{
    if ((s_ready == 0U) || (s_home_shown != 0U)) return;
    if (s_boot_timer != NULL)
    {
        lv_timer_del(s_boot_timer);
        s_boot_timer = NULL;
    }
    s_home_shown = 1U;
    /* LVGL 管理的淡入动画完成后才删除旧屏；不再操作 s_boot_* 子对象。 */
    lv_scr_load_anim(s_home_screen, LV_SCR_LOAD_ANIM_FADE_ON, 280U, 0U, true);
    s_boot_screen = NULL;
    s_boot_dots[0] = NULL;
    s_boot_dots[1] = NULL;
    s_boot_dots[2] = NULL;
}

/**
 * @brief 网络、天气、校时三个相互独立的状态灯。
 * @param wifi_has_ip NetworkTask 接收到过 GOT IP 且尚未 DISCONNECT。
 * @param weather_valid NetworkTask 天气缓存未过有效期（并不证明网络当前在线）。
 * @param sntp_synced 本次启动 NetworkTask 至少成功把 SNTP 时间写入 RTC 一次。
 *
 * 不使用 GUI 自己的网络探测：UI 只能忠实呈现任务发布的状态。
 */
void WeatherLVGL_UpdateStatus(uint8_t wifi_has_ip,
                              uint8_t weather_valid,
                              uint8_t sntp_synced)
{
    uint8_t wifi = wifi_has_ip ? 1U : 0U;
    uint8_t weather = weather_valid ? 1U : 0U;
    uint8_t sync = sntp_synced ? 1U : 0U;
    if (s_ready == 0U) return;

    if ((s_has_status == 0U) || (wifi != s_last_wifi))
        prvIconColor(s_icon_wifi, wifi ? WC_TEAL : WC_OFF);
    if ((s_has_status == 0U) || (weather != s_last_weather))
        prvIconColor(s_icon_weather_status, weather ? WC_SUN : WC_OFF);
    if ((s_has_status == 0U) || (sync != s_last_sync))
        prvIconColor(s_icon_sync, sync ? WC_SKY : WC_OFF);
    s_last_wifi = wifi;
    s_last_weather = weather;
    s_last_sync = sync;
    s_has_status = 1U;
}

/** @brief RTC 由 AppTask 周期读取；UI 没有自行累加时间，不受 Wi-Fi 影响。 */
void WeatherLVGL_UpdateTime(const RTC_DateTime_t *dt)
{
    char time_buf[10];
    char sec_buf[8];
    char date_buf[32];
    static const char *const week[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };
    const char *w;

    if ((s_ready == 0U) || (dt == NULL)) return;
    if ((dt->hour > 23U) || (dt->minute > 59U) || (dt->second > 59U) ||
        (dt->month < 1U) || (dt->month > 12U) ||
        (dt->day < 1U) || (dt->day > 31U)) return;

    /* 大号主值只在分钟改变时更新，降低频繁字形重绘造成的 SPI 刷新量。 */
    if ((dt->hour != s_last_hour) || (dt->minute != s_last_minute))
    {
        (void)snprintf(time_buf, sizeof(time_buf), "%02u:%02u",
                       (unsigned int)dt->hour, (unsigned int)dt->minute);
        prvTextIfDifferent(s_time, time_buf);
        s_last_hour = dt->hour;
        s_last_minute = dt->minute;
    }

    (void)snprintf(sec_buf, sizeof(sec_buf), "%02u s", (unsigned int)dt->second);
    prvTextIfDifferent(s_seconds, sec_buf);

    w = (dt->weekday >= 1U && dt->weekday <= 7U) ? week[dt->weekday - 1U] : "---";
    (void)snprintf(date_buf, sizeof(date_buf), "%04u.%02u.%02u  %s",
                   (unsigned int)dt->year, (unsigned int)dt->month,
                   (unsigned int)dt->day, w);
    prvTextIfDifferent(s_date, date_buf);
}

/** @brief 输入测量数据只在 valid=1 时访问；有效温湿度彼此独立格式化。 */
void WeatherLVGL_UpdateIndoor(const AHT20_Measurement_t *data, uint8_t valid)
{
    char val[16];
    char label[32];
    if (s_ready == 0U) return;
    if ((valid == 0U) || (data == NULL))
    {
        prvTextIfDifferent(s_indoor_temp, "--.-");
        prvTextIfDifferent(s_indoor_humi, "--% RH");
        return;
    }
    prvOneDecimal(val, sizeof(val), data->temperature_celsius);
    prvTextIfDifferent(s_indoor_temp, val);
    prvOneDecimal(val, sizeof(val), data->humidity_percent);
    (void)snprintf(label, sizeof(label), "%s%% RH", val);
    prvTextIfDifferent(s_indoor_humi, label);
}

/**
 * @brief 室外天气数据变更时更新图标、天气代码文本、温度以及风速。
 * @note 此函数不判断 Wi-Fi 当前是否在线；缓存有效性由 NetworkTask 决定。
 */
void WeatherLVGL_UpdateOutdoor(const WeatherData_t *data, uint8_t valid)
{
    char val[16];
    char label[32];
    uint32_t color = WC_OFF;
    const lv_img_dsc_t *source;
    uint32_t wind;
    if (s_ready == 0U) return;
    if ((valid == 0U) || (data == NULL))
    {
        prvTextIfDifferent(s_outdoor_temp, "--.-");
        prvTextIfDifferent(s_outdoor_desc, "NO WEATHER DATA");
        prvTextIfDifferent(s_wind, "-- km/h");
        lv_img_set_src(s_outdoor_icon, &wc_img_cloud);
        prvIconColor(s_outdoor_icon, WC_OFF);
        return;
    }

    prvOneDecimal(val, sizeof(val), data->temperature);
    prvTextIfDifferent(s_outdoor_temp, val);
    prvTextIfDifferent(s_outdoor_desc, prvWeatherText(data->weather_code, data->is_day));
    source = prvWeatherPicture(data->weather_code, data->is_day, &color);
    if (lv_img_get_src(s_outdoor_icon) != (const void *)source)
        lv_img_set_src(s_outdoor_icon, source);
    prvIconColor(s_outdoor_icon, color);

    if ((data->wind_speed >= 0.0f) && (data->wind_speed <= 500.0f))
    {
        wind = (uint32_t)(data->wind_speed + 0.5f);
        (void)snprintf(label, sizeof(label), "%lu km/h", (unsigned long)wind);
    }
    else (void)snprintf(label, sizeof(label), "-- km/h");
    prvTextIfDifferent(s_wind, label);
}

uint8_t WeatherLVGL_IsReady(void)
{
    return s_ready;
}
