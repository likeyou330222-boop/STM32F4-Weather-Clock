#ifndef ILI9341_FONT_DATA_H
#define ILI9341_FONT_DATA_H

/*
 * ============================================================================
 * @file    font_data.h
 * @brief   ILI9341 显示模块的纯字体/字形数据声明。
 * ============================================================================
 *
 * 【本文件只做什么】
 *   只声明“字体数据是什么、尺寸是多少、数组叫什么”。
 *   不负责 Draw（绘制）、SPI、DMA、ILI9341 命令或 FreeRTOS。
 *
 * 【数据来源】
 *   本文件与 font_data.c 由用户本次上传的 FONT(1).c / FONT(1).H 重新整理。
 *   不凭空新增原文件中不存在的字模。
 *
 * 【重要格式】
 *   1) asc2_xxxx 系列来自 PC2LCD2002：阴码 + 逐列式（Column Major，列优先）
 *      + 顺向 + C51 格式。每个 Byte 内按 MSB First（最高有效位在前）解释。
 *   2) hz16 / hz24 / hz32 为中文/符号点阵，按行组织（Row Major，行优先），
 *      每一行从左到右，每个 Byte 的 bit7 对应该组最左侧 Pixel（像素）。
 *   3) 中文索引统一保存为明确的 GBK Byte，避免 Keil 源文件编码不同导致
 *      中文字符串字节发生变化。
 *
 * 【原文件不一致之处】
 *   - FONT(1).H 写 hz16_num=56，但 FONT(1).c 实际只有“电”1 个 16×16 字形；
 *   - FONT(1).H 声明 hz54[]，但 FONT(1).c 没有 hz54[] 的实际定义；
 *   因此本文件严格采用 C 文件中“实际存在的数据”。
 * ============================================================================
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== 连续 ASCII ================================ */
#define ILI9341_FONT_DATA_ASCII_6X12_COUNT      95U
#define ILI9341_FONT_DATA_ASCII_8X16_COUNT      95U
#define ILI9341_FONT_DATA_ASCII_12X24_COUNT     95U
#define ILI9341_FONT_DATA_ASCII_16X32_COUNT     95U

/* 6×12：' ' ~ '~' 共95个可打印 ASCII；每字12 Byte。 */
extern const uint8_t asc2_1206[ILI9341_FONT_DATA_ASCII_6X12_COUNT][12];
/* 8×16：' ' ~ '~' 共95个可打印 ASCII；每字16 Byte。 */
extern const uint8_t asc2_1608[ILI9341_FONT_DATA_ASCII_8X16_COUNT][16];
/* 12×24：' ' ~ '~' 共95个可打印 ASCII；每字36 Byte。 */
extern const uint8_t asc2_2412[ILI9341_FONT_DATA_ASCII_12X24_COUNT][36];
/* 16×32：' ' ~ '~' 共95个可打印 ASCII；每字64 Byte。 */
extern const uint8_t asc2_3216[ILI9341_FONT_DATA_ASCII_16X32_COUNT][64];

/* ============================= 大号稀疏字符 =============================== */
#define ILI9341_FONT_DATA_ASCII_27X54_COUNT     14U
#define ILI9341_FONT_DATA_ASCII_36X72_COUNT     13U

/* 27×54，14个字形：1 2 3 4 5 6 7 8 9 0 : [空格] - %。 */
extern const uint8_t asc2_5427[ILI9341_FONT_DATA_ASCII_27X54_COUNT][189];
/* 36×72，13个字形：1 2 3 4 5 6 7 8 9 0 : [空格] -。 */
extern const uint8_t asc2_7236[ILI9341_FONT_DATA_ASCII_36X72_COUNT][324];

/* ============================== 中文点阵 ================================== */
/* 16×16：GBK双字节索引 + 32 Byte点阵（16行×2 Byte）。 */
typedef struct
{
    uint8_t Index[2]; /* Index = 索引：该汉字的两个 GBK Byte。 */
    uint8_t Msk[32];  /* Msk = Mask（掩码）：16×16 的1-bit点阵。 */
} ILI9341_FontDataGB16_t;

/* 24×24：GBK双字节索引 + 72 Byte点阵（24行×3 Byte）。 */
typedef struct
{
    uint8_t Index[2];
    uint8_t Msk[72];
} ILI9341_FontDataGB24_t;

/* 32×32：保留4 Byte索引槽；当前前2 Byte为GBK，后2 Byte固定为0。 */
typedef struct
{
    uint8_t Index[4];
    uint8_t Msk[128]; /* 32行×4 Byte = 128 Byte。 */
} ILI9341_FontDataGB32_t;

#define ILI9341_FONT_DATA_HZ16_COUNT             1U
#define ILI9341_FONT_DATA_HZ24_COUNT            19U
#define ILI9341_FONT_DATA_HZ32_COUNT            10U

/* 16×16：当前实际只有“电”。 */
extern const ILI9341_FontDataGB16_t hz16[ILI9341_FONT_DATA_HZ16_COUNT];
/* 24×24：星期/室内环境/秦皇岛/连接中所需的19个汉字。 */
extern const ILI9341_FontDataGB24_t hz24[ILI9341_FONT_DATA_HZ24_COUNT];
/* 32×32：“梅花 / 嵌入式 / 天气 / 时钟 / ℃”共10个字/符号。 */
extern const ILI9341_FontDataGB32_t hz32[ILI9341_FONT_DATA_HZ32_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* ILI9341_FONT_DATA_H */
