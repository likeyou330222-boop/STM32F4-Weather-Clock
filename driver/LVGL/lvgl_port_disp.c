/* ============================================================================
 * 文件名称：lvgl_port_disp.c
 *
 * 项目平台：STM32F407 + FreeRTOS + LVGL v8.3.11 + ILI9341 (SPI + DMA)
 * 配套头文件：lvgl_port_disp.h
 *
 * 一、本文件解决什么问题？
 * ---------------------------------------------------------------------------
 * LVGL 可以在 RAM 中把界面画成 RGB565 像素，但它并不知道：
 *   - ILI9341 怎样设置写入窗口；
 *   - SPI DMA 怎样发送数据；
 *   - LCD 硬件由哪个 FreeRTOS 任务独占。
 *
 * 本文件是“桥梁”，把 LVGL 绘好的像素送给已经存在的 LCD Service Task。
 *
 * 二、正常刷新时的任务调用关系
 * ---------------------------------------------------------------------------
 * LVGL 所属任务（未来由 AppTask 统一持有 LVGL）
 *   |
 *   | lv_timer_handler() 触发 LVGL 绘制
 *   v
 * prvLVGLFlushCallback()                 [仍在 LVGL 所属任务中]
 *   |
 *   | 1. 检查刷新矩形
 *   | 2. RGB565 转成 ILI9341 线上字节序
 *   | 3. 通过 ILI9341_RTOS_Execute() 提交 Job
 *   v
 * LCD Service Task
 *   |
 *   | prvLCDFlushJob()                   [函数定义在本文件，执行在服务任务]
 *   v
 * ILI9341_RTOS_WriteWindowBytesDMA()
 *   |
 *   | BeginWrite -> DMAStart -> ISR 通知 -> DMAFinalize -> EndWrite
 *   v
 * LCD Service Task 通知 LVGL 所属任务：Job 执行完毕
 *   |
 *   v
 * ILI9341_RTOS_Execute() 返回
 *   |
 *   v
 * lv_disp_flush_ready()                  [仍在 LVGL 所属任务中]
 *
 * 注意：“函数定义在哪个 .c 文件”和“在哪个任务运行”不是一回事。
 *
 * 三、前置条件与约束
 * ---------------------------------------------------------------------------
 * 1. 调用 LVGL_PortDisp_Init() 前，必须完成 lv_init()。
 * 2. 必须先初始化原工程的 ILI9341_RTOS_Init()，LCD Service 正常运行。
 * 3. 由唯一的 LVGL 所属任务调用本模块的初始化，并调用 lv_timer_handler()。
 *    其他任务不能不加保护地并发操作 LVGL。
 * 4. 依赖现有 ili9341_freertos.c 的“同步等待 Job 完成”语义：
 *    - 提交失败：Job 未入队，立即返回；
 *    - 入队成功：一直等待 LCD Service 执行完成，才返回；
 *    - queue_wait_ticks 只限制提交阶段，不限制已入队 Job 的执行时间。
 *    这保证下文的局部 Job 结构体不会在服务任务使用时失效。
 * 5. LVGL 配置：LV_COLOR_DEPTH=16，LV_COLOR_16_SWAP=0。
 * 6. 该文件负责显示移植；LVGL Tick、周期任务、启动页和主页稍后接入。
 * 7. 暂不修改已验证的 ILI9341 Port/Core/GFX/DMA 中断源文件。
 *
 * 四、内存说明
 * ---------------------------------------------------------------------------
 * 一行：320 像素 × 2 Byte = 640 Byte。
 * 本文件使用 10 行局部刷新：320 × 10 × 2 = 6400 Byte。
 *   LVGL draw buffer    : 6400 Byte，存放 lv_color_t RGB565 像素。
 *   LCD DMA wire buffer : 6400 Byte，存放 SPI 线上要求的字节顺序。
 * 两块静态缓冲区合计 12800 Byte，不等于 LVGL 内部 32 KB 内存池。
 *
 * DMA 不能访问 STM32F407 的 CCM RAM（0x10000000 起始），所以在注册
 * 显示驱动之前，程序会检查 DMA 字节缓冲区位于 0x20000000 起始的 SRAM。
 * 真正的 RAM/Flash 使用量仍应结合 Keil MAP 和运行时堆栈水位评估。
 * ============================================================================
 */

/* 对外接口声明：uint8_t LVGL_PortDisp_Init(void)。 */
#include "lvgl_port_disp.h"

/* LVGL 类型、颜色转换、显示驱动注册和刷新完成 API。 */
#include "lvgl.h"

/* 已有 LCD Service API、ILI9341_Status_t、旋转枚举等。 */
#include "ili9341_freertos.h"

/* FreeRTOS：portMAX_DELAY、pdMS_TO_TICKS() 等。 */
#include "FreeRTOS.h"

/* FreeRTOS：xTaskGetSchedulerState() 等任务 API。 */
#include "task.h"

/* NULL、uintptr_t、uint8_t、uint16_t、uint32_t。 */
#include <stddef.h>
#include <stdint.h>


/* ============================================================================
 * 1. 显示参数（均为编译期常量）
 * ============================================================================
 */

/* 当前硬件已验证为 ILI9341 横屏，逻辑宽 320。 */
#define LVGL_PORT_HOR_RES          320U

/* 当前硬件横屏逻辑高 240。 */
#define LVGL_PORT_VER_RES          240U

/* 每次最多给 LVGL 绘制 10 行，分块逐次写屏，不需要整屏帧缓冲。 */
#define LVGL_PORT_DRAW_LINES       10U

/* 320 * 10 = 3200：注意这里单位是“像素”，不是字节。 */
#define LVGL_PORT_DRAW_PIXELS      (LVGL_PORT_HOR_RES * LVGL_PORT_DRAW_LINES)

/* RGB565 每像素 2 字节，因此 DMA 最大输入是 6400 字节。 */
#define LVGL_PORT_DMA_BYTES        (LVGL_PORT_DRAW_PIXELS * 2U)

/* 申请提交锁或等待 LCD Job 队列有空位时，最长等 1000 ms。 */
#define LVGL_PORT_QUEUE_WAIT_MS    1000U

/* 一次底层 DMA 分段等待 IRQ 的最长时间：1000 ms。 */
#define LVGL_PORT_DMA_WAIT_MS      1000U

/* STM32F407 常规 SRAM：128 KiB；根据旧工程 Keil IRAM 配置检查。 */
#define LVGL_PORT_SRAM_START       0x20000000UL
#define LVGL_PORT_SRAM_END         0x20020000UL /* 末尾地址 + 1，半开区间 */


/* ============================================================================
 * 2. 编译期约束：防止不小心改坏 lv_conf.h 而造成颜色错乱
 * ============================================================================
 */

#if (LV_COLOR_DEPTH != 16)
#error "lvgl_port_disp.c requires LV_COLOR_DEPTH == 16 (RGB565)"
#endif

#if (LV_COLOR_16_SWAP != 0)
#error "lvgl_port_disp.c requires LV_COLOR_16_SWAP == 0"
#endif

/* 编译器会据此检查 lv_color_t 真的是两个字节。
 * typedef 数组尺寸为 -1 时会报错，这是兼容 ARMCC5 的编译期校验技巧。 */
typedef char LVGL_Port_ColorMustBe2Bytes[(sizeof(lv_color_t) == 2U) ? 1 : -1];


/* ============================================================================
 * 3. 模块拥有的静态资源
 * ============================================================================
 */

/* 3.1 LVGL 像素缓冲区
 *
 * LVGL 软件渲染在这里生成每个像素；类型必须是 lv_color_t。
 * LV_COLOR_DEPTH=16 时，每个元素就是 16 bit RGB565。
 * 静态定义：对象在整个程序运行期内保持有效，不占用某次函数调用的栈。
 */
static lv_color_t s_draw_pixels[LVGL_PORT_DRAW_PIXELS];

/* 3.2 SPI DMA 字节缓冲区
 *
 * 这块内存和 LVGL 的 s_draw_pixels 不是同一个数组。
 * 转换后的字节按“高字节在前”存储，例如红色 0xF800 -> F8 00。
 * 使用 uint8_t 数组，是因为原 LCD DMA 接口需要 wire-order Byte Stream。
 *
 * 核心生命周期要求：在 DMA 彻底完成之前，这块内存绝不能被覆盖。
 * 本文件通过同步等待 ILI9341_RTOS_Execute() 返回来满足这个要求。
 */
static uint8_t s_dma_wire_bytes[LVGL_PORT_DMA_BYTES];

/* 3.3 LVGL draw-buffer 描述符。
 * 只负责描述内存位置、缓冲容量等，不是像素数组本身。 */
static lv_disp_draw_buf_t s_draw_buf_desc;

/* 3.4 LVGL 显示驱动描述符。
 * 里面包含水平/垂直分辨率、flush_cb 函数指针、draw_buf 等。 */
static lv_disp_drv_t s_disp_drv;

/* 3.5 注册成功后的 LVGL 显示设备对象。
 * NULL 表示还没有注册；用于避免重复向 LVGL 注册第二个屏幕。 */
static lv_disp_t *s_registered_display = NULL;

/* 3.6 调试变量（Keil Watch 可观察）。
 * s_last_flush_status：最近一次刷新最终返回的 ILI9341 状态码。
 * s_flush_error_count：刷新失败累计次数；数值增加表示发生了异常。
 * volatile：避免编译器把外部调试可能关注的状态缓存到寄存器。
 * 注意：volatile 不是互斥锁，也不保证跨任务读写的复杂操作原子性。 */
static volatile ILI9341_Status_t s_last_flush_status = ILI9341_OK;
static volatile uint32_t s_flush_error_count = 0UL;


/* ============================================================================
 * 4. 传给 LCD Service 的 Job 参数
 * ============================================================================
 *
 * ILI9341_RTOS_Execute() 的 function 参数是函数指针，user_context 是 void *。
 * 但一次 LCD 刷新要传 6 项参数，因此将它们统一封装在结构体里。
 *
 * 这里的“width/height”是窗口尺寸，不是右下角坐标。
 * data_length 必须严格等于 width * height * 2（RGB565）。
 */
typedef struct
{
    uint16_t x;               /* 窗口左上角 X 坐标，范围 0~319。 */
    uint16_t y;               /* 窗口左上角 Y 坐标，范围 0~239。 */
    uint16_t width;           /* 本次窗口宽度，像素数。 */
    uint16_t height;          /* 本次窗口高度，像素数。 */
    const uint8_t *data;      /* 已转换好的高字节在前 RGB565 数组。 */
    uint32_t data_length;     /* data 的字节长度。 */
} LVGL_PortFlushJob_t;


/* ============================================================================
 * 5. LCD Service 中运行的“设置横屏”Job
 * ============================================================================
 *
 * 由谁调用？
 *   LVGL_PortDisp_Init() 把本函数地址交给 ILI9341_RTOS_Execute()。
 * 在哪里执行？
 *   正常情况下，由 LCD Service Task 从队列取出并调用。
 * 什么时候完成？
 *   ILI9341_SetRotation() 返回且 320x240 尺寸检查通过后。
 *
 * 为什么不直接在 LVGL 任务调用 SetRotation()？
 *   LCD Service 是唯一的 LCD 硬件所有者。把显示控制也串行化，
 *   可以避免多个任务同时改寄存器或干扰正在进行的 LCD Transaction。
 */
static ILI9341_Status_t prvLCDSetLandscapeJob(void *user_context)
{
    ILI9341_Status_t status;

    /* Job 不需要额外参数；标记为刻意未使用，避免编译器 Warning。 */
    (void)user_context;

    /* 按旧工程定义设置普通横屏（不是反向横屏）。 */
    status = ILI9341_SetRotation(ILI9341_ROTATION_LANDSCAPE);

    /* 原始状态原样上报，不能把失败伪装成成功。 */
    if (status != ILI9341_OK)
    {
        return status;
    }

    /* 旋转成功还要核对 Core 的逻辑分辨率，防止界面坐标错位。 */
    if ((ILI9341_GetWidth() != LVGL_PORT_HOR_RES) ||
        (ILI9341_GetHeight() != LVGL_PORT_VER_RES))
    {
        return ILI9341_ERROR_STATE;
    }

    return ILI9341_OK;
}


/* ============================================================================
 * 6. LCD Service 中运行的“写一块像素”Job
 * ============================================================================
 *
 * 注意：虽然本函数写在 lvgl_port_disp.c 中，但它由 LCD Service Task 执行。
 *
 * user_context 的具体类型在 Execute() 接口中被抹成 void *，
 * 因此这里将它转换回 LVGL_PortFlushJob_t 指针。
 *
 * 内部直接调用 WriteWindowBytesDMA，而不是再提交一次异步任务。
 * 此时已经位于 LCD Service 上下文，底层会完成：
 *   BeginWrite -> DMA 分段发送 -> DMA 中断/任务通知 -> Finalize -> EndWrite。
 *
 * 这个函数返回时，底层的这一轮 DMA 传输应该已完成或已失败收尾。
 */
static ILI9341_Status_t prvLCDFlushJob(void *user_context)
{
    const LVGL_PortFlushJob_t *job;

    /* 把无类型指针恢复为本模块约定的 Job 参数结构体指针。 */
    job = (const LVGL_PortFlushJob_t *)user_context;

    /* 结构体指针、数据指针都必须有效，尺寸不允许为 0。 */
    if ((job == NULL) || (job->data == NULL) ||
        (job->width == 0U) || (job->height == 0U))
    {
        return ILI9341_ERROR_PARAM;
    }

    /* 再检查一次长度，避免不一致时 LCD 窗口传输损坏。 */
    if (job->data_length !=
        ((uint32_t)job->width * (uint32_t)job->height * 2UL))
    {
        return ILI9341_ERROR_PARAM;
    }

    /* 传输使用旧工程已经验证过的 ILI9341 FreeRTOS 包装函数。
     * portMAX_DELAY：LCD 访问互斥锁可以一直等到获得。
     * pdMS_TO_TICKS(1000)：每段 DMA 最多等待 1000 ms IRQ 通知。
     *
     * 注意：DMA 超时后的 Abort 恢复由原驱动负责，这里不重复操作硬件。 */
    return ILI9341_RTOS_WriteWindowBytesDMA(
        job->x,
        job->y,
        job->width,
        job->height,
        job->data,
        job->data_length,
        portMAX_DELAY,
        pdMS_TO_TICKS(LVGL_PORT_DMA_WAIT_MS)
    );
}


/* ============================================================================
 * 7. RGB565 颜色转换：LVGL 像素 -> ILI9341 wire-order 字节流
 * ============================================================================
 *
 * 为什么转换？
 *   STM32F407 为小端 CPU，例如 uint16_t 0xF800 在 RAM 中通常是 00 F8。
 *   但是 ILI9341 接收 RGB565 像素时要先收高 8 位，再收低 8 位：F8 00。
 *   因此不能直接将 LVGL 的 lv_color_t* 强制转换为 uint8_t* 发 DMA。
 *
 * src        : LVGL 绘好的 RGB565 像素首地址。
 * dst        : 本模块专用的 DMA 字节缓冲区首地址。
 * pixel_count: 本次要转换的像素数（已由调用者检查 <= 3200）。
 *
 * 本函数纯粹修改内存，不使用 FreeRTOS、SPI 或 DMA API。
 * 示例：
 *   RGB565 红：0xF800 => dst[0]=0xF8，dst[1]=0x00
 *   RGB565 绿：0x07E0 => dst[0]=0x07，dst[1]=0xE0
 *   RGB565 蓝：0x001F => dst[0]=0x00，dst[1]=0x1F
 */
static void prvConvertRGB565ToWire(const lv_color_t *src,
                                   uint8_t *dst,
                                   uint32_t pixel_count)
{
    uint32_t i;
    uint32_t byte_index;
    uint16_t rgb565;

    /* 依次处理每一个 16 bit 像素。 */
    for (i = 0UL; i < pixel_count; ++i)
    {
        /* LVGL 8.3.11：在 LV_COLOR_DEPTH=16 时得到当前 RGB565 数值。 */
        rgb565 = lv_color_to16(src[i]);

        /* 每个像素占两个字节，像素 i 的起点是 i*2。 */
        byte_index = i * 2UL;

        /* 先存高 8 位：右移 8 后截成 uint8_t。 */
        dst[byte_index] = (uint8_t)(rgb565 >> 8);

        /* 再存低 8 位：0x00FF 掩码只保留低字节。 */
        dst[byte_index + 1UL] = (uint8_t)(rgb565 & 0x00FFU);
    }
}


/* ============================================================================
 * 8. LVGL 刷新回调（flush_cb）——整个移植层的核心
 * ============================================================================
 *
 * 谁调用它？
 *   不是 AppTask 显式调用，而是 LVGL 绘图内部通过驱动描述符中的
 *   flush_cb 函数指针间接调用。在本项目约定下，它运行于 LVGL 所属任务。
 *
 * 三个参数：
 *   disp_drv : LVGL 显示驱动描述符（后面给 flush_ready 用）。
 *   area     : 要更新的矩形；x1/y1 是左上角，x2/y2 是右下角，坐标含端点。
 *   color_p  : 已经渲染好的 LVGL 像素数组，按区域逐行排列。
 *
 * 正常流程：
 *   检查坐标 -> 计算像素数 -> 检查容量 -> 转换字节序 ->
 *   提交 LCD Service Job -> 等传输处理结束 -> lv_disp_flush_ready()。
 *
 * 为什么要等？
 *   LVGL 只有收到 flush_ready 才能再次使用 s_draw_pixels。
 *   如果 DMA 还在读取上一次像素，这时又覆盖缓冲区，就会出现花屏。
 *
 * 失败怎么办？
 *   保存错误状态和累计次数，仍通知 LVGL 该次请求已处理完毕，
 *   防止渲染流程永久等待。注意：失败后并不代表屏幕数据正确，
 *   后续可在 AppTask 根据错误计数增加恢复或重绘策略。
 */
static void prvLVGLFlushCallback(lv_disp_drv_t *disp_drv,
                                 const lv_area_t *area,
                                 lv_color_t *color_p)
{
    int32_t width;
    int32_t height;
    uint32_t pixel_count;
    LVGL_PortFlushJob_t job;
    ILI9341_Status_t submit_status;
    ILI9341_Status_t job_status;

    /* 先假定本轮失败，成功后才改为 ILI9341_OK。
     * 这样早退分支不会遗漏错误记录。 */
    ILI9341_Status_t result = ILI9341_ERROR_PARAM;

    /* 防御性检查：LVGL 的 disp_drv 正常情况下永远不应为 NULL。
     * 如果真是 NULL，则无法安全执行 lv_disp_flush_ready(NULL)，
     * 因此只记录错误并返回。这个分支属于不可恢复的调用契约错误。 */
    if (disp_drv == NULL)
    {
        s_last_flush_status = ILI9341_ERROR_PARAM;
        ++s_flush_error_count;
        return;
    }

    /* 如果矩形或者像素地址无效，跳转到统一收尾。
     * 统一收尾的好处是任何普通失败分支都不会忘记 flush_ready。 */
    if ((area == NULL) || (color_p == NULL))
    {
        goto flush_finish;
    }

    /* LVGL 传入的区域必须完全在屏幕范围内。
     * 暂不做“超出屏幕后的裁剪”：裁剪还要同步调整像素首地址/行跨度，
     * 不能简单修改 x1、x2，否则会读取错误像素导致错位。 */
    if ((area->x1 < 0) ||
        (area->y1 < 0) ||
        (area->x2 < area->x1) ||
        (area->y2 < area->y1) ||
        (area->x2 >= (lv_coord_t)LVGL_PORT_HOR_RES) ||
        (area->y2 >= (lv_coord_t)LVGL_PORT_VER_RES))
    {
        goto flush_finish;
    }

    /* LVGL 的 area 使用闭区间。
     * 比如 X 从 0 到 319，宽度是 319 - 0 + 1 = 320。 */
    width = (int32_t)area->x2 - (int32_t)area->x1 + 1L;
    height = (int32_t)area->y2 - (int32_t)area->y1 + 1L;

    /* 此处 width <= 320，height <= 240，所以乘法不会溢出 uint32_t。 */
    pixel_count = (uint32_t)width * (uint32_t)height;

    /* 单次 LVGL flush 的像素数量不能超过实际 10 行缓冲容量。
     * LVGL partial 模式会把较大区域拆成多次 flush；这里仍必须做检查。 */
    if ((pixel_count == 0UL) || (pixel_count > LVGL_PORT_DRAW_PIXELS))
    {
        goto flush_finish;
    }

    /* LVGL 像素 --> DMA 要求的“高字节在前”RGB565 字节流。
     * 这一步完成前不会使用 SPI；因此不会产生硬件并发访问。 */
    prvConvertRGB565ToWire(color_p, s_dma_wire_bytes, pixel_count);

    /* 构造 LCD Job 参数：把一个刷新区域的所有信息放进结构体。
     * Job 是局部变量，位于 LVGL 所属任务栈；Execute 语义保证安全。 */
    job.x = (uint16_t)area->x1;
    job.y = (uint16_t)area->y1;
    job.width = (uint16_t)width;
    job.height = (uint16_t)height;
    job.data = s_dma_wire_bytes;
    job.data_length = pixel_count * 2UL;

    /* job_status 用于保存“LCD Job 自身”的执行结果；
     * submit_status 只表示“队列调度过程”的返回结果，两者含义不同。 */
    job_status = ILI9341_ERROR_STATE;

    /* 第一个参数 prvLCDFlushJob 是“函数地址”，不是当场执行它。
     * 第二个参数 &job 是待交给 LCD Service 的数据结构地址。
     * 第三个参数只限制申请提交锁/等待队列空位的时间。
     * 第四个参数让服务任务把实际执行状态回写出来。
     *
     * 当正常返回且 submit_status=OK 时，旧工程保证 Job 已真正执行完毕。
     * 注意：入队成功后本 API 可能无限等待，因此 LCD Service 必须能正常运行。 */
    submit_status = ILI9341_RTOS_Execute(
        prvLCDFlushJob,
        &job,
        pdMS_TO_TICKS(LVGL_PORT_QUEUE_WAIT_MS),
        &job_status
    );

    /* 如果调度阶段就失败，则不能使用尚未产生的 job_status；
     * 如果调度成功，则真正的 LCD 传输结果应以 job_status 为准。 */
    if (submit_status == ILI9341_OK)
    {
        result = job_status;
    }
    else
    {
        result = submit_status;
    }

flush_finish:
    /* 所有正常/参数错误路径统一到这里记录本轮状态。 */
    s_last_flush_status = result;

    /* 失败次数持续累加，用于排查花屏、断线或 DMA 超时。
     * 不在这里输出串口日志，避免在高频刷屏时影响实时性。 */
    if (result != ILI9341_OK)
    {
        ++s_flush_error_count;
    }

    /* 告诉 LVGL：这次 flush 的处理过程已经结束，可以复用绘图缓冲区。
     * 这是通知 LVGL 的 API，不是 DMA 启动函数，也不是硬件成功证明。
     *
     * 当前代码只有在 Job 已完成/失败收尾，或 Job 未能入队后才会到这里，
     * 以免 DMA 仍在使用 s_dma_wire_bytes 时被下一次转换覆盖。 */
    lv_disp_flush_ready(disp_drv);
}


/* ============================================================================
 * 9. 对外接口：注册 LVGL 显示设备
 * ============================================================================
 *
 * 调用关系（未来由 UI 所属任务在初始化时调用）：
 *   lv_init()
 *       -> ILI9341_RTOS_Init() [工程此前启动阶段完成]
 *       -> LVGL_PortDisp_Init()
 *           -> 验证 RTOS/LCD 已运行
 *           -> 提交 SetRotation Job，检查横屏 320x240
 *           -> lv_disp_draw_buf_init()
 *           -> lv_disp_drv_init()
 *           -> 填写 hor_res/ver_res/draw_buf/flush_cb
 *           -> lv_disp_drv_register()
 *
 * 返回值：1 代表注册成功，0 代表注册失败。
 * 注意：本函数不自行调用 lv_init()，不创建 FreeRTOS 任务，
 *       也不负责初始化 SPI、DMA 和 LCD GPIO。
 */
uint8_t LVGL_PortDisp_Init(void)
{
    ILI9341_Status_t submit_status;
    ILI9341_Status_t job_status;
    uintptr_t dma_address;

    /* 本模块已经注册成功，重复调用无需注册第二个相同显示设备。 */
    if (s_registered_display != NULL)
    {
        return 1U;
    }

    /* LCD Job 队列和任务通知需要运行中的 FreeRTOS 调度器。
     * 不能在 scheduler 启动前通过本接口进行同步 Job。 */
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
    {
        return 0U;
    }

    /* LCD Service 以及底层 ILI9341 必须已初始化成功。 */
    if (ILI9341_RTOS_IsInitialized() == 0U)
    {
        return 0U;
    }

    /* 普通 STM32F407 DMA 无法读取 CCM RAM。
     * uintptr_t 是能保存指针数值的无符号整数类型，用于比较地址。
     * 后面的边界检查不仅确保首地址合法，也确保整块 6400 Byte 不越界。
     * 该区间来自此前旧工程的 IRAM 配置；若将来更改内存映射，要重审。 */
    dma_address = (uintptr_t)&s_dma_wire_bytes[0];
    if ((dma_address < (uintptr_t)LVGL_PORT_SRAM_START) ||
        (dma_address > (uintptr_t)(LVGL_PORT_SRAM_END - LVGL_PORT_DMA_BYTES)))
    {
        return 0U;
    }

    /* 所有 LCD 旋转操作也由 LCD Service 独占执行。
     * 若旋转失败或尺寸不为 320x240，则拒绝注册 LVGL 显示驱动。 */
    job_status = ILI9341_ERROR_STATE;
    submit_status = ILI9341_RTOS_Execute(
        prvLCDSetLandscapeJob,
        NULL,
        pdMS_TO_TICKS(LVGL_PORT_QUEUE_WAIT_MS),
        &job_status
    );
    if ((submit_status != ILI9341_OK) || (job_status != ILI9341_OK))
    {
        return 0U;
    }

    /* 初始化 LVGL 的绘图缓冲“描述符”。
     * 参数 1：描述符地址；
     * 参数 2：第一块实际像素缓冲 s_draw_pixels；
     * 参数 3：第二块缓冲传 NULL，表示只使用单缓冲；
     * 参数 4：缓冲区的“像素个数”3200，不能写成“字节数”6400。
     *
     * 单缓冲 Partial 刷新：LVGL 每次绘制一小块，调用 flush_cb 送屏，
     * 送屏完成后再复用同一缓冲区绘下一块。 */
    lv_disp_draw_buf_init(
        &s_draw_buf_desc,
        s_draw_pixels,
        NULL,
        LVGL_PORT_DRAW_PIXELS
    );

    /* 设置驱动结构体的默认值：必须先 init 再修改字段。
     * hor_res/ver_res 是 LVGL 坐标系采用的逻辑分辨率。 */
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = (lv_coord_t)LVGL_PORT_HOR_RES;
    s_disp_drv.ver_res = (lv_coord_t)LVGL_PORT_VER_RES;

    /* 将“LVGL 怎么画”与“画好后怎么交给 LCD”关联起来。 */
    s_disp_drv.draw_buf = &s_draw_buf_desc;
    s_disp_drv.flush_cb = prvLVGLFlushCallback;

    /* 向 LVGL 正式注册显示设备。
     * LVGL 会在需要显示的时候，通过 flush_cb 函数指针回调上面的函数。 */
    s_registered_display = lv_disp_drv_register(&s_disp_drv);

    /* 注册失败不能宣称成功。 */
    if (s_registered_display == NULL)
    {
        return 0U;
    }

    /* 注册成功后清空调试错误记录。 */
    s_last_flush_status = ILI9341_OK;
    s_flush_error_count = 0UL;

    return 1U;
}



