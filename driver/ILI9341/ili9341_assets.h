#ifndef ILI9341_ASSETS_H
#define ILI9341_ASSETS_H

/*
 * ============================================================================
 * @file    ili9341_assets.h
 * @brief   ILI9341 Assets（显示资源）统一描述与查找接口。
 * ============================================================================
 *
 * Assets = 资源。
 *
 * 这一层不保存“大块原始数据”，也不负责 SPI / DMA / ILI9341 协议。
 * 它做的是把 font_data.c / image_data.c 中的原始数组解释成 GFX 能理解的对象。
 *
 * 整体关系：
 *
 *      font_data.c ---------+
 *                           |
 *      image_data.c --------+----> ili9341_assets.c
 *                                      |
 *                                      | Font / Image / Bitmap Descriptor
 *                                      | 字体 / 图片 / 位图描述对象
 *                                      v
 *                                ili9341_gfx.c
 *                                      |
 *                                      v
 *                                  ili9341.c
 *                                      |
 *                                      v
 *                               ili9341_port.c
 *
 * 【职责边界】
 * ---------------------------------------------------------------------------
 * font_data.c / image_data.c：回答“原始数据是什么”。
 * ili9341_assets.c           ：回答“这份数据代表什么、在哪里、如何索引”。
 * ili9341_gfx.c              ：回答“怎样把它画到屏幕”。
 * ili9341.c                  ：回答“怎样按 ILI9341 协议写 GRAM”。
 * ili9341_port.c             ：回答“STM32F407 怎样通过 GPIO/SPI/DMA 发出去”。
 *
 * 因此 Assets 层不会出现 DrawLine / DrawCircle / SPI_Send 之类函数。
 * ============================================================================
 */

/* 固定宽度整数类型。 */
#include <stdint.h>

/* GFX 的 Font / Image / MonoBitmap 描述类型在这里定义。 */
#include "ili9341_gfx.h"

/* C++ 工程中保持 C Linkage（C 链接规则）。 */
#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================================
 * 1. 连续 ASCII 字体资源
 * ============================================================================
 *
 * 这些对象可以直接传给：
 *      ILI9341_GFX_DrawChar()
 *      ILI9341_GFX_DrawString()
 *      ILI9341_GFX_DrawUInt32()
 *      ILI9341_GFX_DrawInt32()
 */

/* 原 asc2_1206：实际逻辑尺寸为 6 x 12。 */
extern const ILI9341_GFX_Font_t ILI9341_ASSET_FONT_6X12;

/* 原 asc2_1608：实际逻辑尺寸为 8 x 16。 */
extern const ILI9341_GFX_Font_t ILI9341_ASSET_FONT_8X16;

/* 原 asc2_2412：实际逻辑尺寸为 12 x 24。 */
extern const ILI9341_GFX_Font_t ILI9341_ASSET_FONT_12X24;

/* 原 asc2_3216：实际逻辑尺寸为 16 x 32。 */
extern const ILI9341_GFX_Font_t ILI9341_ASSET_FONT_16X32;


/* ============================================================================
 * 2. 大号“稀疏 ASCII”字体
 * ============================================================================
 *
 * Sparse = 稀疏。
 *
 * asc2_5427 / asc2_7236 并不是从 ' ' 到 '~' 的 95 个连续字符，
 * 它们只保存少量数字、冒号、空格、减号、百分号等字符。
 *
 * 所以它们不能直接伪装成 ILI9341_GFX_Font_t；
 * Assets 层必须先根据字符找到真正的 Glyph（字形），然后交给
 * ILI9341_GFX_DrawMonoBitmapEx() 绘制。
 */
typedef struct
{
    /* 单个字形宽度，单位 Pixel。 */
    uint16_t width;

    /* 单个字形高度，单位 Pixel。 */
    uint16_t height;

    /* 每个字形占多少 Byte。 */
    uint16_t bytes_per_glyph;

    /* 总共有多少个可用字符。 */
    uint16_t glyph_count;

    /*
     * 字符顺序表。
     * 例如 "1234567890: -%" 的第 0 个字符就对应 data 中第 0 个字形。
     */
    const char *characters;

    /* 所有字形连续存放的第一个 Byte。 */
    const uint8_t *data;

} ILI9341_AssetSparseAsciiFont_t;

/* 原 asc2_5427：27 x 54，大号稀疏字符集。 */
extern const ILI9341_AssetSparseAsciiFont_t ILI9341_ASSET_FONT_27X54;

/* 原 asc2_7236：36 x 72，大号稀疏字符集。 */
extern const ILI9341_AssetSparseAsciiFont_t ILI9341_ASSET_FONT_36X72;

/**
 * @brief 在稀疏 ASCII 字库中查找一个字符。
 *
 * @param font        稀疏字体描述对象。
 * @param ch          要查找的 ASCII 字符。
 * @param out_bitmap  成功后得到可直接交给 GFX 的 1-bit Bitmap 描述。
 *
 * @return 1：找到；0：参数非法或字库中没有该字符。
 */
uint8_t ILI9341_Assets_FindSparseAsciiGlyph(
    const ILI9341_AssetSparseAsciiFont_t *font,
    char ch,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
);


/* ============================================================================
 * 3. 中文点阵资源
 * ============================================================================
 *
 * 原工程的 hz16 / hz24 / hz32 都属于“有限字符表”，并不是完整 GBK 字库。
 * 所以这里同样使用 Descriptor + Lookup（描述对象 + 查找）的方式。
 */
typedef struct
{
    /* 中文字形宽度。 */
    uint16_t width;

    /* 中文字形高度。 */
    uint16_t height;

    /* 每个字形 Msk（点阵）占多少 Byte。 */
    uint16_t bytes_per_glyph;

    /* 当前实际拥有多少个中文字符。 */
    uint16_t glyph_count;

    /* 一个底层数据 Entry（条目）总共占多少 Byte。 */
    uint16_t entry_size;

    /* 从 Entry 起点到 Index（GBK 索引）的 Byte Offset（偏移）。 */
    uint16_t index_offset;

    /* 从 Entry 起点到 Msk（点阵数据）的 Byte Offset。 */
    uint16_t bitmap_offset;

    /* 指向 hz16 / hz24 / hz32 第一个 Entry。 */
    const uint8_t *entries;

    /* 与 Entry 顺序一一对应的 Unicode Code Point（Unicode 码点）表。 */
    const uint32_t *unicode_codepoints;

} ILI9341_AssetChineseFont_t;

/* 当前恢复到的 16 x 16 中文点阵资源。 */
extern const ILI9341_AssetChineseFont_t ILI9341_ASSET_CHINESE_16;

/* 当前恢复到的 24 x 24 中文点阵资源。 */
extern const ILI9341_AssetChineseFont_t ILI9341_ASSET_CHINESE_24;

/* 当前恢复到的 32 x 32 中文点阵资源。 */
extern const ILI9341_AssetChineseFont_t ILI9341_ASSET_CHINESE_32;

/**
 * @brief 使用 Unicode Code Point（Unicode 码点）查找中文字形。
 *
 * 例如：
 *      “梅” = U+6885 -> 传入 0x6885UL。
 *
 * @return 1：找到；0：不存在或参数错误。
 */
uint8_t ILI9341_Assets_FindChineseGlyphUnicode(
    const ILI9341_AssetChineseFont_t *font,
    uint32_t codepoint,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
);

/**
 * @brief 使用原工程 GBK 双字节编码查找中文字形。
 *
 * @param gbk_high  GBK 第一个 Byte。
 * @param gbk_low   GBK 第二个 Byte。
 *
 * @return 1：找到；0：不存在或参数错误。
 */
uint8_t ILI9341_Assets_FindChineseGlyphGBK(
    const ILI9341_AssetChineseFont_t *font,
    uint8_t gbk_high,
    uint8_t gbk_low,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
);

/**
 * @brief 从 UTF-8 字符串开头读取一个字符，并在指定中文资源中查找字形。
 *
 * UTF-8 = Unicode Transformation Format - 8 bit，中文常见编码格式。
 *
 * @param utf8            指向一个 UTF-8 字符起点。
 * @param consumed_bytes  可选输出：当前 UTF-8 字符占了几个 Byte。
 * @param out_bitmap      成功后得到 1-bit 字形描述。
 *
 * @return 1：UTF-8 合法并且字库中找到；0：非法或没有该字符。
 */
uint8_t ILI9341_Assets_FindChineseGlyphUTF8(
    const ILI9341_AssetChineseFont_t *font,
    const char *utf8,
    uint32_t *consumed_bytes,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
);


/* ============================================================================
 * 4. 图片资源
 * ============================================================================
 *
 * 当前从原 Picture.c 真正恢复到的图片只有 gImage_LCD280。
 * 它已经是 RGB565 Wire Bytes，因此 Descriptor 会明确标注格式，
 * Application 只需要：
 *
 *      ILI9341_GFX_DrawImage(x, y, &ILI9341_ASSET_IMAGE_LCD280);
 *
 * 不需要自己记忆宽高或字节序。
 */
extern const ILI9341_GFX_Image_t ILI9341_ASSET_IMAGE_LCD280;


/* C++ extern "C" 结束。 */
#ifdef __cplusplus
}
#endif

/* Header Guard（头文件保护）结束。 */
#endif /* ILI9341_ASSETS_H */
