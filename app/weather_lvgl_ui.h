#ifndef WEATHER_LVGL_UI_H
#define WEATHER_LVGL_UI_H

/* ============================================================================
 * 文件名称：weather_lvgl_ui.h
 *
 * 模块名称：STM32F407 天气时钟 —— LVGL 8.3.11 用户界面层
 *
 * 配套实现文件：weather_lvgl_ui.c
 *
 * 【一、模块职责】
 * 本模块只处理“界面上显示什么、界面对象何时更新”：
 *   1. 构建天气时钟的开机欢迎画面（Boot Screen）。
 *   2. 构建图标化天气时钟主界面（Home Screen）。
 *   3. 将 AppTask 传来的 RTC 日期时间更新到时钟控件。
 *   4. 将 AppTask 传来的 AHT20 温湿度更新到室内信息卡片。
 *   5. 将 AppTask 传来的网络天气快照更新到室外信息卡片。
 *   6. 用独立图标显示 Wi-Fi IP、天气缓存有效性、本次启动的校时结果。
 *
 * 【二、本模块不负责的工作】
 *   - 不初始化 STM32 GPIO、SPI、DMA、RTC 或 AHT20 硬件。
 *   - 不执行 ESP-AT 命令，也不主动连接 Wi-Fi。
 *   - 不创建 FreeRTOS 任务或队列。
 *   - 不调用 lv_timer_handler() 或 lv_tick_inc()。
 *   - 不直接向 ILI9341 写入像素；真正的像素传输交给 LCD Service Task。
 *
 * 【三、任务所有权——非常重要】
 * 以下全部 WeatherLVGL_*() API 只能由同一个 LVGL 所属任务调用。
 * 当前项目计划让 AppTask 成为唯一的 LVGL 任务。
 *
 * 正确方式：
 *   SensorTask ---> 数据队列 / 事件通知 ---->
 *                                           AppTask ---> WeatherLVGL_Update...
 *   NetworkTask --> 天气快照 / 事件通知 ---->       |
 *                                                  v
 *                                            lv_timer_handler()
 *                                                  |
 *                                                  v
 *                                                flush_cb
 *                                                  |
 *                                                  v
 *                                            LCD Service Task
 *                                                  |
 *                                                  v
 *                                               SPI DMA
 *
 * 错误方式：
 *   SensorTask / NetworkTask 直接调用 WeatherLVGL_Update...()
 *
 * 原因：LVGL 8.3.11 在当前工程配置下，不是可供多个任务随意并发操作的
 * GUI 接口。将所有 LVGL API 统一交给 AppTask，可以避免对象状态竞争。
 *
 * 【四、初始化顺序】
 * 在 AppTask 中按以下顺序调用：
 *   ① 确保 LCD Service 已启动且屏幕已成功初始化；
 *   ② lv_init();
 *   ③ LVGL_PortDisp_Init();        // 来自 lvgl_port_disp.h
 *   ④ WeatherLVGL_Init();         // 创建启动画面和主画面
 *   ⑤ 定期调用 lv_timer_handler(); // 执行 LVGL 刷新和动画
 *   ⑥ 到达设计的展示时机后，调用 WeatherLVGL_ShowHome();
 *   ⑦ 在 AppTask 中根据业务数据调用三个 Update 接口。
 *
 * 特别注意：如果没有周期性调用 lv_timer_handler()，即使创建了全部
 * LVGL 对象，屏幕也可能不会刷新，开机动画也不会正常播放。
 *
 * 【五、头文件依赖】
 * 以下三个头文件使用的是旧工程中已经存在的、真实的业务数据类型。
 * 不在 UI 模块中重复定义这些结构体，避免两份数据格式逐渐不一致。
 * ============================================================================
 */

/* uint8_t 等固定宽度整数类型。 */
#include <stdint.h>

/* STM32 RTC 的日期时间快照类型：RTC_DateTime_t。 */
#include "rtc.h"

/* AHT20 转换后的温湿度测量类型：AHT20_Measurement_t。 */
#include "aht20.h"

/* ESP-AT / Open-Meteo 的天气快照类型：WeatherData_t。 */
#include "esp_at.h"

/* 允许 C++ 源文件按 C 语言链接规则调用这些接口。 */
#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. WeatherLVGL_Init —— 创建两个 LVGL 屏幕
 * ============================================================================
 */

/**
 * @brief 创建开机欢迎屏幕和天气主屏幕，首先显示开机欢迎屏幕。
 *
 * @return uint8_t
 *         1U：初始化成功，界面对象已创建。
 *         0U：初始化失败（例如对象创建失败）。
 *
 * @pre 已完成 lv_init() 和 LVGL_PortDisp_Init()。
 * @pre LCD Service Task 可正常处理显示 Job。
 * @pre 在同一个 LVGL 所属任务（计划为 AppTask）中调用。
 *
 * @note  本函数只创建 UI 对象，不自行调用 lv_timer_handler()。
 * @note  成功初始化后重复调用，不会重复创建 UI 对象。
 * @note  初始化成功不等于画面已经传输到 LCD；画面刷新仍由
 *        lv_timer_handler() -> flush_cb -> LCD Service 实现。
 */
uint8_t WeatherLVGL_Init(void);

/* ============================================================================
 * 2. WeatherLVGL_ShowHome —— 从启动页切换到主页
 * ============================================================================
 */

/**
 * @brief 结束开机欢迎画面，切换到天气时钟主界面。
 *
 * @details
 *   - 停止启动页面的循环加载动画计时器。
 *   - 使用 LVGL 自身的屏幕切换效果加载天气主屏幕。
 *   - 旧的启动屏幕将由 LVGL 动画机制在适当时机删除。
 *
 * @pre WeatherLVGL_Init() 已成功执行。
 * @pre 由 AppTask 调用，并且 AppTask 仍会定期调用 lv_timer_handler()。
 *
 * @note 重复调用不会反复切换或重复创建页面。
 * @note 此函数不会等待 Wi-Fi 必须连接成功；没有天气数据时主界面
 *       会通过 UpdateIndoor / UpdateOutdoor 接口显示占位符。
 */
void WeatherLVGL_ShowHome(void);

/* ============================================================================
 * 3. WeatherLVGL_UpdateTime —— 更新日期和数字时钟
 * ============================================================================
 */

/**
 * @brief 将一份 RTC 日期时间快照更新到 LVGL 时钟控件。
 *
 * @param date_time
 *        指向调用者提供的 RTC_DateTime_t 结构体。
 *        函数只读取快照，不修改结构体，也不直接操作 RTC 外设。
 *
 * @details
 *   - 更新 HH:MM 的 48px 标准大号数字文本。
 *   - 更新秒数标签和日期、星期文本。
 *   - 小时/分钟未变化时，避免无谓更新对应的大号标签。
 *
 * @pre WeatherLVGL_Init() 已执行成功。
 * @pre 仅由 AppTask 调用；建议每次检测到 RTC 秒数变化后调用。
 *
 * @note 如果 date_time 为 NULL，或者日期时间字段基本合法性校验
 *       未通过，函数会忽略本次更新。
 * @note 参数只在本次函数调用期间被读取，调用者不必长期保存指针。
 */
void WeatherLVGL_UpdateTime(const RTC_DateTime_t *date_time);

/* ============================================================================
 * 4. WeatherLVGL_UpdateIndoor —— 更新 AHT20 室内温湿度
 * ============================================================================
 */

/**
 * @brief 更新室内温度和相对湿度卡片。
 *
 * @param measurement
 *        指向一份 AHT20_Measurement_t 测量结果；
 *        当 valid 为 0U 时允许传入 NULL。
 *
 * @param valid
 *        1U：measurement 指向有效测量结果，应显示实际数值。
 *        0U：数据不可用，应显示“--.-”等占位文本。
 *
 * @pre WeatherLVGL_Init() 已执行成功。
 * @pre 仅由 AppTask 调用；SensorTask 不可直接调用本接口。
 *
 * @note 本函数不会触发 AHT20 的 I2C 测量，测量由 SensorTask 完成。
 * @note 参数只在当前调用期间被读取，不保存测量结果结构体指针。
 */
void WeatherLVGL_UpdateIndoor(const AHT20_Measurement_t *measurement,
                              uint8_t valid);

/* ============================================================================
 * 5. WeatherLVGL_UpdateOutdoor —— 更新网络室外天气
 * ============================================================================
 */

/**
 * @brief 更新室外天气卡片、天气状态描述及风速。
 *
 * @param weather
 *        指向一份 WeatherData_t 天气快照；
 *        当 valid 为 0U 时允许传入 NULL。
 *
 * @param valid
 *        1U：快照有效，应更新室外温度、天气现象、风速等信息。
 *        0U：数据无效或过期，应显示天气缺失占位信息。
 *
 * @pre WeatherLVGL_Init() 已执行成功。
 * @pre 仅由 AppTask 调用；NetworkTask 不可直接调用本接口。
 *
 * @note UI 的天气“有效”状态不同于 Wi-Fi 的“在线”状态。
 *       即使当前已经断网，尚未过期的本地天气快照也可能继续有效。
 * @note 该函数只显示 NetworkTask 发布的结果，不主动发起 HTTP 请求。
 * @note 参数只在当前调用期间被读取，不保存 weather 指针。
 */
void WeatherLVGL_UpdateOutdoor(const WeatherData_t *weather,
                               uint8_t valid);


/* ============================================================================
 * V3 新增接口：右上角三枚独立的运行状态图标
 * ============================================================================
 * 仅由 AppTask 调用，参数均是从业务层获得的真实状态快照。
 * wifi_has_ip  ：NetworkTask 的 ESP-AT 模块已经获取 IP，且未观察到断网。
 * weather_valid：天气数据仍在 NetworkTask 的缓存有效期内。
 * sntp_synced  ：本次启动已成功完成至少一次 SNTP->RTC 校时。
 *
 * 以上三项互相独立。例如 Wi-Fi 断开，但天气缓存仍有效时，
 * 第二枚图标仍可高亮；本地 RTC 继续运行不等于本次成功同步。
 */
void WeatherLVGL_UpdateStatus(uint8_t wifi_has_ip,
                              uint8_t weather_valid,
                              uint8_t sntp_synced);

/* ============================================================================
 * 6. WeatherLVGL_IsReady —— 查询 UI 对象是否已初始化
 * ============================================================================
 */

/**
 * @brief 查询界面对象是否已经创建成功。
 *
 * @return uint8_t
 *         1U：WeatherLVGL_Init() 已成功执行。
 *         0U：尚未初始化完成。
 *
 * @note “已初始化”不表示已显示主界面，也不表示 LCD DMA 传输成功。
 * @note 该接口是 UI 内部状态查询，不是跨任务线程同步机制；
 *       仍建议只从 AppTask 调用。
 */
uint8_t WeatherLVGL_IsReady(void);

/* C++ 兼容块结束。 */
#ifdef __cplusplus
}
#endif

#endif /* WEATHER_LVGL_UI_H */
