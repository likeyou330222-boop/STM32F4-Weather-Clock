/*
 * ============================================================================
 * @file    ili9341_assets.c
 * @brief   ILI9341 字体 / 中文点阵 / 图片资源统一适配实现。
 * ============================================================================
 *
 * 这个文件负责“解释资源”，不负责“绘制算法”。
 *
 * 它把：
 *      font_data.c 里的原始数组
 *      image_data.c 里的原始数组
 *
 * 转换成：
 *      ILI9341_GFX_Font_t
 *      ILI9341_GFX_MonoBitmap_t
 *      ILI9341_GFX_Image_t
 *
 * 这些 GFX 可以直接理解的描述对象。
 * ============================================================================
 */

/* 当前模块自己的公开声明。 */
#include "ili9341_assets.h"

/* 真正的字体点阵大数组从这里声明。 */
#include "font_data.h"

/* 真正的图片像素大数组从这里声明。 */
#include "image_data.h"

/* offsetof() 用于安全取得结构体字段偏移，不依赖编译器手算布局。 */
#include <stddef.h>


/* ============================================================================
 * 1. 连续 ASCII Font Descriptor（字体描述对象）
 * ============================================================================
 */

/*
 * 原 asc2_1206：
 *      字符范围 = ' ' ~ '~'；
 *      逻辑尺寸 = 6 x 12；
 *      每个 Glyph（字形）= 12 Byte；
 *      数据格式 = PC2LCD2002 逐列式 / MSB First。
 */
const ILI9341_GFX_Font_t ILI9341_ASSET_FONT_6X12 =
{
    (uint8_t)' ',                          /* first_char：第一个字符为空格。 */
    (uint8_t)'~',                          /* last_char：最后一个字符为 '~'。 */
    6U,                                    /* width：宽 6 Pixel。 */
    12U,                                   /* height：高 12 Pixel。 */
    12U,                                   /* bytes_per_glyph：每字符 12 Byte。 */
    (const uint8_t *)&asc2_1206[0][0]      /* data：第一个字形的第一个 Byte。 */
};

/* 原 asc2_1608：8 x 16。 */
const ILI9341_GFX_Font_t ILI9341_ASSET_FONT_8X16 =
{
    (uint8_t)' ',                          /* 第一个可打印 ASCII。 */
    (uint8_t)'~',                          /* 最后一个可打印 ASCII。 */
    8U,                                    /* 字符宽度。 */
    16U,                                   /* 字符高度。 */
    16U,                                   /* 每个字形 16 Byte。 */
    (const uint8_t *)&asc2_1608[0][0]      /* 指向原始点阵。 */
};

/* 原 asc2_2412：12 x 24。 */
const ILI9341_GFX_Font_t ILI9341_ASSET_FONT_12X24 =
{
    (uint8_t)' ',                          /* first_char。 */
    (uint8_t)'~',                          /* last_char。 */
    12U,                                   /* width。 */
    24U,                                   /* height。 */
    36U,                                   /* 12 * ceil(24/8) = 36 Byte。 */
    (const uint8_t *)&asc2_2412[0][0]      /* 原始字模地址。 */
};

/* 原 asc2_3216：16 x 32。 */
const ILI9341_GFX_Font_t ILI9341_ASSET_FONT_16X32 =
{
    (uint8_t)' ',                          /* first_char。 */
    (uint8_t)'~',                          /* last_char。 */
    16U,                                   /* width。 */
    32U,                                   /* height。 */
    64U,                                   /* 16 * ceil(32/8) = 64 Byte。 */
    (const uint8_t *)&asc2_3216[0][0]      /* 原始字模地址。 */
};


/* ============================================================================
 * 2. 大号稀疏 ASCII Font Descriptor
 * ============================================================================
 */

/*
 * asc2_5427 在原 FONT.c 中的真实顺序：
 *      1 2 3 4 5 6 7 8 9 0 : [space] - %
 *
 * 注意：这个顺序不是 ASCII 编码顺序，所以必须保存 characters 映射表。
 */
static const char s_sparse_27x54_characters[] = "1234567890: -%";

/* 27 x 54 大号稀疏字体描述。 */
const ILI9341_AssetSparseAsciiFont_t ILI9341_ASSET_FONT_27X54 =
{
    27U,                                   /* 单字符宽度。 */
    54U,                                   /* 单字符高度。 */
    189U,                                  /* 27 * ceil(54/8) = 189 Byte。 */
    ILI9341_FONT_DATA_ASCII_27X54_COUNT,   /* 实际字符数量 = 14。 */
    s_sparse_27x54_characters,             /* 字符到数组下标的映射顺序。 */
    (const uint8_t *)&asc2_5427[0][0]      /* 第一个大号字形。 */
};

/* asc2_7236 没有 '%'，其余顺序与上面一致。 */
static const char s_sparse_36x72_characters[] = "1234567890: -";

/* 36 x 72 大号稀疏字体描述。 */
const ILI9341_AssetSparseAsciiFont_t ILI9341_ASSET_FONT_36X72 =
{
    36U,                                   /* 单字符宽度。 */
    72U,                                   /* 单字符高度。 */
    324U,                                  /* 36 * (72/8) = 324 Byte。 */
    ILI9341_FONT_DATA_ASCII_36X72_COUNT,   /* 实际字符数量 = 13。 */
    s_sparse_36x72_characters,             /* 字符顺序。 */
    (const uint8_t *)&asc2_7236[0][0]      /* 原始数据。 */
};


/* ============================================================================
 * 3. 中文 Unicode Code Point（Unicode 码点）映射表
 * ============================================================================
 *
 * 为什么还要有 Unicode 表：
 *      原始数据的 Index 是 GBK 两个 Byte；
 *      新应用代码很可能使用 UTF-8；
 *      UTF-8 解码后得到 Unicode Code Point，更适合做统一查找。
 *
 * 下列顺序必须和 font_data.c 中 hz16 / hz24 / hz32 的 Entry 顺序完全一致。
 */

/* hz16 当前实际只有“电”一个字。 */
static const uint32_t s_hz16_unicode[ILI9341_FONT_DATA_HZ16_COUNT] =
{
    0x7535UL                               /* 电：U+7535。 */
};

/* hz24 的 19 个字。 */
static const uint32_t s_hz24_unicode[ILI9341_FONT_DATA_HZ24_COUNT] =
{
    0x661FUL,                              /* 星：U+661F。 */
    0x671FUL,                              /* 期：U+671F。 */
    0x4E00UL,                              /* 一：U+4E00。 */
    0x4E8CUL,                              /* 二：U+4E8C。 */
    0x4E09UL,                              /* 三：U+4E09。 */
    0x56DBUL,                              /* 四：U+56DB。 */
    0x4E94UL,                              /* 五：U+4E94。 */
    0x516DUL,                              /* 六：U+516D。 */
    0x65E5UL,                              /* 日：U+65E5。 */
    0x5BA4UL,                              /* 室：U+5BA4。 */
    0x5185UL,                              /* 内：U+5185。 */
    0x73AFUL,                              /* 环：U+73AF。 */
    0x5883UL,                              /* 境：U+5883。 */
    0x79E6UL,                              /* 秦：U+79E6。 */
    0x7687UL,                              /* 皇：U+7687。 */
    0x5C9BUL,                              /* 岛：U+5C9B。 */
    0x8FDEUL,                              /* 连：U+8FDE。 */
    0x63A5UL,                              /* 接：U+63A5。 */
    0x4E2DUL                               /* 中：U+4E2D。 */
};

/* hz32 的 10 个字/符号。 */
static const uint32_t s_hz32_unicode[ILI9341_FONT_DATA_HZ32_COUNT] =
{
    0x6885UL,                              /* 梅：U+6885。 */
    0x82B1UL,                              /* 花：U+82B1。 */
    0x5D4CUL,                              /* 嵌：U+5D4C。 */
    0x5165UL,                              /* 入：U+5165。 */
    0x5F0FUL,                              /* 式：U+5F0F。 */
    0x5929UL,                              /* 天：U+5929。 */
    0x6C14UL,                              /* 气：U+6C14。 */
    0x65F6UL,                              /* 时：U+65F6。 */
    0x949FUL,                              /* 钟：U+949F。 */
    0x2103UL                               /* ℃：U+2103。 */
};


/* ============================================================================
 * 4. 中文 Font Descriptor
 * ============================================================================
 */

/* 16 x 16 中文资源描述。 */
const ILI9341_AssetChineseFont_t ILI9341_ASSET_CHINESE_16 =
{
    16U,                                                   /* width。 */
    16U,                                                   /* height。 */
    32U,                                                   /* bytes_per_glyph。 */
    ILI9341_FONT_DATA_HZ16_COUNT,                          /* glyph_count。 */
    (uint16_t)sizeof(ILI9341_FontDataGB16_t),              /* entry_size。 */
    (uint16_t)offsetof(ILI9341_FontDataGB16_t, Index),     /* index_offset。 */
    (uint16_t)offsetof(ILI9341_FontDataGB16_t, Msk),       /* bitmap_offset。 */
    (const uint8_t *)&hz16[0],                             /* entries。 */
    s_hz16_unicode                                         /* Unicode 映射。 */
};

/* 24 x 24 中文资源描述。 */
const ILI9341_AssetChineseFont_t ILI9341_ASSET_CHINESE_24 =
{
    24U,                                                   /* width。 */
    24U,                                                   /* height。 */
    72U,                                                   /* bytes_per_glyph。 */
    ILI9341_FONT_DATA_HZ24_COUNT,                          /* glyph_count。 */
    (uint16_t)sizeof(ILI9341_FontDataGB24_t),              /* entry_size。 */
    (uint16_t)offsetof(ILI9341_FontDataGB24_t, Index),     /* index_offset。 */
    (uint16_t)offsetof(ILI9341_FontDataGB24_t, Msk),       /* bitmap_offset。 */
    (const uint8_t *)&hz24[0],                             /* entries。 */
    s_hz24_unicode                                         /* Unicode 映射。 */
};

/* 32 x 32 中文资源描述。 */
const ILI9341_AssetChineseFont_t ILI9341_ASSET_CHINESE_32 =
{
    32U,                                                   /* width。 */
    32U,                                                   /* height。 */
    128U,                                                  /* bytes_per_glyph。 */
    ILI9341_FONT_DATA_HZ32_COUNT,                          /* glyph_count。 */
    (uint16_t)sizeof(ILI9341_FontDataGB32_t),              /* entry_size。 */
    (uint16_t)offsetof(ILI9341_FontDataGB32_t, Index),     /* index_offset。 */
    (uint16_t)offsetof(ILI9341_FontDataGB32_t, Msk),       /* bitmap_offset。 */
    (const uint8_t *)&hz32[0],                             /* entries。 */
    s_hz32_unicode                                         /* Unicode 映射。 */
};


/* ============================================================================
 * 5. 图片 Descriptor
 * ============================================================================
 */

/*
 * gImage_LCD280 已经是 High Byte -> Low Byte 的 ILI9341 Wire Bytes。
 * 所以 Format 必须写 RGB565_WIRE_BYTES，不能写 NATIVE_U16。
 */
const ILI9341_GFX_Image_t ILI9341_ASSET_IMAGE_LCD280 =
{
    ILI9341_IMAGE_LCD280_WIDTH,                            /* 图片宽度。 */
    ILI9341_IMAGE_LCD280_HEIGHT,                           /* 图片高度。 */
    (uint32_t)ILI9341_IMAGE_LCD280_WIDTH * 2UL,            /* 每行 Byte 数。 */
    ILI9341_GFX_IMAGE_FORMAT_RGB565_WIRE_BYTES,            /* 已是线上字节序。 */
    (const void *)&gImage_LCD280[0]                        /* 图片数据首地址。 */
};


/* ============================================================================
 * 6. Private Helper：构造一个可绘制的 Mono Bitmap Descriptor
 * ============================================================================
 */
static void prvBuildMonoBitmap(
    ILI9341_GFX_MonoBitmap_t *out_bitmap,
    uint16_t width,
    uint16_t height,
    uint16_t stride_bytes,
    ILI9341_GFX_MonoLayout_t layout,
    const uint8_t *data
)
{
    /* 输出对象的宽度。 */
    out_bitmap->width = width;

    /* 输出对象的高度。 */
    out_bitmap->height = height;

    /* Row/Column Major 每个主方向单元的 Byte 跨度。 */
    out_bitmap->stride_bytes = stride_bytes;

    /* 点阵在内存中的排列方式。 */
    out_bitmap->layout = layout;

    /* 真正的点阵数据地址。 */
    out_bitmap->data = data;
}


/* ============================================================================
 * 7. Public Lookup：稀疏 ASCII 字形查找
 * ============================================================================
 */
uint8_t ILI9341_Assets_FindSparseAsciiGlyph(
    const ILI9341_AssetSparseAsciiFont_t *font,
    char ch,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
)
{
    /* 扫描字符顺序表使用的下标。 */
    uint16_t index;

    /* Column-Major 每一列占多少 Byte。 */
    uint16_t column_stride;

    /* 当前字形在整个连续数据区中的起始地址。 */
    const uint8_t *glyph_data;

    /* font / out_bitmap / 内部数据指针都必须存在。 */
    if ((font == NULL) ||
        (out_bitmap == NULL) ||
        (font->characters == NULL) ||
        (font->data == NULL))
    {
        /* 参数不完整。 */
        return 0U;
    }

    /* 宽、高、字符数、每字节数都必须有效。 */
    if ((font->width == 0U) ||
        (font->height == 0U) ||
        (font->glyph_count == 0U) ||
        (font->bytes_per_glyph == 0U))
    {
        /* 非法 Descriptor。 */
        return 0U;
    }

    /* 一列需要 ceil(height / 8) Byte。 */
    column_stride =
        (uint16_t)(((uint32_t)font->height + 7UL) / 8UL);

    /*
     * 一个 Column-Major 字形至少应该包含 width * column_stride Byte。
     * 如果 Descriptor 给出的 bytes_per_glyph 更小，后续索引会跨到相邻字形。
     */
    if ((uint32_t)font->bytes_per_glyph <
        ((uint32_t)font->width * (uint32_t)column_stride))
    {
        /* 拒绝不自洽的资源描述。 */
        return 0U;
    }

    /* 从第 0 个字符开始线性查找。 */
    for (index = 0U; index < font->glyph_count; ++index)
    {
        /* 当前映射字符与用户请求相同。 */
        if (font->characters[index] == ch)
        {
            /* 计算这个字形在连续大数组中的首地址。 */
            glyph_data =
                font->data +
                (uint32_t)index * (uint32_t)font->bytes_per_glyph;

            /*
             * 原 asc2_5427 / asc2_7236 都是逐列式，因此输出 Column Major。
             */
            prvBuildMonoBitmap(
                out_bitmap,
                font->width,
                font->height,
                column_stride,
                ILI9341_GFX_MONO_LAYOUT_COLUMN_MAJOR_MSB_FIRST,
                glyph_data
            );

            /* 成功找到。 */
            return 1U;
        }
    }

    /* 扫描完整个稀疏表仍没有匹配字符。 */
    return 0U;
}


/* ============================================================================
 * 8. Private Helper：按 Entry 下标取得中文 Bitmap
 * ============================================================================
 */
static uint8_t prvGetChineseGlyphByIndex(
    const ILI9341_AssetChineseFont_t *font,
    uint16_t index,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
)
{
    /* 当前 Entry 的第一个 Byte。 */
    const uint8_t *entry;

    /* 当前 Entry 中真正 Msk 点阵的第一个 Byte。 */
    const uint8_t *bitmap_data;

    /* Row-Major 中文点阵每一行需要多少 Byte。 */
    uint16_t row_stride;

    /* 参数必须存在。 */
    if ((font == NULL) || (out_bitmap == NULL))
    {
        /* 无法构造输出。 */
        return 0U;
    }

    /* index 必须落在已有字符数量内。 */
    if (index >= font->glyph_count)
    {
        /* 越界。 */
        return 0U;
    }

    /* entries 指针必须有效。 */
    if (font->entries == NULL)
    {
        /* 无底层数据。 */
        return 0U;
    }

    /* 中文位图的宽、高、字形 Byte 数和 Entry 大小都必须有效。 */
    if ((font->width == 0U) ||
        (font->height == 0U) ||
        (font->bytes_per_glyph == 0U) ||
        (font->entry_size == 0U))
    {
        /* Descriptor 自身不完整。 */
        return 0U;
    }

    /*
     * bitmap_offset + bytes_per_glyph 必须完全落在一个 Entry 内部，
     * 否则后续 bitmap_data 会跨到下一个结构体元素。
     */
    if (((uint32_t)font->bitmap_offset +
         (uint32_t)font->bytes_per_glyph) >
        (uint32_t)font->entry_size)
    {
        /* 非法结构体布局描述。 */
        return 0U;
    }

    /* 定位第 index 个结构体 Entry。 */
    entry =
        font->entries +
        (uint32_t)index * (uint32_t)font->entry_size;

    /* 在 Entry 内继续跳到 Msk 字段。 */
    bitmap_data =
        entry +
        (uint32_t)font->bitmap_offset;

    /* 中文点阵是按行保存，所以一行至少 ceil(width/8) Byte。 */
    row_stride =
        (uint16_t)(((uint32_t)font->width + 7UL) / 8UL);

    /* 构造 GFX 可以直接绘制的 Row-Major Bitmap。 */
    prvBuildMonoBitmap(
        out_bitmap,
        font->width,
        font->height,
        row_stride,
        ILI9341_GFX_MONO_LAYOUT_ROW_MAJOR_MSB_FIRST,
        bitmap_data
    );

    /* 构造成功。 */
    return 1U;
}


/* ============================================================================
 * 9. Public Lookup：Unicode 中文查找
 * ============================================================================
 */
uint8_t ILI9341_Assets_FindChineseGlyphUnicode(
    const ILI9341_AssetChineseFont_t *font,
    uint32_t codepoint,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
)
{
    /* 线性查找当前有限字符表。 */
    uint16_t index;

    /* 必须有 Font、输出对象和 Unicode 映射。 */
    if ((font == NULL) ||
        (out_bitmap == NULL) ||
        (font->unicode_codepoints == NULL))
    {
        /* 参数错误统一表达为“未找到”。 */
        return 0U;
    }

    /* 遍历当前资源中真实存在的字符。 */
    for (index = 0U; index < font->glyph_count; ++index)
    {
        /* 找到相同 Unicode Code Point。 */
        if (font->unicode_codepoints[index] == codepoint)
        {
            /* 根据同一个下标取得真正点阵。 */
            return prvGetChineseGlyphByIndex(
                font,
                index,
                out_bitmap
            );
        }
    }

    /* 当前有限字库没有这个 Unicode 字符。 */
    return 0U;
}


/* ============================================================================
 * 10. Public Lookup：GBK 中文查找
 * ============================================================================
 */
uint8_t ILI9341_Assets_FindChineseGlyphGBK(
    const ILI9341_AssetChineseFont_t *font,
    uint8_t gbk_high,
    uint8_t gbk_low,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
)
{
    /* 当前扫描哪个 Entry。 */
    uint16_t index;

    /* 当前 Entry 的第一个 Byte。 */
    const uint8_t *entry;

    /* 当前 Entry 内 Index 字段地址。 */
    const uint8_t *gbk_index;

    /* 必须有输入 Font、输出 Bitmap 和底层 Entry。 */
    if ((font == NULL) ||
        (out_bitmap == NULL) ||
        (font->entries == NULL))
    {
        /* 参数不完整。 */
        return 0U;
    }

    /*
     * GBK 查找至少需要读取两个 Index Byte，必须确认它们都位于一个 Entry 内。
     */
    if (((uint32_t)font->index_offset + 2UL) >
        (uint32_t)font->entry_size)
    {
        /* Descriptor 的 Index 布局非法。 */
        return 0U;
    }

    /* 遍历有限中文字库。 */
    for (index = 0U; index < font->glyph_count; ++index)
    {
        /* 找到当前结构体 Entry 起点。 */
        entry =
            font->entries +
            (uint32_t)index * (uint32_t)font->entry_size;

        /* 再定位到这个 Entry 的 Index 字段。 */
        gbk_index =
            entry +
            (uint32_t)font->index_offset;

        /* 比较 GBK 的两个 Byte。 */
        if ((gbk_index[0] == gbk_high) &&
            (gbk_index[1] == gbk_low))
        {
            /* 找到后构造通用 Bitmap。 */
            return prvGetChineseGlyphByIndex(
                font,
                index,
                out_bitmap
            );
        }
    }

    /* 当前字库没有对应 GBK 编码。 */
    return 0U;
}


/* ============================================================================
 * 11. Private Helper：严格读取一个 UTF-8 Code Point
 * ============================================================================
 */
static uint8_t prvDecodeUTF8One(
    const char *utf8,
    uint32_t *out_codepoint,
    uint32_t *out_consumed
)
{
    /* 按无符号 Byte 读取，避免 char 默认 signed 时出现负数。 */
    const uint8_t *s;

    /* 最终 Unicode Code Point。 */
    uint32_t codepoint;

    /* UTF-8 序列实际占用的 Byte 数。 */
    uint32_t consumed;

    /* 输入和输出必须存在。 */
    if ((utf8 == NULL) ||
        (out_codepoint == NULL) ||
        (out_consumed == NULL))
    {
        /* 无法解码。 */
        return 0U;
    }

    /* 以后全部按 uint8_t 检查位模式。 */
    s = (const uint8_t *)utf8;

    /* NUL 结束符不是一个要查找的可显示字符。 */
    if (s[0] == 0U)
    {
        /* 没有字符可解码。 */
        return 0U;
    }

    /* 0xxxxxxx：单 Byte ASCII。 */
    if (s[0] < 0x80U)
    {
        /* Code Point 就是该 Byte 自身。 */
        codepoint = (uint32_t)s[0];

        /* ASCII 只占 1 Byte。 */
        consumed = 1UL;
    }
    /* 110xxxxx 10xxxxxx：合法 2-Byte UTF-8 起始范围 C2~DF。 */
    else if ((s[0] >= 0xC2U) && (s[0] <= 0xDFU))
    {
        /*
         * 必须先确认第二 Byte 真实存在，再读取其位模式。
         * 这样即使调用者传入“只有起始 Byte + NUL”的短字符串，也不会越过结尾读取。
         */
        if (s[1] == 0U)
        {
            /* UTF-8 序列被 NUL 提前截断。 */
            return 0U;
        }

        /* 第二 Byte 必须满足 10xxxxxx。 */
        if ((s[1] & 0xC0U) != 0x80U)
        {
            /* 非法 Continuation Byte（续字节）。 */
            return 0U;
        }

        /* 拼出 11-bit Unicode 值。 */
        codepoint =
            ((uint32_t)(s[0] & 0x1FU) << 6) |
            (uint32_t)(s[1] & 0x3FU);

        /* 两字节编码。 */
        consumed = 2UL;
    }
    /* 1110xxxx：3-Byte UTF-8，绝大多数常用汉字在这里。 */
    else if ((s[0] >= 0xE0U) && (s[0] <= 0xEFU))
    {
        /* 三字节序列必须完整存在，不能在第二/第三 Byte 前遇到 NUL。 */
        if ((s[1] == 0U) || (s[2] == 0U))
        {
            /* 序列被截断。 */
            return 0U;
        }

        /* 两个 Continuation Byte 都必须是 10xxxxxx。 */
        if (((s[1] & 0xC0U) != 0x80U) ||
            ((s[2] & 0xC0U) != 0x80U))
        {
            /* 非法序列。 */
            return 0U;
        }

        /* E0 后第二 Byte 必须 >= A0，防止 Overlong（过长编码）。 */
        if ((s[0] == 0xE0U) && (s[1] < 0xA0U))
        {
            /* 非法过长编码。 */
            return 0U;
        }

        /* ED A0~BF 会落入 UTF-16 surrogate（代理项）范围，Unicode 不允许。 */
        if ((s[0] == 0xEDU) && (s[1] >= 0xA0U))
        {
            /* 拒绝 surrogate。 */
            return 0U;
        }

        /* 拼出 16-bit 范围内的 Unicode Code Point。 */
        codepoint =
            ((uint32_t)(s[0] & 0x0FU) << 12) |
            ((uint32_t)(s[1] & 0x3FU) << 6) |
            (uint32_t)(s[2] & 0x3FU);

        /* 三字节编码。 */
        consumed = 3UL;
    }
    /* 11110xxx：4-Byte UTF-8，Unicode 最大到 U+10FFFF。 */
    else if ((s[0] >= 0xF0U) && (s[0] <= 0xF4U))
    {
        /* 四字节序列必须完整存在，先检查后续三个 Byte 都没有被 NUL 截断。 */
        if ((s[1] == 0U) ||
            (s[2] == 0U) ||
            (s[3] == 0U))
        {
            /* 序列不完整。 */
            return 0U;
        }

        /* 三个 Continuation Byte 都必须合法。 */
        if (((s[1] & 0xC0U) != 0x80U) ||
            ((s[2] & 0xC0U) != 0x80U) ||
            ((s[3] & 0xC0U) != 0x80U))
        {
            /* 非法四字节序列。 */
            return 0U;
        }

        /* F0 后第二 Byte 必须 >= 90，防止 Overlong。 */
        if ((s[0] == 0xF0U) && (s[1] < 0x90U))
        {
            /* 非法。 */
            return 0U;
        }

        /* F4 后第二 Byte 最大 8F，保证 Code Point <= U+10FFFF。 */
        if ((s[0] == 0xF4U) && (s[1] > 0x8FU))
        {
            /* 超出 Unicode 合法范围。 */
            return 0U;
        }

        /* 拼出完整 21-bit Unicode Code Point。 */
        codepoint =
            ((uint32_t)(s[0] & 0x07U) << 18) |
            ((uint32_t)(s[1] & 0x3FU) << 12) |
            ((uint32_t)(s[2] & 0x3FU) << 6) |
            (uint32_t)(s[3] & 0x3FU);

        /* 四字节编码。 */
        consumed = 4UL;
    }
    else
    {
        /* 其他起始 Byte 都不是合法 UTF-8。 */
        return 0U;
    }

    /* 把解出的 Code Point 交给调用者。 */
    *out_codepoint = codepoint;

    /* 告诉调用者本字符消耗了多少 UTF-8 Byte。 */
    *out_consumed = consumed;

    /* 解码成功。 */
    return 1U;
}


/* ============================================================================
 * 12. Public Lookup：UTF-8 中文查找
 * ============================================================================
 */
uint8_t ILI9341_Assets_FindChineseGlyphUTF8(
    const ILI9341_AssetChineseFont_t *font,
    const char *utf8,
    uint32_t *consumed_bytes,
    ILI9341_GFX_MonoBitmap_t *out_bitmap
)
{
    /* UTF-8 解码得到的 Unicode Code Point。 */
    uint32_t codepoint;

    /* 当前字符占用的 UTF-8 Byte 数。 */
    uint32_t consumed;

    /* 先把可选输出清成 0，失败时调用者不会拿到旧值。 */
    if (consumed_bytes != NULL)
    {
        /* 尚未成功消费任何 Byte。 */
        *consumed_bytes = 0UL;
    }

    /* UTF-8 与输出 Bitmap 必须存在。 */
    if ((utf8 == NULL) || (out_bitmap == NULL))
    {
        /* 参数错误。 */
        return 0U;
    }

    /* 严格解码字符串开头的一个 Unicode 字符。 */
    if (prvDecodeUTF8One(
            utf8,
            &codepoint,
            &consumed
        ) == 0U)
    {
        /* UTF-8 本身非法。 */
        return 0U;
    }

    /* 再用统一 Unicode 查找路径寻找点阵。 */
    if (ILI9341_Assets_FindChineseGlyphUnicode(
            font,
            codepoint,
            out_bitmap
        ) == 0U)
    {
        /* UTF-8 合法，但当前有限字库没有这个字符。 */
        return 0U;
    }

    /* 如果调用者需要，返回这一个 UTF-8 字符占了几个 Byte。 */
    if (consumed_bytes != NULL)
    {
        /* 记录成功消费长度。 */
        *consumed_bytes = consumed;
    }

    /* 查找成功。 */
    return 1U;
}
