#include "ili9341_gfx.h"

/*
 * ============================================================================
 * @file    ili9341_gfx.c
 * @brief   ILI9341 GFX 图形层实现 —— Assets 统一资源版 + 逻辑复核 + 超详细中文注释版
 * ============================================================================
 *
 * 【设计边界】
 *
 * 本文件只依赖 ili9341.h 暴露出来的 Core API。
 * 本文件绝不直接：
 *      - 拉低/拉高 CS；
 *      - 切换 DC；
 *      - 操作 SPI；
 *      - 操作 DMA；
 *      - 发送 0x2A / 0x2B / 0x2C；
 *      - 访问 STM32 寄存器。
 *
 * 这样做的直接结果是：
 *      “图形算法”与“STM32F407 硬件 Port”完全解耦。
 *
 * 【本版重点复核】
 *
 * 1) 所有公开坐标均允许 int32_t 极值；
 * 2) 所有可能发生 x + width、x0 +/- radius 的地方优先使用 int64_t；
 * 3) 线段裁剪避免 signed int64_t 乘法溢出；
 * 4) 字符/位图先做目标矩形裁剪，再扫描可见源区域；
 * 5) 长字符串游标使用 int64_t，避免 int32_t 逐字符累加溢出；
 * 6) 所有 Core 事务错误继续沿用 BeginWrite -> AbortWrite/EndWrite 纪律。
 *
 * ============================================================================
 */


/* ============================================================================
 * 1. 私有工具：把 int64_t 的绝对值安全转换为 uint64_t
 * ============================================================================
 *
 * 不能简单对 INT64_MIN 做 -value，因为它的正值无法放入 int64_t。
 * 这里使用 “-(value + 1) + 1” 的形式规避这个问题。
 */
static uint64_t prvAbsI64ToU64(int64_t value)
{
    /* 非负数可以直接转换成无符号 64 位。 */
    if (value >= 0)
    {
        /* 返回完全相同的数值。 */
        return (uint64_t)value;
    }

    /*
     * value < 0 时先做 value + 1，保证即使 value == INT64_MIN 也可表示，
     * 再取负并补回 1，得到准确的绝对值幅度。
     */
    return (uint64_t)(-(value + 1)) + 1ULL;
}


/* ============================================================================
 * 2. 私有工具：安全计算 (a * b) / divisor
 * ============================================================================
 *
 * Cohen-Sutherland 裁剪需要计算：
 *
 *      dx * delta / dy
 *
 * a / b / divisor 的来源虽然都是 int32_t 坐标差，但：
 *      |dx| 最大可接近 2^32 - 1；
 *      |delta| 也可能超过 2^31；
 *
 * 二者直接使用 int64_t 有符号乘法时，极端坐标会超过 INT64_MAX，
 * 形成 C 语言未定义行为。
 *
 * 但是它们的“绝对值乘积”仍小于 UINT64_MAX，因此这里：
 *      1) 分离符号；
 *      2) 用 uint64_t 做绝对值乘法；
 *      3) 再除以 divisor；
 *      4) 最后恢复符号。
 */
static uint8_t prvMulDivI64(int64_t a, int64_t b, int64_t divisor, int64_t *result)
{
    /* 保存三个操作数的无符号绝对值。 */
    uint64_t ua;
    uint64_t ub;
    uint64_t ud;

    /* 保存无符号乘积。 */
    uint64_t product;

    /* 保存无符号除法结果。 */
    uint64_t quotient;

    /* negative = 1 表示最终结果应该为负数。 */
    uint8_t negative;

    /* 输出指针为空时不能写结果。 */
    if (result == NULL)
    {
        /* 返回 0 表示计算失败。 */
        return 0U;
    }

    /* 除数为 0 时计算没有定义。 */
    if (divisor == 0)
    {
        /* 返回 0，调用者负责把本次裁剪视为失败。 */
        return 0U;
    }

    /*
     * 三个数中负号出现奇数次，则最终结果为负；
     * XOR = Exclusive OR = 异或，非常适合统计符号奇偶性。
     */
    negative = (uint8_t)(
        ((a < 0) ? 1U : 0U) ^
        ((b < 0) ? 1U : 0U) ^
        ((divisor < 0) ? 1U : 0U)
    );

    /* 取得 a 的绝对值幅度。 */
    ua = prvAbsI64ToU64(a);

    /* 取得 b 的绝对值幅度。 */
    ub = prvAbsI64ToU64(b);

    /* 取得 divisor 的绝对值幅度。 */
    ud = prvAbsI64ToU64(divisor);

    /*
     * 本函数在 GFX 中的输入来源都不超过 32 位坐标差，
     * 因而 ua * ub 不会超过 uint64_t 上限。
     */
    product = ua * ub;

    /* 使用无符号除法得到结果的绝对值。 */
    quotient = product / ud;

    /* 结果为负数时恢复负号。 */
    if (negative != 0U)
    {
        /*
         * quotient == 2^63 时唯一合法负值是 INT64_MIN；
         * quotient 更大则无法转换为 int64_t。
         */
        if (quotient > (1ULL << 63))
        {
            /* 超出 int64_t 表示范围，返回失败。 */
            return 0U;
        }

        /* 正好等于 2^63 时构造 INT64_MIN。 */
        if (quotient == (1ULL << 63))
        {
            /* 用 INT64_MIN 宏避免对 2^63 做有符号正值转换。 */
            *result = INT64_MIN;
        }
        else
        {
            /* 普通负数：先安全转成 int64_t，再取负。 */
            *result = -(int64_t)quotient;
        }
    }
    else
    {
        /* 正数最大只能到 INT64_MAX。 */
        if (quotient > (uint64_t)INT64_MAX)
        {
            /* 超范围时返回失败。 */
            return 0U;
        }

        /* 正常保存非负结果。 */
        *result = (int64_t)quotient;
    }

    /* 运行到这里说明结果已经成功写入。 */
    return 1U;
}


/* ============================================================================
 * 3. 私有工具：矩形裁剪
 * ============================================================================
 *
 * 输入：
 *      *x / *y / *width / *height 描述一个可能部分越出屏幕的矩形。
 *
 * 输出：
 *      有可见区域 -> 原地改成裁剪后的矩形并返回 1；
 *      完全不可见 -> 返回 0。
 *
 * 所有边界加法都提升到 int64_t，避免：
 *      x + width
 *      y + height
 * 在 int32_t 极端参数下发生有符号溢出。
 */
static uint8_t prvClipRect(
    int32_t *x,
    int32_t *y,
    uint16_t *width,
    uint16_t *height
)
{
    /* 左边界，使用 int64_t 保存。 */
    int64_t left;

    /* 上边界，使用 int64_t 保存。 */
    int64_t top;

    /* 右边界采用右开区间 [left, right)。 */
    int64_t right;

    /* 下边界采用右开区间 [top, bottom)。 */
    int64_t bottom;

    /* 当前 Rotation 对应的逻辑屏幕宽度。 */
    int64_t screen_width;

    /* 当前 Rotation 对应的逻辑屏幕高度。 */
    int64_t screen_height;

    /* 任意输出指针为空都不能安全裁剪。 */
    if ((x == NULL) ||
        (y == NULL) ||
        (width == NULL) ||
        (height == NULL))
    {
        /* 私有布尔式函数返回 0 表示没有可用结果。 */
        return 0U;
    }

    /* 宽或高为 0 的矩形没有任何 Pixel。 */
    if ((*width == 0U) || (*height == 0U))
    {
        /* 视为不可见。 */
        return 0U;
    }

    /* 从 Core 读取当前逻辑宽度，并提升为 int64_t。 */
    screen_width = (int64_t)ILI9341_GetWidth();

    /* 从 Core 读取当前逻辑高度，并提升为 int64_t。 */
    screen_height = (int64_t)ILI9341_GetHeight();

    /* 极端防御：逻辑尺寸为 0 时不能绘图。 */
    if ((screen_width <= 0) || (screen_height <= 0))
    {
        /* 返回不可见。 */
        return 0U;
    }

    /* 保存原矩形左边界。 */
    left = (int64_t)(*x);

    /* 保存原矩形上边界。 */
    top = (int64_t)(*y);

    /* 计算右开区间右端：x + width。 */
    right = left + (int64_t)(*width);

    /* 计算右开区间下端：y + height。 */
    bottom = top + (int64_t)(*height);

    /*
     * 如果矩形在屏幕左、上、右、下任一方向完全分离，
     * 则屏幕与矩形没有交集。
     */
    if ((right <= 0) ||
        (bottom <= 0) ||
        (left >= screen_width) ||
        (top >= screen_height))
    {
        /* 完全不可见。 */
        return 0U;
    }

    /* 左边越出屏幕时，把可见左边界夹到 0。 */
    if (left < 0)
    {
        /* 屏幕最左列为 X=0。 */
        left = 0;
    }

    /* 上边越出屏幕时，把可见上边界夹到 0。 */
    if (top < 0)
    {
        /* 屏幕最上行为 Y=0。 */
        top = 0;
    }

    /* 右边超出屏幕时，把右开区间夹到 screen_width。 */
    if (right > screen_width)
    {
        /* right 是右开边界，所以直接等于 screen_width。 */
        right = screen_width;
    }

    /* 下边超出屏幕时，把下开边界夹到 screen_height。 */
    if (bottom > screen_height)
    {
        /* bottom 是右开边界，所以直接等于 screen_height。 */
        bottom = screen_height;
    }

    /* 裁剪后 left 已保证在屏幕内，可安全转回 int32_t。 */
    *x = (int32_t)left;

    /* 裁剪后 top 已保证在屏幕内，可安全转回 int32_t。 */
    *y = (int32_t)top;

    /* 可见宽度不超过 320，安全放入 uint16_t。 */
    *width = (uint16_t)(right - left);

    /* 可见高度不超过 320，安全放入 uint16_t。 */
    *height = (uint16_t)(bottom - top);

    /* 返回 1 表示存在可见矩形。 */
    return 1U;
}


/* ============================================================================
 * 4. 私有工具：判断 int64_t 坐标点是否在当前逻辑屏幕内
 * ============================================================================
 */
static uint8_t prvPointVisible64(int64_t x, int64_t y)
{
    /* 负坐标位于屏幕左侧或上侧。 */
    if ((x < 0) || (y < 0))
    {
        /* 返回 0 表示不可见。 */
        return 0U;
    }

    /* X 超过当前逻辑宽度或 Y 超过当前逻辑高度，同样不可见。 */
    if ((x >= (int64_t)ILI9341_GetWidth()) ||
        (y >= (int64_t)ILI9341_GetHeight()))
    {
        /* 返回 0 表示不可见。 */
        return 0U;
    }

    /* 两个坐标都在合法屏幕范围内。 */
    return 1U;
}


/* ============================================================================
 * 5. 私有工具：绘制一个允许越界的 Pixel（Clipped裁剪）
 * ============================================================================
 *
 * 圆算法可能生成：
 *      x0 + radius
 *      x0 - radius
 *
 * 当 x0 接近 INT32_MAX / INT32_MIN 时，用 int32_t 直接相加会产生 UB。
 * 所以这里接收 int64_t，先判断可见性，再安全缩窄到 uint16_t。
 */
static ILI9341_Status_t prvDrawPixelClipped64(
    int64_t x,
    int64_t y,
    uint16_t color
)
{
    /* 屏幕外的点属于正常裁剪结果。 */
    if (prvPointVisible64(x, y) == 0U)
    {
        /* 不访问 Core，直接成功。 */
        return ILI9341_OK;
    }

    /*
     * 到这里 x/y 必然处于 [0, width-1] / [0, height-1]，
     * 因此转换为 uint16_t 不会丢失有效信息。
     */
    return ILI9341_WritePixel(
        (uint16_t)x,
        (uint16_t)y,
        color
    );
}


/* ============================================================================
 * 6. 私有工具：绘制一个 int64_t 水平 Span（闭区间）
 * ============================================================================
 *
 * Span = 扫描线段。
 * 输入 [x0, x1] 为包含两端的闭区间。
 * 该函数主要用于：
 *      - DrawRect 的水平边；
 *      - FillCircle 的水平填充线。
 */
static ILI9341_Status_t prvDrawHSpan64(
    int64_t x0,
    int64_t x1,
    int64_t y,
    uint16_t color
)
{
    /* 当前逻辑屏幕宽度。 */
    int64_t screen_width;

    /* 临时交换变量。 */
    int64_t temp;

    /* 最终可见长度。 */
    uint16_t length;

    /* 如果两个端点顺序相反，交换后统一处理。 */
    if (x0 > x1)
    {
        /* 暂存原 x0。 */
        temp = x0;

        /* 把较小端放到 x0。 */
        x0 = x1;

        /* 把较大端放到 x1。 */
        x1 = temp;
    }

    /* Y 不在屏幕内时，整条水平线不可见。 */
    if ((y < 0) || (y >= (int64_t)ILI9341_GetHeight()))
    {
        /* 正常裁剪为无内容。 */
        return ILI9341_OK;
    }

    /* 读取当前逻辑屏幕宽度。 */
    screen_width = (int64_t)ILI9341_GetWidth();

    /* 屏幕宽度为 0 时没有可绘制 Pixel。 */
    if (screen_width <= 0)
    {
        /* 返回正常无内容。 */
        return ILI9341_OK;
    }

    /* 整条线都在屏幕左侧或右侧时没有交集。 */
    if ((x1 < 0) || (x0 >= screen_width))
    {
        /* 不访问 Core。 */
        return ILI9341_OK;
    }

    /* 左端越界时裁到 X=0。 */
    if (x0 < 0)
    {
        /* 屏幕最左 Pixel。 */
        x0 = 0;
    }

    /* 右端越界时裁到 width-1。 */
    if (x1 >= screen_width)
    {
        /* 屏幕最右 Pixel。 */
        x1 = screen_width - 1;
    }

    /* 裁剪后长度最大只有当前屏幕宽度，可安全放入 uint16_t。 */
    length = (uint16_t)(x1 - x0 + 1);

    /* 复用公共快速水平线接口，最终走 FillRect + Core Transaction。 */
    return ILI9341_GFX_DrawFastHLine(
        (int32_t)x0,
        (int32_t)y,
        length,
        color
    );
}


/* ============================================================================
 * 7. 私有工具：绘制一个 int64_t 垂直 Span（闭区间）
 * ============================================================================
 */
static ILI9341_Status_t prvDrawVSpan64(
    int64_t x,
    int64_t y0,
    int64_t y1,
    uint16_t color
)
{
    /* 当前逻辑屏幕高度。 */
    int64_t screen_height;

    /* 临时交换变量。 */
    int64_t temp;

    /* 最终可见长度。 */
    uint16_t length;

    /* 如果上下端点顺序相反，交换后统一处理。 */
    if (y0 > y1)
    {
        /* 暂存原 y0。 */
        temp = y0;

        /* 把较小端放到 y0。 */
        y0 = y1;

        /* 把较大端放到 y1。 */
        y1 = temp;
    }

    /* X 不在屏幕内时，整条垂直线不可见。 */
    if ((x < 0) || (x >= (int64_t)ILI9341_GetWidth()))
    {
        /* 正常裁剪为无内容。 */
        return ILI9341_OK;
    }

    /* 读取当前逻辑屏幕高度。 */
    screen_height = (int64_t)ILI9341_GetHeight();

    /* 屏幕高度为 0 时没有可绘制 Pixel。 */
    if (screen_height <= 0)
    {
        /* 返回正常无内容。 */
        return ILI9341_OK;
    }

    /* 整条线都在屏幕上方或下方时没有交集。 */
    if ((y1 < 0) || (y0 >= screen_height))
    {
        /* 不访问 Core。 */
        return ILI9341_OK;
    }

    /* 上端越界时裁到 Y=0。 */
    if (y0 < 0)
    {
        /* 屏幕最上 Pixel。 */
        y0 = 0;
    }

    /* 下端越界时裁到 height-1。 */
    if (y1 >= screen_height)
    {
        /* 屏幕最下 Pixel。 */
        y1 = screen_height - 1;
    }

    /* 裁剪后长度最大只有屏幕高度，可安全放入 uint16_t。 */
    length = (uint16_t)(y1 - y0 + 1);

    /* 复用公共快速垂直线接口。 */
    return ILI9341_GFX_DrawFastVLine(
        (int32_t)x,
        (int32_t)y0,
        length,
        color
    );
}


/* ============================================================================
 * 8. 私有工具：Cohen-Sutherland 线段裁剪区域码
 * ============================================================================
 *
 * 每个 bit 表示点位于屏幕哪个外部半平面。
 */
#define ILI9341_GFX_CLIP_LEFT       0x01U  /* 点位于屏幕左侧。 */
#define ILI9341_GFX_CLIP_RIGHT      0x02U  /* 点位于屏幕右侧。 */
#define ILI9341_GFX_CLIP_TOP        0x04U  /* 点位于屏幕上侧。 */
#define ILI9341_GFX_CLIP_BOTTOM     0x08U  /* 点位于屏幕下侧。 */

static uint8_t prvLineOutCode(
    int32_t x,
    int32_t y,
    int32_t max_x,
    int32_t max_y
)
{
    /* 初始认为点位于屏幕内部。 */
    uint8_t code = 0U;

    /* X 小于 0，置 LEFT bit。 */
    if (x < 0)
    {
        /* 按位或把 LEFT 标志加入 code。 */
        code |= ILI9341_GFX_CLIP_LEFT;
    }
    else if (x > max_x)
    {
        /* X 大于最右有效坐标，置 RIGHT bit。 */
        code |= ILI9341_GFX_CLIP_RIGHT;
    }

    /* Y 小于 0，置 TOP bit。 */
    if (y < 0)
    {
        /* 把 TOP 标志加入 code。 */
        code |= ILI9341_GFX_CLIP_TOP;
    }
    else if (y > max_y)
    {
        /* Y 大于最下有效坐标，置 BOTTOM bit。 */
        code |= ILI9341_GFX_CLIP_BOTTOM;
    }

    /* 返回该点对应的 4-bit 区域码。 */
    return code;
}


/* ============================================================================
 * 9. 私有工具：Cohen-Sutherland 线段裁剪
 * ============================================================================
 *
 * 返回 1：线段至少有一部分可见，且端点已修改为屏幕内裁剪结果。
 * 返回 0：整条线不可见，或输入/计算失败。
 */
static uint8_t prvClipLine(
    int32_t *x0,
    int32_t *y0,
    int32_t *x1,
    int32_t *y1
)
{
    /* 屏幕最右有效 X。 */
    int32_t max_x;

    /* 屏幕最下有效 Y。 */
    int32_t max_y;

    /* 起点区域码。 */
    uint8_t code0;

    /* 终点区域码。 */
    uint8_t code1;

    /* 当前选择裁剪的外部端点区域码。 */
    uint8_t out_code;

    /* 新交点 X，使用 int64_t 暂存。 */
    int64_t x;

    /* 新交点 Y，使用 int64_t 暂存。 */
    int64_t y;

    /* x1 - x0。 */
    int64_t dx;

    /* y1 - y0。 */
    int64_t dy;

    /* 安全乘除得到的坐标增量。 */
    int64_t delta;

    /* 任意端点指针为空都无法裁剪。 */
    if ((x0 == NULL) ||
        (y0 == NULL) ||
        (x1 == NULL) ||
        (y1 == NULL))
    {
        /* 返回不可用。 */
        return 0U;
    }

    /* 当前逻辑屏幕尺寸为 0 时没有可见区域。 */
    if ((ILI9341_GetWidth() == 0U) ||
        (ILI9341_GetHeight() == 0U))
    {
        /* 返回不可见。 */
        return 0U;
    }

    /* 最右合法坐标 = width - 1。 */
    max_x = (int32_t)ILI9341_GetWidth() - 1;

    /* 最下合法坐标 = height - 1。 */
    max_y = (int32_t)ILI9341_GetHeight() - 1;

    /* 计算起点区域码。 */
    code0 = prvLineOutCode(*x0, *y0, max_x, max_y);

    /* 计算终点区域码。 */
    code1 = prvLineOutCode(*x1, *y1, max_x, max_y);

    /* 循环不断把屏幕外端点裁到某条边界上。 */
    for (;;)
    {
        /* 两端区域码都为 0，说明两个端点都已经位于屏幕内。 */
        if ((code0 | code1) == 0U)
        {
            /* 线段可见，裁剪成功。 */
            return 1U;
        }

        /*
         * 两端在同一个外部半平面时，按位与会保留共同标志，
         * 说明整条线不可能穿过屏幕。
         */
        if ((code0 & code1) != 0U)
        {
            /* 整条线不可见。 */
            return 0U;
        }

        /* 优先选择起点中位于屏幕外的那个端点。 */
        out_code = (code0 != 0U) ? code0 : code1;

        /* 使用 int64_t 计算 X 差值，避免 int32_t 相减溢出。 */
        dx = (int64_t)(*x1) - (int64_t)(*x0);

        /* 使用 int64_t 计算 Y 差值。 */
        dy = (int64_t)(*y1) - (int64_t)(*y0);

        /* 当前外点位于屏幕上方，需要和 Y=0 求交。 */
        if ((out_code & ILI9341_GFX_CLIP_TOP) != 0U)
        {
            /* 水平线 dy=0 不可能从上方穿入屏幕。 */
            if (dy == 0)
            {
                /* 裁剪失败/不可见。 */
                return 0U;
            }

            /* 交点的 Y 固定为屏幕上边界 0。 */
            y = 0;

            /* 安全计算 dx * (y - y0) / dy。 */
            if (prvMulDivI64(
                    dx,
                    y - (int64_t)(*y0),
                    dy,
                    &delta
                ) == 0U)
            {
                /* 极端计算失败时保守地判为不可见。 */
                return 0U;
            }

            /* 新 X = x0 + 比例增量。 */
            x = (int64_t)(*x0) + delta;
        }
        /* 当前外点位于屏幕下方，需要和 Y=max_y 求交。 */
        else if ((out_code & ILI9341_GFX_CLIP_BOTTOM) != 0U)
        {
            /* 水平线 dy=0 不可能从下方穿入屏幕。 */
            if (dy == 0)
            {
                /* 返回不可见。 */
                return 0U;
            }

            /* 交点 Y 固定为屏幕下边界。 */
            y = (int64_t)max_y;

            /* 安全计算 dx * (y - y0) / dy。 */
            if (prvMulDivI64(
                    dx,
                    y - (int64_t)(*y0),
                    dy,
                    &delta
                ) == 0U)
            {
                /* 计算失败。 */
                return 0U;
            }

            /* 得到交点 X。 */
            x = (int64_t)(*x0) + delta;
        }
        /* 当前外点位于屏幕右侧，需要和 X=max_x 求交。 */
        else if ((out_code & ILI9341_GFX_CLIP_RIGHT) != 0U)
        {
            /* 垂直线 dx=0 不可能从右侧穿入屏幕。 */
            if (dx == 0)
            {
                /* 返回不可见。 */
                return 0U;
            }

            /* 交点 X 固定为屏幕右边界。 */
            x = (int64_t)max_x;

            /* 安全计算 dy * (x - x0) / dx。 */
            if (prvMulDivI64(
                    dy,
                    x - (int64_t)(*x0),
                    dx,
                    &delta
                ) == 0U)
            {
                /* 计算失败。 */
                return 0U;
            }

            /* 得到交点 Y。 */
            y = (int64_t)(*y0) + delta;
        }
        /* 剩余情况只能是位于屏幕左侧，需要和 X=0 求交。 */
        else
        {
            /* 垂直线 dx=0 不可能从左侧穿入屏幕。 */
            if (dx == 0)
            {
                /* 返回不可见。 */
                return 0U;
            }

            /* 交点 X 固定为屏幕左边界 0。 */
            x = 0;

            /* 安全计算 dy * (x - x0) / dx。 */
            if (prvMulDivI64(
                    dy,
                    x - (int64_t)(*x0),
                    dx,
                    &delta
                ) == 0U)
            {
                /* 计算失败。 */
                return 0U;
            }

            /* 得到交点 Y。 */
            y = (int64_t)(*y0) + delta;
        }

        /* 如果本轮裁剪的是起点。 */
        if (out_code == code0)
        {
            /* 插值结果位于原两端之间，因此应处于 int32_t 范围内。 */
            *x0 = (int32_t)x;

            /* 保存新的起点 Y。 */
            *y0 = (int32_t)y;

            /* 重新计算新起点区域码。 */
            code0 = prvLineOutCode(*x0, *y0, max_x, max_y);
        }
        else
        {
            /* 本轮裁剪终点，保存新的 X。 */
            *x1 = (int32_t)x;

            /* 保存新的 Y。 */
            *y1 = (int32_t)y;

            /* 重新计算新终点区域码。 */
            code1 = prvLineOutCode(*x1, *y1, max_x, max_y);
        }
    }
}


/* ============================================================================
 * 10. 私有工具：字体合法性检查
 * ============================================================================
 */
static uint8_t prvFontIsValid(const ILI9341_GFX_Font_t *font)
{
    /* 每一列需要多少 Byte 才能覆盖字体高度。 */
    uint32_t bytes_per_column;

    /* 按当前列式格式，一个字形理论最少需要多少 Byte。 */
    uint32_t minimum_bytes_per_glyph;

    /* 字体描述指针为空必然无效。 */
    if (font == NULL)
    {
        /* 返回无效。 */
        return 0U;
    }

    /* 检查所有基本字段。 */
    if ((font->data == NULL) ||
        (font->width == 0U) ||
        (font->height == 0U) ||
        (font->bytes_per_glyph == 0U) ||
        (font->last_char < font->first_char))
    {
        /* 任意基本条件不满足都判定字体无效。 */
        return 0U;
    }

    /* ceil(height / 8)，即一列纵向需要几个 Byte。 */
    bytes_per_column =
        ((uint32_t)font->height + 7UL) / 8UL;

    /* 每字至少需要 width * bytes_per_column 个 Byte。 */
    minimum_bytes_per_glyph =
        (uint32_t)font->width * bytes_per_column;

    /* 实际 bytes_per_glyph 小于理论最小值说明描述与数据格式不匹配。 */
    if ((uint32_t)font->bytes_per_glyph < minimum_bytes_per_glyph)
    {
        /* 返回无效。 */
        return 0U;
    }

    /* 所有基本结构检查均通过。 */
    return 1U;
}


/* ============================================================================
 * 11. 私有工具：读取字体中的一个 Pixel Bit
 * ============================================================================
 */
static uint8_t prvFontPixelOn(
    const ILI9341_GFX_Font_t *font,
    uint8_t ch,
    uint16_t glyph_x,
    uint16_t glyph_y
)
{
    /* 字符在 font 字符表中的索引。 */
    uint32_t glyph_index;

    /* 每列占多少 Byte。 */
    uint32_t bytes_per_column;

    /* 当前 Pixel 所在 Byte 相对 font->data 的偏移。 */
    uint32_t byte_offset;

    /* 读出的字模 Byte。 */
    uint8_t value;

    /* 当前 glyph_y 对应的 bit mask。 */
    uint8_t mask;

    /* 例如 'A' - ' ' 得到 'A' 是第几个字形。 */
    glyph_index =
        (uint32_t)ch - (uint32_t)font->first_char;

    /* 每一列纵向需要 ceil(height / 8) Byte。 */
    bytes_per_column =
        ((uint32_t)font->height + 7UL) / 8UL;

    /*
     * 字模是“逐列式”：
     *      glyph_index * 每字节数        -> 找到当前字符；
     *      glyph_x * 每列字节数          -> 找到当前列；
     *      glyph_y / 8                    -> 找到列内第几个 Byte。
     */
    byte_offset =
        glyph_index * (uint32_t)font->bytes_per_glyph +
        (uint32_t)glyph_x * bytes_per_column +
        (uint32_t)(glyph_y / 8U);

    /* 从字库中取出对应 Byte。 */
    value = font->data[byte_offset];

    /* bit7 对应纵向小段最上方 Pixel，所以向右移动 glyph_y % 8。 */
    mask = (uint8_t)(0x80U >> (glyph_y & 0x07U));

    /* 当前 bit 非 0 就返回 1，否则返回 0。 */
    return ((value & mask) != 0U) ? 1U : 0U;
}


/* ============================================================================
 * 12. 私有工具：uint32_t -> 十进制字符串
 * ============================================================================
 */
static void prvUInt32ToString(
    uint32_t value,
    char *buffer,
    uint32_t buffer_size
)
{
    /* uint32_t 最大 10 位十进制数字，所以 10 Byte 足够保存反向数字。 */
    char reverse[10];

    /* reverse 中当前已经保存多少位。 */
    uint32_t count = 0UL;

    /* 输出 buffer 当前写到哪个下标。 */
    uint32_t out = 0UL;

    /* 输出指针为空或容量为 0 时没有任何合法写法。 */
    if ((buffer == NULL) || (buffer_size == 0UL))
    {
        /* void 函数直接返回。 */
        return;
    }

    /* 0 需要单独处理，因为普通 while(value != 0) 不会进入循环。 */
    if (value == 0UL)
    {
        /* 至少需要 '0' + '\0' 两个 Byte 才能完整输出。 */
        if (buffer_size >= 2UL)
        {
            /* 第一个字符写数字 0。 */
            buffer[0] = '0';

            /* 第二个字符写 C 字符串结束符。 */
            buffer[1] = '\0';
        }
        else
        {
            /* 容量只有 1 时只能输出空字符串，保证 NUL 终止。 */
            buffer[0] = '\0';
        }

        /* 0 已经处理完毕。 */
        return;
    }

    /* 逐位取 value 的个位，因此首先得到的是反向数字。 */
    while ((value != 0UL) && (count < 10UL))
    {
        /* value % 10 得到当前个位，并转换成 ASCII '0'~'9'。 */
        reverse[count] =
            (char)('0' + (char)(value % 10UL));

        /* 去掉已经处理的个位。 */
        value /= 10UL;

        /* 反向缓存长度增加 1。 */
        ++count;
    }

    /* 从 reverse 尾部向前复制，恢复正常十进制顺序。 */
    while ((count > 0UL) && ((out + 1UL) < buffer_size))
    {
        /* 先减 count，使它指向当前尚未复制的最高位。 */
        --count;

        /* 复制一个正常顺序数字到输出。 */
        buffer[out] = reverse[count];

        /* 输出下标前进。 */
        ++out;
    }

    /* 无论是否因 buffer_size 截断，都保证以 '\0' 结束。 */
    buffer[out] = '\0';
}


/* ============================================================================
 * 13. Draw Pixel
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawPixel(
    int32_t x,
    int32_t y,
    uint16_t color
)
{
    /* 提升到 int64_t 后统一走安全的可见性检查。 */
    return prvDrawPixelClipped64((int64_t)x, (int64_t)y, color);
}


/* ============================================================================
 * 14. Fill Screen
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_FillScreen(uint16_t color)
{
    /* Core 已经有经过验证的全屏 Clear 路径，GFX 直接复用。 */
    return ILI9341_Clear(color);
}


/* ============================================================================
 * 15. Fill Rectangle
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_FillRect(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    uint16_t color
)
{
    /* 保存裁剪后的可见宽度。 */
    uint16_t clipped_width;

    /* 保存裁剪后的可见高度。 */
    uint16_t clipped_height;

    /* 可见区域总 Pixel 数。 */
    uint32_t pixel_count;

    /* 保存 Core API 返回状态。 */
    ILI9341_Status_t status;

    /* 宽或高为 0 属于调用参数错误，而不是“完全屏幕外”。 */
    if ((width == 0U) || (height == 0U))
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 初始可见宽度先等于原始宽度。 */
    clipped_width = width;

    /* 初始可见高度先等于原始高度。 */
    clipped_height = height;

    /* 对矩形做屏幕边界裁剪，同时 x/y 会被改为可见左上角。 */
    if (prvClipRect(
            &x,
            &y,
            &clipped_width,
            &clipped_height
        ) == 0U)
    {
        /* 完全在屏幕外属于正常图形裁剪结果。 */
        return ILI9341_OK;
    }

    /* 可见矩形 Pixel 数 = 可见宽 * 可见高。 */
    pixel_count =
        (uint32_t)clipped_width * (uint32_t)clipped_height;

    /* 打开一个恰好覆盖可见矩形的 RAMWR Transaction。 */
    status =
        ILI9341_BeginWrite(
            (uint16_t)x,
            (uint16_t)y,
            clipped_width,
            clipped_height
        );

    /* BeginWrite 失败时当前函数没有建立成功事务，可直接返回。 */
    if (status != ILI9341_OK)
    {
        /* 原样向上传递 Core 错误。 */
        return status;
    }

    /* 在当前 Window 中重复写同一种 RGB565 颜色。 */
    status =
        ILI9341_WriteColorRepeatBlocking(
            color,
            pixel_count
        );

    /* 数据阶段失败时必须 Abort，不能把 CS 留在 Low。 */
    if (status != ILI9341_OK)
    {
        /* Abort 只用于收尾；这里保留最初的数据写错误作为最终返回值。 */
        (void)ILI9341_AbortWrite();

        /* 返回真正导致 FillRect 失败的错误。 */
        return status;
    }

    /* 数据全部写完后由 EndWrite 正常释放 CS 并恢复 READY。 */
    return ILI9341_EndWrite();
}


/* ============================================================================
 * 16. Fast Horizontal Line
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawFastHLine(
    int32_t x,
    int32_t y,
    uint16_t length,
    uint16_t color
)
{
    /* 0 长度没有明确几何意义，按参数错误处理。 */
    if (length == 0U)
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 水平线就是“高度为 1 的填充矩形”。 */
    return ILI9341_GFX_FillRect(
        x,
        y,
        length,
        1U,
        color
    );
}


/* ============================================================================
 * 17. Fast Vertical Line
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawFastVLine(
    int32_t x,
    int32_t y,
    uint16_t length,
    uint16_t color
)
{
    /* 0 长度按参数错误处理。 */
    if (length == 0U)
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 垂直线就是“宽度为 1 的填充矩形”。 */
    return ILI9341_GFX_FillRect(
        x,
        y,
        1U,
        length,
        color
    );
}


/* ============================================================================
 * 18. Draw Line
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawLine(
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    uint16_t color
)
{
    /* Bresenham 中 X 方向绝对差。 */
    int32_t dx;

    /* Bresenham 中使用负数形式保存的 Y 方向差。 */
    int32_t dy;

    /* X 每次前进一步的方向：+1 或 -1。 */
    int32_t sx;

    /* Y 每次前进一步的方向：+1 或 -1。 */
    int32_t sy;

    /* Bresenham 累积误差。 */
    int32_t err;

    /* 误差的 2 倍，用于判断本轮是否推进 X/Y。 */
    int32_t e2;

    /* Core/GFX 调用状态。 */
    ILI9341_Status_t status;

    /*
     * 先把任意 int32_t 端点裁到 240x320 / 320x240 的屏幕区域。
     * 裁剪后坐标范围很小，因此后面的 Bresenham int32_t 运算绝对安全。
     */
    if (prvClipLine(&x0, &y0, &x1, &y1) == 0U)
    {
        /* 整条线不可见属于正常情况。 */
        return ILI9341_OK;
    }

    /* 水平线不必逐 Pixel 建事务，直接走 FillRect 快速路径。 */
    if (y0 == y1)
    {
        /* 取两个端点中较小的 X 作为左端。 */
        int32_t left = (x0 < x1) ? x0 : x1;

        /* 两端都已裁进屏幕，跨度最多 320，可安全转成 uint16_t。 */
        uint32_t span =
            (uint32_t)((x0 < x1) ? (x1 - x0) : (x0 - x1)) + 1UL;

        /* 一次 Transaction 绘制完整水平线。 */
        return ILI9341_GFX_DrawFastHLine(
            left,
            y0,
            (uint16_t)span,
            color
        );
    }

    /* 垂直线同理走快速路径。 */
    if (x0 == x1)
    {
        /* 取较小 Y 作为上端。 */
        int32_t top = (y0 < y1) ? y0 : y1;

        /* 计算包含两端点的垂直跨度。 */
        uint32_t span =
            (uint32_t)((y0 < y1) ? (y1 - y0) : (y0 - y1)) + 1UL;

        /* 一次 Transaction 绘制完整垂直线。 */
        return ILI9341_GFX_DrawFastVLine(
            x0,
            top,
            (uint16_t)span,
            color
        );
    }

    /* 裁剪后 x0/x1 均在屏幕内，所以差值不会溢出 int32_t。 */
    dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);

    /* Bresenham 经典写法让 dy 保存为非正值。 */
    dy = (y1 >= y0) ? (y0 - y1) : (y1 - y0);

    /* x0 需要向右移动时 sx=+1，否则 sx=-1。 */
    sx = (x0 < x1) ? 1 : -1;

    /* y0 需要向下移动时 sy=+1，否则 sy=-1。 */
    sy = (y0 < y1) ? 1 : -1;

    /* 初始误差 = dx + dy。 */
    err = dx + dy;

    /* 循环直到当前点走到终点。 */
    for (;;)
    {
        /* 裁剪后每个 Bresenham 点都位于屏幕内，可直接走 Core WritePixel。 */
        status = ILI9341_WritePixel(
            (uint16_t)x0,
            (uint16_t)y0,
            color
        );

        /* 任意 Pixel 写失败都立即停止整条线。 */
        if (status != ILI9341_OK)
        {
            /* 原样向上传递错误。 */
            return status;
        }

        /* 当前点已经等于终点时整条线绘制完成。 */
        if ((x0 == x1) && (y0 == y1))
        {
            /* 退出无限循环。 */
            break;
        }

        /* 保存 2 * err，避免后面重复计算。 */
        e2 = err * 2;

        /* 满足条件时沿 X 方向前进一步。 */
        if (e2 >= dy)
        {
            /* X 前进一步后修正误差。 */
            err += dy;

            /* 根据 sx 选择向左或向右。 */
            x0 += sx;
        }

        /* 满足条件时沿 Y 方向前进一步。 */
        if (e2 <= dx)
        {
            /* Y 前进一步后修正误差。 */
            err += dx;

            /* 根据 sy 选择向上或向下。 */
            y0 += sy;
        }
    }

    /* 线段所有 Pixel 均绘制成功。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 19. Draw Rectangle
 * ============================================================================
 *
 * 注意：
 *      空心矩形不能先把“整个矩形”裁成屏幕交集后再画四条边，
 *      否则会把裁剪边错误地当成原始矩形边。
 *
 * 例如原矩形左边在 x=-10，真正的左边框应该不可见；
 * 如果先把矩形裁到 x=0 再画，就会错误地在屏幕 x=0 补一条竖线。
 *
 * 因此这里保留“原始四条边”，每条边独立裁剪。
 */
ILI9341_Status_t ILI9341_GFX_DrawRect(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    uint16_t color
)
{
    /* 原矩形左边 X，提升到 int64_t。 */
    int64_t left;

    /* 原矩形右边 X，闭区间坐标。 */
    int64_t right;

    /* 原矩形上边 Y。 */
    int64_t top;

    /* 原矩形下边 Y，闭区间坐标。 */
    int64_t bottom;

    /* 保存每条边的绘制结果。 */
    ILI9341_Status_t status;

    /* 0 宽或 0 高是无效矩形参数。 */
    if ((width == 0U) || (height == 0U))
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 保存原左边界，不做提前裁剪。 */
    left = (int64_t)x;

    /* 右边界 = x + width - 1，int64_t 防止 int32_t 溢出。 */
    right = left + (int64_t)width - 1;

    /* 保存原上边界。 */
    top = (int64_t)y;

    /* 下边界 = y + height - 1。 */
    bottom = top + (int64_t)height - 1;

    /* 高度为 1 时矩形退化成一条水平线，只画一次。 */
    if (height == 1U)
    {
        /* 按原左右端点独立裁剪这条水平边。 */
        return prvDrawHSpan64(left, right, top, color);
    }

    /* 宽度为 1 时矩形退化成一条垂直线，只画一次。 */
    if (width == 1U)
    {
        /* 按原上下端点独立裁剪。 */
        return prvDrawVSpan64(left, top, bottom, color);
    }

    /* 先画原始上边。 */
    status = prvDrawHSpan64(left, right, top, color);

    /* 上边失败时停止后续绘制。 */
    if (status != ILI9341_OK)
    {
        /* 返回实际错误。 */
        return status;
    }

    /* 再画原始下边。 */
    status = prvDrawHSpan64(left, right, bottom, color);

    /* 下边失败时停止。 */
    if (status != ILI9341_OK)
    {
        /* 返回实际错误。 */
        return status;
    }

    /*
     * 竖边去掉上下角点，避免重复绘制四个角；
     * height > 2 时中间至少还有 1 Pixel。
     */
    if (height > 2U)
    {
        /* 绘制原始左边框的中间部分。 */
        status = prvDrawVSpan64(
            left,
            top + 1,
            bottom - 1,
            color
        );

        /* 左边失败时立即返回。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 绘制原始右边框的中间部分。 */
        status = prvDrawVSpan64(
            right,
            top + 1,
            bottom - 1,
            color
        );

        /* 右边失败时立即返回。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }
    }

    /* 四条原始边都已处理完毕。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 20. Draw Circle
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawCircle(
    int32_t x0,
    int32_t y0,
    uint16_t radius,
    uint16_t color
)
{
    /* 中点圆算法当前 X 偏移。 */
    int32_t x;

    /* 中点圆算法当前 Y 偏移。 */
    int32_t y;

    /* 中点圆决策变量。 */
    int32_t decision;

    /* 保存每个对称 Pixel 的绘制状态。 */
    ILI9341_Status_t status;

    /* 半径为 0 时圆退化为圆心一个 Pixel。 */
    if (radius == 0U)
    {
        /* 使用 int64_t 裁剪版本，兼容极端圆心。 */
        return prvDrawPixelClipped64((int64_t)x0, (int64_t)y0, color);
    }

    /*
     * 先用圆的包围盒做快速不可见判断，避免一个完全在屏幕外的超大圆
     * 仍执行成千上万次中点圆循环。
     */
    if ((((int64_t)x0 + (int64_t)radius) < 0) ||
        (((int64_t)y0 + (int64_t)radius) < 0) ||
        (((int64_t)x0 - (int64_t)radius) >= (int64_t)ILI9341_GetWidth()) ||
        (((int64_t)y0 - (int64_t)radius) >= (int64_t)ILI9341_GetHeight()))
    {
        /* 包围盒都不相交，圆周一定不可见。 */
        return ILI9341_OK;
    }

    /* 中点圆从 (0, radius) 开始。 */
    x = 0;

    /* radius 最大 65535，可安全放入 int32_t。 */
    y = (int32_t)radius;

    /* 标准中点圆初始决策变量。 */
    decision = 1 - (int32_t)radius;

    /* 只计算第一八分之一圆周，再利用 8 对称性扩展。 */
    while (x <= y)
    {
        /* 对称点 1：(+x, +y)。 */
        status = prvDrawPixelClipped64(
            (int64_t)x0 + (int64_t)x,
            (int64_t)y0 + (int64_t)y,
            color
        );

        /* 任一 Core 写失败都立即停止。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 对称点 2：(-x, +y)。 */
        status = prvDrawPixelClipped64(
            (int64_t)x0 - (int64_t)x,
            (int64_t)y0 + (int64_t)y,
            color
        );

        /* 检查结果。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 对称点 3：(+x, -y)。 */
        status = prvDrawPixelClipped64(
            (int64_t)x0 + (int64_t)x,
            (int64_t)y0 - (int64_t)y,
            color
        );

        /* 检查结果。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 对称点 4：(-x, -y)。 */
        status = prvDrawPixelClipped64(
            (int64_t)x0 - (int64_t)x,
            (int64_t)y0 - (int64_t)y,
            color
        );

        /* 检查结果。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 对称点 5：(+y, +x)。 */
        status = prvDrawPixelClipped64(
            (int64_t)x0 + (int64_t)y,
            (int64_t)y0 + (int64_t)x,
            color
        );

        /* 检查结果。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 对称点 6：(-y, +x)。 */
        status = prvDrawPixelClipped64(
            (int64_t)x0 - (int64_t)y,
            (int64_t)y0 + (int64_t)x,
            color
        );

        /* 检查结果。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 对称点 7：(+y, -x)。 */
        status = prvDrawPixelClipped64(
            (int64_t)x0 + (int64_t)y,
            (int64_t)y0 - (int64_t)x,
            color
        );

        /* 检查结果。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 对称点 8：(-y, -x)。 */
        status = prvDrawPixelClipped64(
            (int64_t)x0 - (int64_t)y,
            (int64_t)y0 - (int64_t)x,
            color
        );

        /* 检查结果。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* X 向外推进一个 Pixel。 */
        ++x;

        /* decision < 0 表示中点仍在圆内部，只需要推进 X。 */
        if (decision < 0)
        {
            /* 标准中点圆误差更新公式。 */
            decision += 2 * x + 1;
        }
        else
        {
            /* 中点越出圆时 Y 同时向内收缩一个 Pixel。 */
            --y;

            /* 同时推进 X/Y 后的误差更新。 */
            decision += 2 * (x - y) + 1;
        }
    }

    /* 整个圆周绘制完成。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 21. Fill Circle
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_FillCircle(
    int32_t x0,
    int32_t y0,
    uint16_t radius,
    uint16_t color
)
{
    /* 中点圆当前 X 偏移。 */
    int32_t x;

    /* 中点圆当前 Y 偏移。 */
    int32_t y;

    /* 中点圆决策变量。 */
    int32_t decision;

    /* 保存每条水平 Span 的返回状态。 */
    ILI9341_Status_t status;

    /* 半径为 0 时实心圆退化为一个 Pixel。 */
    if (radius == 0U)
    {
        /* 安全绘制圆心。 */
        return prvDrawPixelClipped64((int64_t)x0, (int64_t)y0, color);
    }

    /* 包围盒完全不与屏幕相交时直接返回。 */
    if ((((int64_t)x0 + (int64_t)radius) < 0) ||
        (((int64_t)y0 + (int64_t)radius) < 0) ||
        (((int64_t)x0 - (int64_t)radius) >= (int64_t)ILI9341_GetWidth()) ||
        (((int64_t)y0 - (int64_t)radius) >= (int64_t)ILI9341_GetHeight()))
    {
        /* 完全不可见。 */
        return ILI9341_OK;
    }

    /* 中点圆从 X 偏移 0 开始。 */
    x = 0;

    /* 初始 Y 偏移等于半径。 */
    y = (int32_t)radius;

    /* 初始化中点圆决策变量。 */
    decision = 1 - (int32_t)radius;

    /* 扫描第一八分之一，并利用对称性画四组水平线。 */
    while (x <= y)
    {
        /* 在 y0 + y 这一行画 [x0-x, x0+x]。 */
        status = prvDrawHSpan64(
            (int64_t)x0 - (int64_t)x,
            (int64_t)x0 + (int64_t)x,
            (int64_t)y0 + (int64_t)y,
            color
        );

        /* 任何 Core 错误都终止填充。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* y != 0 时再画关于圆心 X 轴对称的下方线，避免中心线重复。 */
        if (y != 0)
        {
            /* 绘制 y0 - y 对称扫描线。 */
            status = prvDrawHSpan64(
                (int64_t)x0 - (int64_t)x,
                (int64_t)x0 + (int64_t)x,
                (int64_t)y0 - (int64_t)y,
                color
            );

            /* 检查绘制结果。 */
            if (status != ILI9341_OK)
            {
                /* 返回错误。 */
                return status;
            }
        }

        /* x != y 时还有另一组交换 x/y 后的扫描线。 */
        if (x != y)
        {
            /* 在 y0 + x 行画 [x0-y, x0+y]。 */
            status = prvDrawHSpan64(
                (int64_t)x0 - (int64_t)y,
                (int64_t)x0 + (int64_t)y,
                (int64_t)y0 + (int64_t)x,
                color
            );

            /* 检查结果。 */
            if (status != ILI9341_OK)
            {
                /* 返回错误。 */
                return status;
            }

            /* x != 0 时 y0-x 与 y0+x 不同，再画另一条对称线。 */
            if (x != 0)
            {
                /* 绘制 y0 - x 扫描线。 */
                status = prvDrawHSpan64(
                    (int64_t)x0 - (int64_t)y,
                    (int64_t)x0 + (int64_t)y,
                    (int64_t)y0 - (int64_t)x,
                    color
                );

                /* 检查结果。 */
                if (status != ILI9341_OK)
                {
                    /* 返回错误。 */
                    return status;
                }
            }
        }

        /* X 向外推进一个 Pixel。 */
        ++x;

        /* 根据中点位置决定 Y 是否收缩。 */
        if (decision < 0)
        {
            /* 只推进 X 的误差更新。 */
            decision += 2 * x + 1;
        }
        else
        {
            /* Y 向内收缩。 */
            --y;

            /* X/Y 同时改变后的误差更新。 */
            decision += 2 * (x - y) + 1;
        }
    }

    /* 整个圆内部扫描线填充完成。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 22. Draw RGB565 Image（连续行）
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawImageRGB565(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    const uint16_t *pixels
)
{
    /*
     * 连续图片的每行跨度正好等于 width，
     * 所以直接转给更通用的 Stride 版本，避免两套图片裁剪逻辑。
     */
    return ILI9341_GFX_DrawImageRGB565Stride(
        x,
        y,
        width,
        height,
        width,
        pixels
    );
}


/* ============================================================================
 * 23. Draw RGB565 Image with Stride
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawImageRGB565Stride(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    uint16_t stride_pixels,
    const uint16_t *pixels
)
{
    /* 目标图片裁剪后的左上角 X。 */
    int32_t clipped_x;

    /* 目标图片裁剪后的左上角 Y。 */
    int32_t clipped_y;

    /* 目标图片裁剪后的可见宽度。 */
    uint16_t clipped_width;

    /* 目标图片裁剪后的可见高度。 */
    uint16_t clipped_height;

    /* 可见区域相对源图片左侧跳过多少 Pixel。 */
    uint32_t source_x;

    /* 可见区域相对源图片顶部跳过多少行。 */
    uint32_t source_y;

    /* 逐行发送时的当前可见行下标。 */
    uint32_t row;

    /* 当前可见行在源图中的首 Pixel 指针。 */
    const uint16_t *source;

    /* Core/GFX 调用状态。 */
    ILI9341_Status_t status;

    /*
     * pixels 必须有效；图片宽高必须非 0；
     * stride 小于 width 会导致下一行在当前行尚未结束时就开始，属于非法布局。
     */
    if ((pixels == NULL) ||
        (width == 0U) ||
        (height == 0U) ||
        (stride_pixels < width))
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 初始裁剪 X 使用用户目标 X。 */
    clipped_x = x;

    /* 初始裁剪 Y 使用用户目标 Y。 */
    clipped_y = y;

    /* 初始可见宽度等于源图宽度。 */
    clipped_width = width;

    /* 初始可见高度等于源图高度。 */
    clipped_height = height;

    /* 对目标矩形做屏幕边界裁剪。 */
    if (prvClipRect(
            &clipped_x,
            &clipped_y,
            &clipped_width,
            &clipped_height
        ) == 0U)
    {
        /* 图片完全在屏幕外时不读取源图数据。 */
        return ILI9341_OK;
    }

    /*
     * clipped_x/y 已经落在屏幕内；只有原 x/y 位于其左/上方时才有裁剪偏移。
     * 使用 int64_t 做差，彻底避免极端 int32_t 减法溢出。
     */
    source_x = (uint32_t)((int64_t)clipped_x - (int64_t)x);

    /* 计算顶部被裁掉多少源图行。 */
    source_y = (uint32_t)((int64_t)clipped_y - (int64_t)y);

    /*
     * 最快路径：
     *      图片没有任何裁剪；
     *      每行紧密连续；
     * 此时整个图片只建立一次 RAMWR Transaction。
     */
    if ((source_x == 0UL) &&
        (source_y == 0UL) &&
        (clipped_width == width) &&
        (clipped_height == height) &&
        (stride_pixels == width))
    {
        /* 为整个图片建立一个完整 Window。 */
        status =
            ILI9341_BeginWrite(
                (uint16_t)clipped_x,
                (uint16_t)clipped_y,
                clipped_width,
                clipped_height
            );

        /* BeginWrite 失败时直接返回。 */
        if (status != ILI9341_OK)
        {
            /* 返回 Core 错误。 */
            return status;
        }

        /* 一次把全部 uint16_t RGB565 Pixel 交给 Core 转换并发送。 */
        status =
            ILI9341_WritePixelsBlocking(
                pixels,
                (uint32_t)width * (uint32_t)height
            );

        /* 数据阶段失败时必须 Abort 当前 Transaction。 */
        if (status != ILI9341_OK)
        {
            /* 尝试清理当前 RAMWR。 */
            (void)ILI9341_AbortWrite();

            /* 返回原始写数据错误。 */
            return status;
        }

        /* 正常结束完整图片 Transaction。 */
        return ILI9341_EndWrite();
    }

    /*
     * 慢路径：
     *      发生裁剪，或者源图每行有额外 stride。
     * 每一行在源内存中都是一段连续 uint16_t，所以逐可见行发送，
     * 不需要额外申请整幅图片临时 Buffer。
     */
    for (row = 0UL; row < (uint32_t)clipped_height; ++row)
    {
        /*
         * 找到当前可见行在源图中的第一个可见 Pixel：
         *      源行 = source_y + row；
         *      每行跨度 = stride_pixels；
         *      再跳过左侧 source_x Pixel。
         */
        source =
            pixels +
            (source_y + row) * (uint32_t)stride_pixels +
            source_x;

        /* 每一行建立一个“高为 1”的 Window。 */
        status =
            ILI9341_BeginWrite(
                (uint16_t)clipped_x,
                (uint16_t)(clipped_y + (int32_t)row),
                clipped_width,
                1U
            );

        /* BeginWrite 失败时这一行尚未开始，可直接返回。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }

        /* 发送这一行可见的 RGB565 Pixel。 */
        status =
            ILI9341_WritePixelsBlocking(
                source,
                (uint32_t)clipped_width
            );

        /* 数据发送失败时 Abort 当前这一行事务。 */
        if (status != ILI9341_OK)
        {
            /* 收尾当前 Transaction。 */
            (void)ILI9341_AbortWrite();

            /* 返回原始错误。 */
            return status;
        }

        /* 当前行数据完成后正常结束事务。 */
        status = ILI9341_EndWrite();

        /* EndWrite 自己失败时不能继续下一行。 */
        if (status != ILI9341_OK)
        {
            /* 返回 EndWrite 状态。 */
            return status;
        }
    }

    /* 所有可见行均绘制完成。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 24. Draw Mono Bitmap/Icon  1-bit Mono Bitmap（单色位图）（图标 字模 汉字点阵 ascii字体常用这种格式）
 * 本函数是 MSB First，即最高有效位优先。  
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawMonoBitmap(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t *bitmap,
    uint16_t stride_bytes,
    uint16_t foreground,   //前景色
    uint16_t background,   //背景色
    ILI9341_GFX_DrawMode_t mode
)
{
    /* 裁剪后的目标左上角 X。 */
    int32_t clipped_x;

    /* 裁剪后的目标左上角 Y。 */
    int32_t clipped_y;

    /* 裁剪后的可见宽度。 */
    uint16_t clipped_width;

    /* 裁剪后的可见高度。 */
    uint16_t clipped_height;

    /* 可见区域在源位图中的起始列。 */
    uint32_t source_x;

    /* 可见区域在源位图中的起始行。 */
    uint32_t source_y;

    /* 当前可见行。 */
    uint16_t row;

    /* 当前可见列。 */
    uint16_t column;

    /* 当前连续前景 Run 在可见行中的起始列。 */
    uint16_t run_start;

    /* 当前连续前景 Run 长度。 */
    uint16_t run_length;

    /* 每一源行至少需要多少 Byte 才能容纳 width 个 bit。 */
    uint16_t minimum_stride;

    /* 当前 Pixel 对应的源行。 */
    uint32_t source_row;

    /* 当前 Pixel 对应的源列。 */
    uint32_t source_column;

    /* 当前 Pixel 所在源 Byte 下标。 */
    uint32_t byte_index;

    /* 当前 Pixel 所在 bit 的 mask。 */
    uint8_t mask;

    /* 当前 bit 是否为 1。 */
    uint8_t bit_on;

    /* 前一个可见 Pixel bit 状态，用于识别连续 Run。 */
    uint8_t previous_state;

    /* 保存 GFX/Core 返回状态。 */
    ILI9341_Status_t status;

    /* bitmap 指针、宽、高必须有效。 */
    if ((bitmap == NULL) ||
        (width == 0U) ||
        (height == 0U))
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* mode 只允许透明或不透明两种枚举值。 */
    if ((mode != ILI9341_GFX_MODE_TRANSPARENT) &&
        (mode != ILI9341_GFX_MODE_OPAQUE))
    {
        /* 非法枚举值属于参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* ceil(width / 8)，得到按行 1-bit 图最少 Byte 数。 */
    minimum_stride =
        (uint16_t)(((uint32_t)width + 7UL) / 8UL);

    /* 用户 stride 太小会造成行数据互相覆盖，拒绝。 */
    if (stride_bytes < minimum_stride)
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 初始裁剪坐标等于目标左上角。 */
    clipped_x = x;

    /* 初始裁剪 Y。 */
    clipped_y = y;

    /* 初始可见宽度等于原位图宽度。 */
    clipped_width = width;

    /* 初始可见高度等于原位图高度。 */
    clipped_height = height;

    /*
     * 先裁剪整个目标矩形。
     * 这是 V2 的重要修复：避免完全屏幕外时仍逐 Pixel 扫描，
     * 也避免后续 x + run_start / y + row 发生 int32_t 极端溢出。
     */
    if (prvClipRect(
            &clipped_x,
            &clipped_y,
            &clipped_width,
            &clipped_height
        ) == 0U)
    {
        /* 完全不可见时不读取 bitmap。 */
        return ILI9341_OK;
    }

    /* 左侧被裁掉多少列。 */
    source_x =
        (uint32_t)((int64_t)clipped_x - (int64_t)x);

    /* 顶部被裁掉多少行。 */
    source_y =
        (uint32_t)((int64_t)clipped_y - (int64_t)y);

    /*
     * 不透明模式先把“可见部分”一次填成背景色。
     * 屏幕外背景无需写入，因此不需要填原始整个 width x height。
     */
    if (mode == ILI9341_GFX_MODE_OPAQUE)
    {
        /* 一次背景 FillRect 比逐个 bit 写 background 高效得多。 */
        status = ILI9341_GFX_FillRect(
            clipped_x,
            clipped_y,
            clipped_width,
            clipped_height,
            background
        );

        /* 背景填充失败时不能继续画前景。 */
        if (status != ILI9341_OK)
        {
            /* 返回实际错误。 */
            return status;
        }
    }

    /* 只扫描屏幕上真正可见的源位图行。 */
    for (row = 0U; row < clipped_height; ++row)
    {
        /* 当前源行 = 顶部裁剪偏移 + 当前可见行。 */
        source_row = source_y + (uint32_t)row;

        /* 新一行尚未出现前景 Run。 */
        run_start = 0U;

        /* 当前 Run 长度清零。 */
        run_length = 0U;

        /* 进入新一行前，前一个 bit 视为 0。 */
        previous_state = 0U;

        /* 只扫描当前行真正可见的列。 */
        for (column = 0U; column < clipped_width; ++column)
        {
            /* 当前源列 = 左侧裁剪偏移 + 当前可见列。 */
            source_column = source_x + (uint32_t)column;

            /* 找到当前源 bit 所在 Byte。 */
            byte_index =
                source_row * (uint32_t)stride_bytes +
                source_column / 8UL;

            /* MSB first：源列模 8 决定从 bit7 向右移动多少位。 */
            mask =
                (uint8_t)(0x80U >> (source_column & 0x07UL));

            /* 读取当前 bit。 */
            bit_on =
                ((bitmap[byte_index] & mask) != 0U) ? 1U : 0U;

            /* 0 -> 1：发现一个新的连续前景 Run。 */
            if ((bit_on != 0U) && (previous_state == 0U))
            {
                /* 记录 Run 在“可见区域”里的起始列。 */
                run_start = column;

                /* 当前已经包含 1 个前景 Pixel。 */
                run_length = 1U;
            }
            /* 1 -> 1：仍处在同一个连续 Run 中。 */
            else if ((bit_on != 0U) && (previous_state != 0U))
            {
                /* Run 长度增加 1。 */
                ++run_length;
            }
            /* 1 -> 0：前一个 Run 在当前列之前结束，需要一次画出去。 */
            else if ((bit_on == 0U) && (previous_state != 0U))
            {
                /* 用一条 HLine 一次绘制整个前景 Run。 */
                status = ILI9341_GFX_DrawFastHLine(
                    clipped_x + (int32_t)run_start,
                    clipped_y + (int32_t)row,
                    run_length,
                    foreground
                );

                /* HLine 失败时停止。 */
                if (status != ILI9341_OK)
                {
                    /* 返回 Core/GFX 错误。 */
                    return status;
                }

                /* 当前 Run 已经画完，长度清零。 */
                run_length = 0U;
            }

            /* 当前 bit 成为下一轮的 previous_state。 */
            previous_state = bit_on;
        }

        /* 行结束时如果仍处于前景 Run，说明 Run 一直延伸到了最右可见列。 */
        if (previous_state != 0U)
        {
            /* 把行尾 Run 画出去。 */
            status = ILI9341_GFX_DrawFastHLine(
                clipped_x + (int32_t)run_start,
                clipped_y + (int32_t)row,
                run_length,
                foreground
            );

            /* 检查行尾 Run 结果。 */
            if (status != ILI9341_OK)
            {
                /* 返回错误。 */
                return status;
            }
        }
    }

    /* 所有可见源 bit 已处理完成。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 25. Draw ASCII Character
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawChar(
    int32_t x,
    int32_t y,
    char ch,
    const ILI9341_GFX_Font_t *font,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
)
{
    /* 最终实际使用的字形编码。 */
    uint8_t glyph_char;

    /* 裁剪后的目标 X。 */
    int32_t clipped_x;

    /* 裁剪后的目标 Y。 */
    int32_t clipped_y;

    /* 裁剪后的可见字符宽度。 */
    uint16_t clipped_width;

    /* 裁剪后的可见字符高度。 */
    uint16_t clipped_height;

    /* 可见区域对应字形内部的起始 X。 */
    uint32_t source_x;

    /* 可见区域对应字形内部的起始 Y。 */
    uint32_t source_y;

    /* 当前可见行。 */
    uint16_t row;

    /* 当前可见列。 */
    uint16_t column;

    /* 当前前景 Run 起点。 */
    uint16_t run_start;

    /* 当前前景 Run 长度。 */
    uint16_t run_length;

    /* 当前字形内部真实 X。 */
    uint16_t glyph_x;

    /* 当前字形内部真实 Y。 */
    uint16_t glyph_y;

    /* 当前字模 bit。 */
    uint8_t bit_on;

    /* 上一个 bit 状态。 */
    uint8_t previous_state;

    /* 保存 GFX/Core 状态。 */
    ILI9341_Status_t status;

    /* 先验证字体结构，避免后面越界解释 data。 */
    if (prvFontIsValid(font) == 0U)
    {
        /* 字体描述错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* mode 必须是两个合法枚举之一。 */
    if ((mode != ILI9341_GFX_MODE_TRANSPARENT) &&
        (mode != ILI9341_GFX_MODE_OPAQUE))
    {
        /* 非法模式。 */
        return ILI9341_ERROR_PARAM;
    }

    /* char 可能是 signed char，先按原始 8-bit 模式转换为 uint8_t。 */
    glyph_char = (uint8_t)ch;

    /* 如果字符不在当前字体范围内。 */
    if ((glyph_char < font->first_char) ||
        (glyph_char > font->last_char))
    {
        /* 如果字体内包含 '?'，使用问号作为 fallback = 回退字符。 */
        if (((uint8_t)'?' >= font->first_char) &&
            ((uint8_t)'?' <= font->last_char))
        {
            /* 把实际绘制字符改成 '?'。 */
            glyph_char = (uint8_t)'?';
        }
        else
        {
            /* 连问号都没有时无法合理替代。 */
            return ILI9341_ERROR_PARAM;
        }
    }

    /* 初始裁剪目标 X。 */
    clipped_x = x;

    /* 初始裁剪目标 Y。 */
    clipped_y = y;

    /* 初始宽度就是完整字体宽度。 */
    clipped_width = font->width;

    /* 初始高度就是完整字体高度。 */
    clipped_height = font->height;

    /*
     * 先裁剪整个字形矩形：
     *      - 完全不可见时无需读取任何字模数据；
     *      - 部分可见时只扫描可见字形区域；
     *      - 避免 x + run_start / y + row 极端 int32_t 溢出。
     */
    if (prvClipRect(
            &clipped_x,
            &clipped_y,
            &clipped_width,
            &clipped_height
        ) == 0U)
    {
        /* 字符完全不可见属于正常裁剪。 */
        return ILI9341_OK;
    }

    /* 字符左侧被裁掉多少 Pixel。 */
    source_x =
        (uint32_t)((int64_t)clipped_x - (int64_t)x);

    /* 字符顶部被裁掉多少 Pixel。 */
    source_y =
        (uint32_t)((int64_t)clipped_y - (int64_t)y);

    /* 不透明模式先把可见字符矩形一次性刷成背景色。 */
    if (mode == ILI9341_GFX_MODE_OPAQUE)
    {
        /* 只需要填屏幕可见区域。 */
        status = ILI9341_GFX_FillRect(
            clipped_x,
            clipped_y,
            clipped_width,
            clipped_height,
            background
        );

        /* 背景失败时停止。 */
        if (status != ILI9341_OK)
        {
            /* 返回错误。 */
            return status;
        }
    }

    /* 按“可见行”扫描字形。 */
    for (row = 0U; row < clipped_height; ++row)
    {
        /* 真实字形 Y = 被裁掉的顶部 + 当前可见行。 */
        glyph_y = (uint16_t)(source_y + (uint32_t)row);

        /* 新一行 Run 起点初始化。 */
        run_start = 0U;

        /* 新一行 Run 长度清零。 */
        run_length = 0U;

        /* 前一个 bit 初始视为 0。 */
        previous_state = 0U;

        /* 扫描当前行的可见列。 */
        for (column = 0U; column < clipped_width; ++column)
        {
            /* 真实字形 X = 左侧裁剪偏移 + 当前可见列。 */
            glyph_x = (uint16_t)(source_x + (uint32_t)column);

            /* 从列式字库读取当前 Pixel bit。 */
            bit_on = prvFontPixelOn(
                font,
                glyph_char,
                glyph_x,
                glyph_y
            );

            /* 0 -> 1：新前景 Run 开始。 */
            if ((bit_on != 0U) && (previous_state == 0U))
            {
                /* 记录可见区域内的 Run 起点。 */
                run_start = column;

                /* Run 已包含当前 1 Pixel。 */
                run_length = 1U;
            }
            /* 1 -> 1：Run 继续。 */
            else if ((bit_on != 0U) && (previous_state != 0U))
            {
                /* Run 长度增加。 */
                ++run_length;
            }
            /* 1 -> 0：Run 在当前列之前结束。 */
            else if ((bit_on == 0U) && (previous_state != 0U))
            {
                /* 用一次 HLine 绘制整个连续前景段。 */
                status = ILI9341_GFX_DrawFastHLine(
                    clipped_x + (int32_t)run_start,
                    clipped_y + (int32_t)row,
                    run_length,
                    foreground
                );

                /* 绘制失败时停止。 */
                if (status != ILI9341_OK)
                {
                    /* 返回错误。 */
                    return status;
                }

                /* 当前 Run 已结束，清零等待下一段。 */
                run_length = 0U;
            }

            /* 更新 previous_state。 */
            previous_state = bit_on;
        }

        /* 行尾仍为 1 时，最后一个 Run 需要补画。 */
        if (previous_state != 0U)
        {
            /* 绘制行尾连续前景段。 */
            status = ILI9341_GFX_DrawFastHLine(
                clipped_x + (int32_t)run_start,
                clipped_y + (int32_t)row,
                run_length,
                foreground
            );

            /* 检查行尾绘制结果。 */
            if (status != ILI9341_OK)
            {
                /* 返回错误。 */
                return status;
            }
        }
    }

    /* 字符所有可见前景 Pixel 已绘制。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 26. Draw ASCII String
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawString(
    int32_t x,
    int32_t y,
    const char *text,
    const ILI9341_GFX_Font_t *font,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
)
{
    /*
     * 字符串游标用 int64_t，而不是 int32_t：
     * 长字符串不断 cursor_x += font->width 时不会产生 signed int32_t UB。
     */
    int64_t cursor_x;

    /* 当前文本行的 Y 游标，同样用 int64_t 防累加溢出。 */
    int64_t cursor_y;

    /* '\n' 后需要恢复到的初始 X。 */
    int64_t origin_x;

    /* 保存单字符绘制状态。 */
    ILI9341_Status_t status;

    /* text 必须有效，同时字体必须通过结构检查。 */
    if ((text == NULL) || (prvFontIsValid(font) == 0U))
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /*
     * V1 对空字符串不会进入 DrawChar，因此非法 mode 可能被漏过；
     * V2 在入口统一验证，保证 DrawString 的参数契约一致。
     */
    if ((mode != ILI9341_GFX_MODE_TRANSPARENT) &&
        (mode != ILI9341_GFX_MODE_OPAQUE))
    {
        /* 返回参数错误。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 保存换行时恢复的原始 X。 */
    origin_x = (int64_t)x;

    /* 初始化当前 X 游标。 */
    cursor_x = (int64_t)x;

    /* 初始化当前 Y 游标。 */
    cursor_y = (int64_t)y;

    /* C 字符串直到遇到 '\0' 才结束。 */
    while (*text != '\0')
    {
        /* '\r' = Carriage Return，这里按接口约定直接忽略。 */
        if (*text == '\r')
        {
            /* 指针移到下一个字符。 */
            ++text;

            /* 不绘制当前 '\r'，直接继续循环。 */
            continue;
        }

        /* '\n' = New Line，换到下一文本行。 */
        if (*text == '\n')
        {
            /* X 恢复到字符串最初起点。 */
            cursor_x = origin_x;

            /* Y 向下增加一个完整字体高度。 */
            cursor_y += (int64_t)font->height;

            /* 消费当前 '\n'。 */
            ++text;

            /* 换行本身不绘制字形。 */
            continue;
        }

        /*
         * DrawChar 公共接口要求 int32_t 坐标。
         * 如果 int64_t 游标已经超出 int32_t 范围，该字符必然远离屏幕，
         * 因为字符串 X/Y 只会单调增加，不需要危险地强转回 int32_t。
         */
        if ((cursor_x >= (int64_t)INT32_MIN) &&
            (cursor_x <= (int64_t)INT32_MAX) &&
            (cursor_y >= (int64_t)INT32_MIN) &&
            (cursor_y <= (int64_t)INT32_MAX))
        {
            /* 绘制当前字符；DrawChar 自己负责屏幕边界裁剪。 */
            status = ILI9341_GFX_DrawChar(
                (int32_t)cursor_x,
                (int32_t)cursor_y,
                *text,
                font,
                foreground,
                background,
                mode
            );

            /* 任意字符绘制失败都停止字符串。 */
            if (status != ILI9341_OK)
            {
                /* 返回实际错误。 */
                return status;
            }
        }

        /* 当前字符结束后 X 前进一个固定字体宽度。 */
        cursor_x += (int64_t)font->width;

        /* 字符串指针移动到下一个字符。 */
        ++text;
    }

    /* 整个字符串已处理到 NUL 结束符。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 27. Draw uint32_t
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawUInt32(
    int32_t x,
    int32_t y,
    uint32_t value,
    const ILI9341_GFX_Font_t *font,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
)
{
    /* uint32_t 最多 10 位数字，再加一个 '\0'，所以需要 11 Byte。 */
    char buffer[11];

    /* 把整数转换成标准 NUL 结尾十进制字符串。 */
    prvUInt32ToString(
        value,
        buffer,
        (uint32_t)sizeof(buffer)
    );

    /* 复用字符串绘制，不重复实现数字字形逻辑。 */
    return ILI9341_GFX_DrawString(
        x,
        y,
        buffer,
        font,
        foreground,
        background,
        mode
    );
}


/* ============================================================================
 * 28. Draw int32_t
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawInt32(
    int32_t x,
    int32_t y,
    int32_t value,
    const ILI9341_GFX_Font_t *font,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
)
{
    /* 最长 "-2147483648" 为 11 个字符，再加 '\0'，共 12 Byte。 */
    char buffer[12];

    /* 保存绝对值部分的无符号十进制字符串。 */
    char magnitude_text[11];

    /* value 的绝对值幅度，使用 uint32_t 可容纳 2147483648。 */
    uint32_t magnitude;

    /* magnitude_text 读取下标。 */
    uint32_t index;

    /* buffer 输出下标。 */
    uint32_t out;

    /* 非负 int32_t 可以直接交给无符号版本。 */
    if (value >= 0)
    {
        /* 复用 DrawUInt32。 */
        return ILI9341_GFX_DrawUInt32(
            x,
            y,
            (uint32_t)value,
            font,
            foreground,
            background,
            mode
        );
    }

    /*
     * 不能直接写 magnitude = (uint32_t)(-value)：
     * value == INT32_MIN(-2147483648) 时，+2147483648 不能由 int32_t 表示，
     * 直接 -value 会产生 signed overflow。
     *
     * -(value + 1) 一定仍在 int32_t 正数范围内，再 +1UL 恢复正确幅度。
     */
    magnitude =
        (uint32_t)(-(value + 1)) + 1UL;

    /* 把正的绝对值幅度转换成十进制文本。 */
    prvUInt32ToString(
        magnitude,
        magnitude_text,
        (uint32_t)sizeof(magnitude_text)
    );

    /* 负数输出第一个字符固定为减号。 */
    buffer[0] = '-';

    /* 下一个输出位置从 1 开始。 */
    out = 1UL;

    /* 从 magnitude_text 第 0 个数字开始复制。 */
    index = 0UL;

    /*
     * 复制绝对值文本，同时永远给最终 '\0' 留一个位置。
     */
    while ((magnitude_text[index] != '\0') &&
           (out < ((uint32_t)sizeof(buffer) - 1UL)))
    {
        /* 复制一个十进制数字。 */
        buffer[out] = magnitude_text[index];

        /* 输出下标前进。 */
        ++out;

        /* 输入下标前进。 */
        ++index;
    }

    /* 形成合法 C 字符串。 */
    buffer[out] = '\0';

    /* 最后复用统一字符串绘制。 */
    return ILI9341_GFX_DrawString(
        x,
        y,
        buffer,
        font,
        foreground,
        background,
        mode
    );
}


/* ============================================================================
 * 29. Draw RGB565 Wire-Byte Image（紧密行）
 * ============================================================================
 *
 * Wire = “线上传输格式”。
 *
 * 为什么必须额外提供这一组函数：
 * ---------------------------------------------------------------------------
 * STM32F407 是 Little Endian（小端）CPU。
 * 普通 uint16_t 红色 0xF800 在 RAM 中通常排列为：
 *
 *      00 F8
 *
 * 但是 ILI9341 当前 8-bit SPI 发送 RGB565 时需要：
 *
 *      F8 00
 *
 * 原工程 Picture.c 里的 gImage_LCD280 已经直接保存成第二种顺序，也就是
 * “可以直接发送到 LCD 的 Byte Stream（字节流）”。
 *
 * 因此绝对不能把它简单强制转换成 uint16_t * 再走 DrawImageRGB565()，
 * 否则 Core 会把已经正确的字节顺序再次转换，最终颜色会错误。
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawImageRGB565Bytes(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t *wire_bytes
)
{
    /*
     * 一行紧密排列的 RGB565 图片，每个 Pixel（像素）固定占 2 Byte。
     *
     * 这里转成 uint32_t 再乘 2，避免 width 在 uint16_t 中先发生窄类型计算。
     */
    uint32_t stride_bytes = (uint32_t)width * 2UL;

    /*
     * 实际裁剪和发送逻辑统一交给 Stride（行跨度）版本。
     * 这样只维护一套复杂逻辑，减少重复代码和未来两个版本行为不一致的风险。
     */
    return ILI9341_GFX_DrawImageRGB565BytesStride(
        x,
        y,
        width,
        height,
        stride_bytes,
        wire_bytes
    );
}


/* ============================================================================
 * 30. Draw RGB565 Wire-Byte Image with Stride
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawImageRGB565BytesStride(
    int32_t x,
    int32_t y,
    uint16_t width,
    uint16_t height,
    uint32_t stride_bytes,
    const uint8_t *wire_bytes
)
{
    /* 裁剪后的目标左上角 X。 */
    int32_t clipped_x;

    /* 裁剪后的目标左上角 Y。 */
    int32_t clipped_y;

    /* 裁剪后真正需要显示的宽度。 */
    uint16_t clipped_width;

    /* 裁剪后真正需要显示的高度。 */
    uint16_t clipped_height;

    /* 左侧被裁掉多少 Pixel。 */
    uint32_t source_x;

    /* 顶部被裁掉多少源图行。 */
    uint32_t source_y;

    /* 当前正在发送第几条“可见行”。 */
    uint32_t row;

    /* 当前行源数据起始地址。 */
    const uint8_t *source;

    /* 每一行至少需要多少 Byte 才能容纳 width 个 RGB565 Pixel。 */
    uint32_t minimum_stride;

    /* 当前可见行需要发送多少 Byte。 */
    uint32_t visible_row_bytes;

    /* 完整紧密图片总 Byte 数，使用 64 bit 避免中间乘法溢出。 */
    uint64_t total_bytes_64;

    /* 当前源行起点的 64-bit Byte Offset（字节偏移）。 */
    uint64_t source_offset_64;

    /* 保存 Core/GFX 返回状态。 */
    ILI9341_Status_t status;

    /* 原始数据指针必须存在，图片宽高也必须大于 0。 */
    if ((wire_bytes == NULL) ||
        (width == 0U) ||
        (height == 0U))
    {
        /* 无法形成有效图片。 */
        return ILI9341_ERROR_PARAM;
    }

    /* RGB565 每个 Pixel 固定两个 Byte。 */
    minimum_stride = (uint32_t)width * 2UL;

    /*
     * stride_bytes = 一条源图行在内存里实际跨过的 Byte 数。
     * 如果它比 width*2 还小，那么下一行会在当前行像素尚未结束前就开始，布局非法。
     */
    if (stride_bytes < minimum_stride)
    {
        /* 拒绝越界/重叠的源图布局。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 初始目标区域就是用户请求的目标 X。 */
    clipped_x = x;

    /* 初始目标区域就是用户请求的目标 Y。 */
    clipped_y = y;

    /* 未裁剪前，可见宽度等于完整图片宽度。 */
    clipped_width = width;

    /* 未裁剪前，可见高度等于完整图片高度。 */
    clipped_height = height;

    /*
     * GFX 先负责屏幕裁剪。
     * Core 只接收已经合法的 uint16_t 屏幕坐标，这样职责边界保持清晰。
     */
    if (prvClipRect(
            &clipped_x,
            &clipped_y,
            &clipped_width,
            &clipped_height
        ) == 0U)
    {
        /* 图片完全在屏幕外属于正常情况，不访问源数据，也不访问 Core。 */
        return ILI9341_OK;
    }

    /* 计算屏幕左侧裁掉了多少源图 Pixel。 */
    source_x =
        (uint32_t)((int64_t)clipped_x - (int64_t)x);

    /* 计算屏幕顶部裁掉了多少源图行。 */
    source_y =
        (uint32_t)((int64_t)clipped_y - (int64_t)y);

    /* 当前每条可见行需要发送 clipped_width * 2 Byte。 */
    visible_row_bytes = (uint32_t)clipped_width * 2UL;

    /* 使用 64 bit 得到完整紧密图片理论总 Byte。 */
    total_bytes_64 =
        (uint64_t)width *
        (uint64_t)height *
        2ULL;

    /*
     * Fast Path（快速路径）：
     *      1) 图片完全没有被裁剪；
     *      2) 每一行紧密连续，没有 Padding（填充字节）；
     *      3) 总 Byte 数能由 Core 的 uint32_t length 表示。
     *
     * 满足这些条件时，只建立一次 RAMWR Transaction，效率最高。
     */
    if ((source_x == 0UL) &&
        (source_y == 0UL) &&
        (clipped_width == width) &&
        (clipped_height == height) &&
        (stride_bytes == minimum_stride) &&
        (total_bytes_64 <= 0xFFFFFFFFULL))
    {
        /* 为整幅图片一次性建立 LCD Address Window（地址窗口）。 */
        status = ILI9341_BeginWrite(
            (uint16_t)clipped_x,
            (uint16_t)clipped_y,
            clipped_width,
            clipped_height
        );

        /* BeginWrite 失败说明事务根本没有成功建立。 */
        if (status != ILI9341_OK)
        {
            /* 原样返回 Core 状态。 */
            return status;
        }

        /*
         * 关键：这里调用 WriteBytesBlocking，而不是 WritePixelsBlocking。
         * 因为 wire_bytes 已经是 F8 00 这种 LCD 正确线上顺序，不需要再换字节。
         */
        status = ILI9341_WriteBytesBlocking(
            wire_bytes,
            (uint32_t)total_bytes_64
        );

        /* 数据发送阶段失败时，必须主动放弃当前 RAMWR Transaction。 */
        if (status != ILI9341_OK)
        {
            /* 尝试释放 CS、清理当前写会话。 */
            (void)ILI9341_AbortWrite();

            /* 保留最初的数据发送错误。 */
            return status;
        }

        /* 全部图片 Byte 已经写完，正常结束 Transaction。 */
        return ILI9341_EndWrite();
    }

    /*
     * Slow Path（逐行路径）：
     * 图片发生裁剪，或者源图存在额外行 Padding 时，逐可见行发送。
     *
     * 优点：不需要为裁剪后的图片再申请一块巨大临时 Buffer。
     */
    for (row = 0UL; row < (uint32_t)clipped_height; ++row)
    {
        /*
         * 源行起点：
         *      (source_y + row) * stride_bytes
         * 再加：
         *      source_x * 2
         * 得到当前行第一个可见 Pixel 的 High Byte 地址。
         */
        source_offset_64 =
            ((uint64_t)source_y + (uint64_t)row) *
            (uint64_t)stride_bytes +
            (uint64_t)source_x * 2ULL;

        /*
         * STM32F407 是 32-bit MCU，实际地址空间不可能超过 4 GiB。
         * 这个检查也避免 64-bit 计算结果在强制转换为指针偏移时被截断。
         */
        if (source_offset_64 > 0xFFFFFFFFULL)
        {
            /* 这种源图布局对当前 MCU 不可表示。 */
            return ILI9341_ERROR_PARAM;
        }

        /* 得到当前可见源行的实际 Byte 指针。 */
        source = wire_bytes + (uint32_t)source_offset_64;

        /* 为当前一行建立一个高度为 1 的 Address Window。 */
        status = ILI9341_BeginWrite(
            (uint16_t)clipped_x,
            (uint16_t)(clipped_y + (int32_t)row),
            clipped_width,
            1U
        );

        /* 当前行事务没有建立成功时，直接停止。 */
        if (status != ILI9341_OK)
        {
            /* 返回真实 Core 错误。 */
            return status;
        }

        /* 当前行已经是 Wire Bytes，所以直接原样发送。 */
        status = ILI9341_WriteBytesBlocking(
            source,
            visible_row_bytes
        );

        /* 当前行数据发送失败。 */
        if (status != ILI9341_OK)
        {
            /* 放弃当前行事务，避免 CS/状态遗留。 */
            (void)ILI9341_AbortWrite();

            /* 返回真正失败原因。 */
            return status;
        }

        /* 当前一行全部发送成功，正常关闭这条 Transaction。 */
        status = ILI9341_EndWrite();

        /* 如果收尾失败，不能继续下一行。 */
        if (status != ILI9341_OK)
        {
            /* 返回收尾错误。 */
            return status;
        }
    }

    /* 所有可见行都已经正确绘制。 */
    return ILI9341_OK;
}


/* ============================================================================
 * 31. Draw Generic Image Descriptor（通用图片描述对象）
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawImage(
    int32_t x,
    int32_t y,
    const ILI9341_GFX_Image_t *image
)
{
    /* Native U16 图片换算后的每行 Pixel 数。 */
    uint32_t stride_pixels_32;

    /* image 自己就是图片格式契约，所以指针必须存在。 */
    if (image == NULL)
    {
        /* NULL Descriptor（空描述对象）无法绘制。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 图片数据地址、宽度、高度都必须有效。 */
    if ((image->data == NULL) ||
        (image->width == 0U) ||
        (image->height == 0U))
    {
        /* 描述对象内容不完整。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 根据 Format（格式）选择真正的数据通路。 */
    if (image->format == ILI9341_GFX_IMAGE_FORMAT_RGB565_NATIVE_U16)
    {
        /*
         * uint16_t Pixel 每个占 2 Byte，所以一行 Byte 数必须是偶数。
         * 奇数 stride 无法表示“完整的 uint16_t Pixel 行”。
         */
        if ((image->stride_bytes & 1UL) != 0UL)
        {
            /* 非法 Native-U16 行布局。 */
            return ILI9341_ERROR_PARAM;
        }

        /* 把 Byte Stride 换算成 Pixel Stride。 */
        stride_pixels_32 = image->stride_bytes / 2UL;

        /*
         * 旧公开 API 的 stride_pixels 使用 uint16_t。
         * 对 STM32 LCD 图片来说这个范围已经远大于实际需求，超出则拒绝。
         */
        if (stride_pixels_32 > 0xFFFFUL)
        {
            /* 当前接口无法表达如此大的源行跨度。 */
            return ILI9341_ERROR_PARAM;
        }

        /*
         * uint16_t 读取要求至少 2-Byte 对齐。
         * STM32F407 对非对齐半字访问虽然部分场景可工作，但不应该让资源层依赖它。
         */
        if ((((uintptr_t)image->data) & 1UL) != 0UL)
        {
            /* 数据地址不满足 uint16_t 对齐要求。 */
            return ILI9341_ERROR_PARAM;
        }

        /* 进入已有的 Native-U16 图片绘制通路。 */
        return ILI9341_GFX_DrawImageRGB565Stride(
            x,
            y,
            image->width,
            image->height,
            (uint16_t)stride_pixels_32,
            (const uint16_t *)image->data
        );
    }

    /* 第二种合法格式：已经准备好的 LCD Wire Bytes。 */
    if (image->format == ILI9341_GFX_IMAGE_FORMAT_RGB565_WIRE_BYTES)
    {
        /* 进入不会再次换字节的 Wire-Byte 通路。 */
        return ILI9341_GFX_DrawImageRGB565BytesStride(
            x,
            y,
            image->width,
            image->height,
            image->stride_bytes,
            (const uint8_t *)image->data
        );
    }

    /* 未知格式意味着 Descriptor 配置错误。 */
    return ILI9341_ERROR_PARAM;
}


/* ============================================================================
 * 32. Draw Generic Mono Bitmap（支持 Row / Column Major）
 * ============================================================================
 */
ILI9341_Status_t ILI9341_GFX_DrawMonoBitmapEx(
    int32_t x,
    int32_t y,
    const ILI9341_GFX_MonoBitmap_t *bitmap,
    uint16_t foreground,
    uint16_t background,
    ILI9341_GFX_DrawMode_t mode
)
{
    /* 裁剪后的目标左上角 X。 */
    int32_t clipped_x;

    /* 裁剪后的目标左上角 Y。 */
    int32_t clipped_y;

    /* 裁剪后可见宽度。 */
    uint16_t clipped_width;

    /* 裁剪后可见高度。 */
    uint16_t clipped_height;

    /* 左侧裁掉的源位图列数。 */
    uint32_t source_x;

    /* 顶部裁掉的源位图行数。 */
    uint32_t source_y;

    /* 当前正在扫描的可见行。 */
    uint16_t row;

    /* 当前正在扫描的可见列。 */
    uint16_t column;

    /* 当前前景连续 Run（连续段）的起始列。 */
    uint16_t run_start;

    /* 当前前景 Run 的长度。 */
    uint16_t run_length;

    /* Column-Major 模式下，每一列最少需要多少 Byte。 */
    uint16_t minimum_column_stride;

    /* 当前 Pixel 在原始资源中的源行。 */
    uint32_t source_row;

    /* 当前 Pixel 在原始资源中的源列。 */
    uint32_t source_column;

    /* 当前 Pixel 对应到 bitmap->data 中的 Byte 下标。 */
    uint32_t byte_index;

    /* 当前 Pixel 对应 Byte 内的 bit mask（位掩码）。 */
    uint8_t mask;

    /* 当前 Pixel 的 bit 是否为 1。 */
    uint8_t bit_on;

    /* 前一个 Pixel 的 bit，用于把连续 1 合并成 HLine。 */
    uint8_t previous_state;

    /* GFX/Core 调用状态。 */
    ILI9341_Status_t status;

    /* 描述对象本身必须存在。 */
    if (bitmap == NULL)
    {
        /* 无法读取任何位图参数。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 数据、宽、高都必须有效。 */
    if ((bitmap->data == NULL) ||
        (bitmap->width == 0U) ||
        (bitmap->height == 0U))
    {
        /* 不完整的 Bitmap Descriptor。 */
        return ILI9341_ERROR_PARAM;
    }

    /* mode 只允许 Transparent（透明）和 Opaque（不透明）。 */
    if ((mode != ILI9341_GFX_MODE_TRANSPARENT) &&
        (mode != ILI9341_GFX_MODE_OPAQUE))
    {
        /* 非法枚举值。 */
        return ILI9341_ERROR_PARAM;
    }

    /*
     * Row Major 已经由原来的 DrawMonoBitmap 完整实现，直接复用。
     * 这里不复制第二套同样的行优先算法。
     */
    if (bitmap->layout == ILI9341_GFX_MONO_LAYOUT_ROW_MAJOR_MSB_FIRST)
    {
        /* 进入原有稳定的行优先 Bitmap 通路。 */
        return ILI9341_GFX_DrawMonoBitmap(
            x,
            y,
            bitmap->width,
            bitmap->height,
            bitmap->data,
            bitmap->stride_bytes,
            foreground,
            background,
            mode
        );
    }

    /* 除 Row Major 外，当前只支持 Column Major。 */
    if (bitmap->layout != ILI9341_GFX_MONO_LAYOUT_COLUMN_MAJOR_MSB_FIRST)
    {
        /* 未知 Layout（布局）拒绝处理。 */
        return ILI9341_ERROR_PARAM;
    }

    /*
     * Column Major 时“一列”从上到下保存。
     * 每一列至少需要 ceil(height / 8) Byte。
     */
    minimum_column_stride =
        (uint16_t)(((uint32_t)bitmap->height + 7UL) / 8UL);

    /* stride 太小会让相邻列的数据互相覆盖。 */
    if (bitmap->stride_bytes < minimum_column_stride)
    {
        /* 数据布局非法。 */
        return ILI9341_ERROR_PARAM;
    }

    /* 初始裁剪 X。 */
    clipped_x = x;

    /* 初始裁剪 Y。 */
    clipped_y = y;

    /* 初始可见宽度等于完整位图宽度。 */
    clipped_width = bitmap->width;

    /* 初始可见高度等于完整位图高度。 */
    clipped_height = bitmap->height;

    /* 先裁剪整个目标矩形，避免屏幕外无意义扫描。 */
    if (prvClipRect(
            &clipped_x,
            &clipped_y,
            &clipped_width,
            &clipped_height
        ) == 0U)
    {
        /* 完全不可见是正常结果。 */
        return ILI9341_OK;
    }

    /* 得到源图左侧被裁掉的列数。 */
    source_x =
        (uint32_t)((int64_t)clipped_x - (int64_t)x);

    /* 得到源图顶部被裁掉的行数。 */
    source_y =
        (uint32_t)((int64_t)clipped_y - (int64_t)y);

    /*
     * Opaque（不透明）模式先一次性把可见区域填成背景色。
     * 随后只需要画 bit=1 的前景 Run，比每个 0/1 Pixel 单独写屏快很多。
     */
    if (mode == ILI9341_GFX_MODE_OPAQUE)
    {
        /* 一次填满可见背景。 */
        status = ILI9341_GFX_FillRect(
            clipped_x,
            clipped_y,
            clipped_width,
            clipped_height,
            background
        );

        /* 背景写入失败就不能继续。 */
        if (status != ILI9341_OK)
        {
            /* 返回真实错误。 */
            return status;
        }
    }

    /* 一行一行扫描“最终屏幕上的可见行”。 */
    for (row = 0U; row < clipped_height; ++row)
    {
        /* 当前行对应原始资源中的哪一行。 */
        source_row = source_y + (uint32_t)row;

        /* 新的一行还没有找到前景 Run。 */
        run_start = 0U;

        /* Run 长度从 0 开始。 */
        run_length = 0U;

        /* 行首之前视为 bit=0。 */
        previous_state = 0U;

        /* 从左到右扫描这一条可见屏幕行。 */
        for (column = 0U; column < clipped_width; ++column)
        {
            /* 当前屏幕列对应原始资源中的源列。 */
            source_column = source_x + (uint32_t)column;

            /*
             * Column Major Byte 地址：
             *      第 source_column 列 * 每列 stride
             *      + source_row / 8 找到这一列中的第几个 Byte。
             */
            byte_index =
                source_column * (uint32_t)bitmap->stride_bytes +
                source_row / 8UL;

            /*
             * MSB First = 最高有效位在前。
             * row%8==0 对应 bit7；row%8==7 对应 bit0。
             */
            mask =
                (uint8_t)(0x80U >> (source_row & 0x07UL));

            /* 读取当前 Pixel 的 1-bit 状态。 */
            bit_on =
                ((bitmap->data[byte_index] & mask) != 0U) ? 1U : 0U;

            /* 0 -> 1：新的前景连续段开始。 */
            if ((bit_on != 0U) && (previous_state == 0U))
            {
                /* 记录这个 Run 在可见行里的开始位置。 */
                run_start = column;

                /* 当前这个 Pixel 已经构成长度 1。 */
                run_length = 1U;
            }
            /* 1 -> 1：还在同一个连续前景段内。 */
            else if ((bit_on != 0U) && (previous_state != 0U))
            {
                /* 连续段再增加一个 Pixel。 */
                ++run_length;
            }
            /* 1 -> 0：前景段刚刚结束，需要一次画出去。 */
            else if ((bit_on == 0U) && (previous_state != 0U))
            {
                /* 把多个相邻前景 Pixel 合并成一条 HLine，提高效率。 */
                status = ILI9341_GFX_DrawFastHLine(
                    clipped_x + (int32_t)run_start,
                    clipped_y + (int32_t)row,
                    run_length,
                    foreground
                );

                /* Core/GFX 写失败立即停止。 */
                if (status != ILI9341_OK)
                {
                    /* 返回真正错误。 */
                    return status;
                }

                /* 当前 Run 已完成，重新清零。 */
                run_length = 0U;
            }

            /* 当前 bit 成为下一轮判断中的 previous_state。 */
            previous_state = bit_on;
        }

        /* 如果一行结束时仍是 bit=1，说明最后一个 Run 一直延伸到最右边。 */
        if (previous_state != 0U)
        {
            /* 画出最后一个行尾 Run。 */
            status = ILI9341_GFX_DrawFastHLine(
                clipped_x + (int32_t)run_start,
                clipped_y + (int32_t)row,
                run_length,
                foreground
            );

            /* 行尾绘制失败同样停止。 */
            if (status != ILI9341_OK)
            {
                /* 返回真实错误。 */
                return status;
            }
        }
    }

    /* 所有可见 bit 已经完成绘制。 */
    return ILI9341_OK;
}
