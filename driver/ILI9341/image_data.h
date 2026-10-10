#ifndef ILI9341_IMAGE_DATA_H
#define ILI9341_IMAGE_DATA_H

/*
 * ============================================================================
 * @file    image_data.h
 * @brief   ILI9341 显示模块的纯图片像素数据声明。
 * ============================================================================
 *
 * 【数据来源】
 *   本文件与 image_data.c 严格根据本次上传的 Picture(1).c / Picture(1).h 整理。
 *
 * 【实际找到的图片数据】
 *   Picture(1).c 只真正定义了一个名字：gImage_LCD280[153600]，
 *   但由条件编译保存了“两张不同内容的图片”：
 *
 *   ILI9341_IMAGE_DATA_HORIZONTAL = 1：
 *      - 320 × 240 横屏；
 *      - 内容：多枚色彩鲜艳、带不同花纹的彩蛋/装饰蛋；
 *      - RGB565，逐行（Row Major），每 Pixel 2 Byte。
 *
 *   ILI9341_IMAGE_DATA_HORIZONTAL = 0：
 *      - 240 × 320 竖屏；
 *      - 内容：黑长发女性人物插画，周围有粉色花朵和灰绿色叶片，人物穿红色衣服；
 *      - RGB565，逐行（Row Major），每 Pixel 2 Byte。
 *
 *   这两份数据不是同一张图的旋转版本，而是两张不同内容的原图。
 *
 * 【字节序】
 *   数组已经按 LCD 线上顺序保存为 RGB565 High Byte -> Low Byte（高字节在前）。
 *   因此它属于 Wire Bytes（线上字节流），不能把 uint8_t* 强转成普通 STM32
 *   little-endian（小端）uint16_t* 后按本地 RGB565 数组处理。
 *
 * 【Picture(1).h 中只有声明、没有数据定义的名字】
 *   gImage_image_meihua / error / wifi / icon_wifi / icon_wenduji / icon_qing /
 *   icon_yintian / icon_duoyun / icon_leizhenyu / icon_na / icon_yueliang /
 *   icon_zhongxue / icon_zhongyu
 *   在本次 Picture(1).c 中均没有实际定义，所以本文件绝不伪造这些图片数据。
 * ============================================================================
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 选择原 Picture(1).c 中的哪一个 gImage_LCD280 分支：
 *   1 = 320×240 彩蛋横屏图；
 *   0 = 240×320 女性+花朵竖屏图。
 */
#ifndef ILI9341_IMAGE_DATA_HORIZONTAL
#define ILI9341_IMAGE_DATA_HORIZONTAL 1
#endif

#if (ILI9341_IMAGE_DATA_HORIZONTAL == 1)
    #define ILI9341_IMAGE_LCD280_WIDTH       320U
    #define ILI9341_IMAGE_LCD280_HEIGHT      240U
#else
    #define ILI9341_IMAGE_LCD280_WIDTH       240U
    #define ILI9341_IMAGE_LCD280_HEIGHT      320U
#endif

/* 320×240×2 与 240×320×2 都等于 153600 Byte。 */
#define ILI9341_IMAGE_LCD280_BYTES          153600UL

/*
 * gImage_LCD280：当前条件分支选中的整屏 RGB565 Wire-Byte 图片。
 * 数据顺序：Pixel0_H, Pixel0_L, Pixel1_H, Pixel1_L, ...
 */
extern const uint8_t gImage_LCD280[ILI9341_IMAGE_LCD280_BYTES];

#ifdef __cplusplus
}
#endif

#endif /* ILI9341_IMAGE_DATA_H */
