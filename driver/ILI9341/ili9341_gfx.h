#ifndef ILI9341_GFX_H
#define ILI9341_GFX_H

/*
 * ============================================================================
 * @file    ili9341_gfx.h
 * @brief   ILI9341 GFX 图形层公开接口 —— Assets 统一资源版 / 教学级超详细注释版
 * ============================================================================
 *
 * 【1. 本层在整个工程中的位置】
 *
 *      Application / UI / FreeRTOS Service
 *                    |
 *                    v
 *            ili9341_gfx.h / .c              <- 当前 GFX 图形层
 *                    |
 *                    v
 *              ili9341.h / .c                <- ILI9341 Controller Core
 *                    |
 *                    v
 *          ili9341_port.h / .c               <- STM32F407 Port
 *
 * GFX = Graphics，中文可理解为“图形绘制层”。
 *
 * Core 层已经负责：
 *      1) ILI9341 初始化；
 *      2) CASET / PASET / RAMWR；
 *      3) RGB565 高字节先发的线序转换；
 *      4) Blocking / DMA 数据发送；
 *      5) Rotation、Width、Height；
 *      6) BeginWrite -> Write -> EndWrite 的 RAMWR Transaction。
 *
 * 因此 GFX 层绝不直接操作：
 *      GPIO、SPI、DMA、CS、DC、ILI9341 Command、STM32 寄存器。
 *
 * GFX 层只负责“画什么”和“几何算法”：
 *      Pixel、直线、矩形、圆、区域填充、RGB565 图片、1-bit 图标、
 *      ASCII 点阵字符、字符串和整数。
 *
 * 【2. 返回值约定】
 *
 * 本层直接复用 ILI9341_Status_t：
 *      ILI9341_OK                    成功；
 *      ILI9341_ERROR_PARAM           参数错误；
 *      ILI9341_ERROR_NOT_INITIALIZED Core 尚未初始化；
 *      ILI9341_ERROR_BUSY            Core / DMA 正忙；
 *      ILI9341_ERROR_TIMEOUT         底层超时；
 *      ILI9341_ERROR_DMA             DMA 错误；
 *      ILI9341_ERROR_HW              底层硬件故障；
 *      ILI9341_ERROR_STATE           调用时序/状态不正确。
 *
 * 注意：
 *      图元“完全在屏幕外”属于正常裁剪结果，不属于参数错误，
 *      因此通常返回 ILI9341_OK，并且不会访问 Core。
 *
 * 【3. 线程安全】
 *
 * 本层本身不创建 Mutex，也不依赖 FreeRTOS。
 * 在多任务系统中，不允许多个任务同时直接调用 GFX/Core；
 * 后续 ili9341_freertos.c 应负责将 LCD 访问串行化。
 *
 * 【4. 坐标与裁剪】
 *
 * GFX 公共坐标使用 int32_t，是为了允许传入负坐标：
 *      一部分在屏幕外 -> 只绘制可见部分；
 *      完全在屏幕外   -> 什么也不画并返回 ILI9341_OK。
 *
 * Core 层继续保持严格的 uint16_t 合法坐标检查。
 * GFX 会先裁剪，确认坐标在屏幕内以后才把 uint16_t 坐标交给 Core。
 *
 * 【5. V2 逻辑修订】
 *
 * 相比初版，本版重点补强了极端坐标安全：
 *      - 矩形边缘 x + width / y + height 使用 int64_t 中间量；
 *      - 圆的 x0 +/- radius / y0 +/- radius 使用 int64_t 中间量；
 *      - 位图、字符绘制先裁剪源区域，避免极端坐标加法溢出；
 *      - 字符串游标使用 int64_t，避免长字符串累加造成 int32_t 溢出；
 *      - Cohen-Sutherland 线段裁剪使用安全的有符号乘除辅助函数，
 *        避免极端 int32_t 端点在 dx * delta 中造成 int64_t 有符号溢出。
 *
 * 公共 API 名称与参数保持不变，因此上层调用方式不需要修改。
 * ============================================================================
 */

/* uint8_t / uint16_t / uint32_t / int32_t 等固定宽度整数类型。 */
#include <stdint.h>

/* NULL / size_t 等通用定义。 */
#include <stddef.h>

/* GFX 只通过 Core 层公开接口访问 LCD。 */
#include "ili9341.h"

/* C++ 工程包含本头文件时，要求这些函数保持 C linkage。 */
#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================================
 * 1. 常用 RGB565 颜色
 * ============================================================================
 *
 * RGB565：
 *      R 占 5 bit；
 *      G 占 6 bit；
 *      B 占 5 bit；
 *      总计 16 bit，即一个 uint16_t。
 */
#define ILI9341_GFX_COLOR_BLACK          0x0000U    /* 黑色：R=0,   G=0,   B=0。 */
#define ILI9341_GFX_COLOR_WHITE          0xFFFFU    /* 白色：R=max, G=max, B=max。 */
#define ILI9341_GFX_COLOR_RED            0xF800U    /* 红色：R=max, G=0,   B=0。 */
#define ILI9341_GFX_COLOR_GREEN          0x07E0U    /* 绿色：R=0,   G=max, B=0。 */
#define ILI9341_GFX_COLOR_BLUE           0x001FU    /* 蓝色：R=0,   G=0,   B=max。 */
#define ILI9341_GFX_COLOR_YELLOW         0xFFE0U    /* 黄色：红 + 绿。 */
#define ILI9341_GFX_COLOR_CYAN           0x07FFU    /* 青色：绿 + 蓝。 */
#define ILI9341_GFX_COLOR_MAGENTA        0xF81FU    /* 品红：红 + 蓝。 */
#define ILI9341_GFX_COLOR_GRAY           0x8410U    /* 中灰。 */
#define ILI9341_GFX_COLOR_LIGHT_GRAY     0xC618U    /* 浅灰。 */
#define ILI9341_GFX_COLOR_DARK_BLUE      0x01CFU    /* 深蓝。 */


/* ============================================================================
 * 2. 文本 / 单色位图绘制模式
 * ============================================================================
 */
typedef enum
{
    /*
     * Transparent = 透明模式。
     *
     * Bit = 1：绘制 foreground（前景色）；
     * Bit = 0：不写 LCD，保留原来的屏幕内容。
     */
    ILI9341_GFX_MODE_TRANSPARENT = 0,

    /*
     * Opaque = 不透明模式。
     *
     * Bit = 1：绘制 foreground（前景色）；
     * Bit = 0：绘制 background（背景色）。
     */
    ILI9341_GFX_MODE_OPAQUE

} ILI9341_GFX_DrawMode_t;


/* ============================================================================
 * 3. ASCII 点阵字体描述
 * ============================================================================
 *
 * 本结构专门兼容原工程 FONT.c 的 PC2LCD2002 字模格式：
 *      阴码 + 逐列式 + 顺向 + C51 格式。
 *
 * 原工程数组：
 *      asc2_1206[95][12] -> 实际字形宽 6、高 12、每字 12 Byte；
 *      asc2_1608[95][16] -> 实际字形宽 8、高 16、每字 16 Byte；
 *      asc2_2412[95][36] -> 实际字形宽 12、高 24、每字 36 Byte；
 *      asc2_3216[95][64] -> 实际字形宽 16、高 32、每字 64 Byte。
 *
 * “1206”不是 12x12，而是常见命名“高度 12、宽度 6”。
 *
 * 每个字形的存储规则：
 *      1) 按列保存；
 *      2) 每列从上到下；
 *      3) 每个 Byte 的 bit7 对应该 8 Pixel 小段中的最上方 Pixel；
 *      4) 字体高度不是 8 的整数倍时，最后一个 Byte 的尾部 bit 不使用。
 *
 * 典型适配示例：
 *
 *      static const ILI9341_GFX_Font_t Font_1206 =
 *      {
 *          ' ', '~',
 *          6U, 12U,
 *          12U,
 *          &asc2_1206[0][0]
 *      };
 */
typedef struct
{
    /* 字库中第一个可用字符编码，例如普通 ASCII 字库通常为空格 ' '。 */
    uint8_t first_char;

    /* 字库中最后一个可用字符编码，例如普通 ASCII 字库通常为 '~'。 */
    uint8_t last_char;

    /* 单个字符的逻辑宽度，单位为 Pixel。 */
    uint16_t width;

    /* 单个字符的逻辑高度，单位为 Pixel。 */
    uint16_t height;

    /* 单个字符在 data 中占用的 Byte 数。 */
    uint16_t bytes_per_glyph;

    /* 指向连续字库数据的只读指针。 */
    const uint8_t *data;

} ILI9341_GFX_Font_t;


/* ============================================================================
 * 3.1 通用 1-bit Bitmap 描述
 * ============================================================================
 *
 * Bitmap = 位图。这里专门描述“每个 Pixel 只有 0 / 1 两种状态”的单色资源。
 *
 * 为什么需要这个结构：
 *      原工程里的中文 hz16 / hz24 / hz32 是“按行存储”；
 *      原工程里的大号 asc2_5427 / asc2_7236 是“按列存储”。
 *
 * 如果 GFX 只支持一种布局，Assets 层就必须复制或转置原始数据，既浪费 Flash，
 * 又容易破坏原字库。因此 GFX 明确支持 Row Major 和 Column Major 两种布局。
 */
typedef enum
{
    /*
     * Row Major = 行优先。
     * 一行一行保存，每个 Byte 内 Bit7 对应该 8 Pixel 中最左边的 Pixel。
     * 中文 16x16 / 24x24 / 32x32 点阵使用这一类。
     */
    ILI9341_GFX_MONO_LAYOUT_ROW_MAJOR_MSB_FIRST = 0,

    /*
     * Column Major = 列优先。
     * 一列一列保存，每个 Byte 内 Bit7 对应该 8 Pixel 中最上方的 Pixel。
     * 原 PC2LCD2002 ASCII 字库使用这一类。
     */
    ILI9341_GFX_MONO_LAYOUT_COLUMN_MAJOR_MSB_FIRST

} ILI9341_GFX_MonoLayout_t;


typedef struct
{
    /* 位图逻辑宽度，单位 Pixel（像素）。 */
    uint16_t width;

    /* 位图逻辑高度，单位 Pixel。 */
    uint16_t height;

    /*
     * Major Line Stride（主方向跨度），单位 Byte。
     *
     * Row Major：表示“一行占多少 Byte”；
     * Column Major：表示“一列占多少 Byte”。
     */
    uint16_t stride_bytes;

    /* 原始点阵实际采用哪一种存储布局。 */
    ILI9341_GFX_MonoLayout_t layout;

    /* 指向只读 1-bit 点阵数据。 */
    const uint8_t *data;

} ILI9341_GFX_MonoBitmap_t;


/* ============================================================================
 * 3.2 通用 RGB565 Image 描述
 * ============================================================================
 *
 * Image = 图片。
 *
 * STM32 上常见的 RGB565 图片实际上有两种完全不同的内存格式：
 *
 * 1) Native U16（MCU 本地 uint16_t）
 *      RAM 中保存 uint16_t Pixel，例如红色数值为 0xF800；
 *      STM32 是 Little Endian（小端），内存 Byte 顺序通常是 00 F8；
 *      所以发送前必须由 Core 调整成 LCD 需要的 F8 00。
 *
 * 2) Wire Bytes（LCD 线上 Byte Stream）
 *      数据本身已经排列成 High Byte -> Low Byte；
 *      例如红色直接保存 F8 00；
 *      原工程 Picture.c 的 gImage_LCD280 就属于这一类。
 *
 * 两类数据不能靠强制类型转换互换，所以这里把 Format（格式）明确记录下来。
 */
typedef enum
{
    /* 普通 STM32 uint16_t RGB565 Pixel 数组。 */
    ILI9341_GFX_IMAGE_FORMAT_RGB565_NATIVE_U16 = 0,

    /* 已经按 ILI9341 线上顺序准备好的 High-Byte / Low-Byte 数据。 */
    ILI9341_GFX_IMAGE_FORMAT_RGB565_WIRE_BYTES

} ILI9341_GFX_ImageFormat_t;


typedef struct
{
    /* 图片宽度，单位 Pixel。 */
    uint16_t width;

    /* 图片高度，单位 Pixel。 */
    uint16_t height;

    /*
     * 每一源图行实际占用多少 Byte。
     * 紧密 RGB565 图片通常就是 width * 2。
     */
    uint32_t stride_bytes;

    /* 图片内存采用哪一种 RGB565 格式。 */
    ILI9341_GFX_ImageFormat_t format;

    /* 指向实际图片数据；具体解释由 format 决定。 */
    const void *data;

} ILI9341_GFX_Image_t;


/* ============================================================================
 * 4. 基础 Pixel / 填充
 * ============================================================================
 */

/**
 * @brief  绘制一个 Pixel（像素）。
 * @param  x      X 坐标；允许负数，屏幕外会被裁剪。
 * @param  y      Y 坐标；允许负数，屏幕外会被裁剪。
 * @param  color  RGB565 颜色。
 * @return Core/GFX 状态码；完全在屏幕外返回 ILI9341_OK。
 */
ILI9341_Status_t ILI9341_GFX_DrawPixel(
    int32_t x,
    int32_t y,
    uint16_t color
);

/**
 * @brief  使用一个 RGB565 颜色填满当前逻辑屏幕。
 * @param  color  RGB565 颜色。
 * @return Core 状态码。
 */
ILI9341_Status_t ILI9341_GFX_FillScreen(uint16_t color);

/**
 * @brief  填充矩形。
 * @param  x       左上角 X；允许负数，GFX 会裁剪。
 * @param  y       左上角 Y；允许负数，GFX 会裁剪。
 * @param  width   矩形宽度，必须 > 0。
 * @param  height  矩形高度，必须 > 0。
 * @param  color   RGB565 颜色。
 * @return 成功、参数错误或 Core 返回的状态。
 */
ILI9341_Status_t ILI9341_GFX_FillRect(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    uint16_t color
);


/* ============================================================================
 * 5. 线 / 矩形
 * ============================================================================
 */

/**
 * @brief  绘制水平快速线，本质上走“高为 1 的 FillRect”快速路径。
 * @param  x       起点 X。
 * @param  y       起点 Y。
 * @param  length  线长，单位 Pixel，必须 > 0。
 * @param  color   RGB565 颜色。
 */
ILI9341_Status_t ILI9341_GFX_DrawFastHLine(
    int32_t x,
    int32_t y,
    uint16_t length,
    uint16_t color
);

/**
 * @brief  绘制垂直快速线，本质上走“宽为 1 的 FillRect”快速路径。
 * @param  x       起点 X。
 * @param  y       起点 Y。
 * @param  length  线长，单位 Pixel，必须 > 0。
 * @param  color   RGB565 颜色。
 */
ILI9341_Status_t ILI9341_GFX_DrawFastVLine(
    int32_t x,
    int32_t y,
    uint16_t length,
    uint16_t color
);

/**
 * @brief  使用 Cohen-Sutherland 裁剪 + Bresenham 算法绘制任意直线。
 * @param  x0     起点 X。
 * @param  y0     起点 Y。
 * @param  x1     终点 X。
 * @param  y1     终点 Y。
 * @param  color  RGB565 颜色。
 */
ILI9341_Status_t ILI9341_GFX_DrawLine(
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    uint16_t color
);

/**
 * @brief  绘制空心矩形，只绘制四条边。
 * @param  x       原始矩形左上角 X。
 * @param  y       原始矩形左上角 Y。
 * @param  width   原始矩形宽度，必须 > 0。
 * @param  height  原始矩形高度，必须 > 0。
 * @param  color   RGB565 颜色。
 *
 * @note   每条原始边独立裁剪，不会因为矩形部分越界而人为补出“裁剪边”。
 */
ILI9341_Status_t ILI9341_GFX_DrawRect(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    uint16_t color
);


/* ============================================================================
 * 6. 圆
 * ============================================================================
 */

/**
 * @brief  使用中点圆算法绘制空心圆。
 * @param  x0      圆心 X。
 * @param  y0      圆心 Y。
 * @param  radius  半径；支持完整 uint16_t 范围，极端大圆可能计算较慢。
 * @param  color   RGB565 颜色。
 */
ILI9341_Status_t ILI9341_GFX_DrawCircle(
    int32_t x0,
    int32_t y0,
    uint16_t radius,
    uint16_t color
);

/**
 * @brief  使用中点圆算法 + 水平 Span（扫描线）填充实心圆。
 * @param  x0      圆心 X。
 * @param  y0      圆心 Y。
 * @param  radius  半径；支持完整 uint16_t 范围。
 * @param  color   RGB565 颜色。
 */
ILI9341_Status_t ILI9341_GFX_FillCircle(
    int32_t x0,
    int32_t y0,
    uint16_t radius,
    uint16_t color
);


/* ============================================================================
 * 7. RGB565 图片
 * ============================================================================
 */

/**
 * @brief  绘制行紧密排列的 RGB565 图片。
 * @param  x       图片左上角目标 X。
 * @param  y       图片左上角目标 Y。
 * @param  width   源图片宽度，必须 > 0。
 * @param  height  源图片高度，必须 > 0。
 * @param  pixels  普通 STM32 uint16_t RGB565 数组。
 *
 * @note   GFX 内部复用 ILI9341_WritePixelsBlocking()，因此会由 Core 自动
 *         完成“uint16_t 小端内存 -> ILI9341 high-byte first”转换。
 */
ILI9341_Status_t ILI9341_GFX_DrawImageRGB565(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    const uint16_t *pixels
);

/**
 * @brief  绘制带源图行跨度（stride）的 RGB565 图片。
 * @param  x               图片左上角目标 X。
 * @param  y               图片左上角目标 Y。
 * @param  width           实际图片宽度。
 * @param  height          实际图片高度。
 * @param  stride_pixels   源图每一行在内存中占多少 Pixel；必须 >= width。
 * @param  pixels          源图首 Pixel 指针。
 */
ILI9341_Status_t ILI9341_GFX_DrawImageRGB565Stride(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    uint16_t stride_pixels,
    const uint16_t *pixels
);

/**
 * @brief  绘制已经按 ILI9341 线上顺序准备好的 RGB565 Byte 图片。
 *
 * @param  x           图片左上角目标 X。
 * @param  y           图片左上角目标 Y。
 * @param  width       图片宽度。
 * @param  height      图片高度。
 * @param  wire_bytes  High Byte -> Low Byte 排列的原始像素字节。
 *
 * @note   Wire = 线上传输格式。此函数不会交换每个 Pixel 的两个 Byte。
 *         原工程 Picture.c 的 gImage_LCD280 应走这个接口，而不是强转 uint16_t*。
 */
ILI9341_Status_t ILI9341_GFX_DrawImageRGB565Bytes(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t *wire_bytes
);

/**
 * @brief  绘制带行跨度的 RGB565 Wire-Byte 图片。
 * @param  stride_bytes  源图每一行实际占用的 Byte 数，必须 >= width * 2。
 */
ILI9341_Status_t ILI9341_GFX_DrawImageRGB565BytesStride(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    uint32_t stride_bytes,
    const uint8_t *wire_bytes
);

/**
 * @brief  根据 ILI9341_GFX_Image_t 的 format 自动选择正确图片通路。
 *
 * 这个接口让 Application（应用层）不必自己判断：
 *      “这是 uint16_t 图片还是 LCD Wire Bytes 图片？”
 * Assets 层只需要给出正确的 Image Descriptor（图片描述对象）即可。
 */
ILI9341_Status_t ILI9341_GFX_DrawImage(
    int32_t x,
    int32_t y,
    const ILI9341_GFX_Image_t *image
);


/* ============================================================================
 * 8. 1-bit 单色 Bitmap / Icon
 * ============================================================================
 *
 * bitmap 格式：按行存储、MSB first。
 *
 *      每一行从左到右；
 *      bit7 对应该 Byte 中最左边 Pixel；
 *      stride_bytes 表示源图每一行实际占多少 Byte。
 */

/**
 * @brief  绘制 1-bit 单色位图/图标。
 * @param  x             位图左上角目标 X。
 * @param  y             位图左上角目标 Y。
 * @param  width         位图宽度，必须 > 0。
 * @param  height        位图高度，必须 > 0。
 * @param  bitmap        源位图数据。
 * @param  stride_bytes  源位图每行 Byte 数，必须 >= ceil(width / 8)。
 * @param  foreground    bit=1 时的前景色。
 * @param  background    bit=0 时的不透明背景色；透明模式时不使用。
 * @param  mode          透明 / 不透明模式。
 */
ILI9341_Status_t ILI9341_GFX_DrawMonoBitmap(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t *bitmap,
    uint16_t stride_bytes,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
);

/**
 * @brief  绘制一个带 Layout（存储布局）描述的通用 1-bit Bitmap。
 *
 * Ex = Extended，中文可理解为“扩展版本”。
 *
 * 这个接口既能直接画中文 Row-Major 点阵，也能直接画原工程
 * asc2_5427 / asc2_7236 那种 Column-Major 大号字符，不需要转置数据。
 */
ILI9341_Status_t ILI9341_GFX_DrawMonoBitmapEx(
    int32_t x,
    int32_t y,
    const ILI9341_GFX_MonoBitmap_t *bitmap,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
);


/* ============================================================================
 * 9. ASCII 字符 / 字符串 / 整数
 * ============================================================================
 */

/**
 * @brief  绘制一个 ASCII 点阵字符。
 * @param  x           字符左上角 X。
 * @param  y           字符左上角 Y。
 * @param  ch          要显示的字符。
 * @param  font        字体描述结构。
 * @param  foreground  前景色。
 * @param  background  不透明模式背景色。
 * @param  mode        透明 / 不透明。
 *
 * @note   字库中不存在的字符会尝试使用 '?'；若 '?' 也不存在则返回参数错误。
 */
ILI9341_Status_t ILI9341_GFX_DrawChar(
    int32_t x,
    int32_t y,
    char ch,
    const ILI9341_GFX_Font_t *font,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
);

/**
 * @brief  绘制以 '\0' 结束的 ASCII 字符串。
 * @param  x           第一行起始 X。
 * @param  y           第一行起始 Y。
 * @param  text        NUL 结尾字符串。
 * @param  font        字体描述结构。
 * @param  foreground  前景色。
 * @param  background  不透明模式背景色。
 * @param  mode        透明 / 不透明。
 *
 * 控制字符：
 *      '\r'：忽略；
 *      '\n'：Y 增加一个字体高度，并把 X 恢复到初始 X。
 */
ILI9341_Status_t ILI9341_GFX_DrawString(
    int32_t x,
    int32_t y,
    const char *text,
    const ILI9341_GFX_Font_t *font,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
);

/**
 * @brief  将 uint32_t 转成十进制字符串并绘制。
 * @note   完整支持 0 ~ 4294967295。
 */
ILI9341_Status_t ILI9341_GFX_DrawUInt32(
    int32_t x,
    int32_t y,
    uint32_t value,
    const ILI9341_GFX_Font_t *font,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
);

/**
 * @brief  将 int32_t 转成十进制字符串并绘制。
 * @note   特别处理 INT32_MIN，避免直接执行 -INT32_MIN 产生有符号溢出。
 */
ILI9341_Status_t ILI9341_GFX_DrawInt32(
    int32_t x,
    int32_t y,
    int32_t value,
    const ILI9341_GFX_Font_t *font,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
);


/* C++ extern "C" 作用域结束。 */
#ifdef __cplusplus
}
#endif

/* 头文件保护结束。 */
#endif /* ILI9341_GFX_H */
