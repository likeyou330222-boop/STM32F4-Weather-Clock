#ifndef ILI9341_H
#define ILI9341_H

/*
 * ============================================================================
 * @file    ili9341.h
 * @brief   ILI9341 LCD Controller Core 层公开接口
 * ============================================================================
 *
 * 一、这一层在整个驱动中的位置
 * ---------------------------------------------------------------------------
 *
 *      ili9341_gfx.c / ili9341_freertos.c       <- 更高层，以后实现
 *                     |
 *                     v
 *               ili9341.h / .c                  <- 当前文件所在层
 *                     |
 *                     v
 *            ili9341_port.h / .c                <- 已经完成的硬件适配层
 *                     |
 *                     v
 *       STM32F407 GPIO / SPI1 / DMA2 / NVIC
 *
 * Port 层解决的问题是：
 *
 *      “STM32F407 怎样可靠地把 Byte 发出去？”
 *
 * 当前 Core 层解决的问题是：
 *
 *      “这些 Byte 对 ILI9341 Controller 来说是什么意思？”
 *
 * 因此，本层开始真正负责：
 *
 *      ILI9341 命令；
 *      Controller 初始化；
 *      显示方向；
 *      显示窗口；
 *      RAMWR 显存写入事务；
 *      RGB565 像素 Byte 顺序；
 *      Blocking / DMA 两种像素数据传输路径；
 *      DMA 事件向更高层继续转发。
 *
 * 二、本头文件为什么不包含 stm32f4xx_xxx.h
 * ---------------------------------------------------------------------------
 *
 * Core 层不应该知道：
 *
 *      SPI1
 *      DMA2_Stream3
 *      GPIOA
 *      GPIOC
 *      NVIC
 *
 * 这些都属于 Port 层。
 *
 * 因此 ili9341.h 只依赖 C 标准整数类型，不直接依赖 STM32 SPL。
 *
 * 三、线程安全说明
 * ---------------------------------------------------------------------------
 *
 * 当前 Core 层本身不创建 Mutex，也不依赖 FreeRTOS。
 *
 * 所以：
 *
 *      多任务同时访问 LCD 的互斥问题
 *
 * 将在以后 ili9341_freertos.c 中统一解决。
 *
 * ============================================================================
 */




//  ili9341.h
// │
// ├─ 基础常量
// │  ├─ NATIVE_WIDTH = 240
// │  └─ NATIVE_HEIGHT = 320
// │
// ├─ 类型
// │  ├─ ILI9341_Status_t
// │  ├─ ILI9341_Rotation_t
// │  ├─ ILI9341_DMAEvent_t
// │  └─ ILI9341_DMACallback_t
// │
// ├─ 生命周期
// │  ├─ Init
// │  └─ DeInit
// │
// ├─ 显示控制
// │  ├─ DisplayOn
// │  ├─ DisplayOff
// │  ├─ BacklightOn
// │  └─ BacklightOff
// │
// ├─ 方向和尺寸
// │  ├─ SetRotation
// │  ├─ GetRotation
// │  ├─ GetWidth
// │  └─ GetHeight
// │
// ├─ Window
// │  └─ SetAddressWindow
// │
// ├─ RAMWR Transaction
// │  ├─ BeginWrite
// │  ├─ WriteBytesBlocking
// │  ├─ WritePixelsBlocking
// │  ├─ WriteColorRepeatBlocking
// │  ├─ WriteBytesDMAStart
// │  ├─ DMAFinalize
// │  ├─ EndWrite
// │  └─ AbortWrite
// │
// ├─ 基础像素
// │  ├─ WritePixel
// │  └─ Clear
// │
// └─ DMA/状态
//    ├─ IsBusy
//    ├─ GetDMAMaxTransfer
//    └─ SetDMACallback





#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif         //如果以后这个 C 驱动被 C++ 工程调用，让 C++ 编译器按照 C 的函数链接规则处理这些 API




/* ============================================================================
 * 1. ILI9341 原生（native）物理分辨率
 * ============================================================================
 *
 * Controller GRAM 的原生尺寸为：
 *
 *      240 x 320
 *
 * 旋转到横屏以后，逻辑宽高会变成：
 *
 *      320 x 240
 */
#define ILI9341_NATIVE_WIDTH                  240U
#define ILI9341_NATIVE_HEIGHT                 320U


/* ============================================================================
 * 2. Core 层统一状态码
 * ============================================================================
 *
 * 上层 GFX / FreeRTOS 不应该直接依赖 ILI9341_PortStatus_t。
 *
 * 所以 Core 自己定义一套状态码，Port 的错误会在 ili9341.c 内部进行映射。
 */
typedef enum
{
    /* 操作成功。 */
    ILI9341_OK = 0,

    /*
     * 参数非法。
     *
     * 例如：
     *      data == NULL
     *      width == 0
     *      height == 0
     *      坐标越界
     */
    ILI9341_ERROR_PARAM,

    /* Core / Port 尚未完成初始化。 */
    ILI9341_ERROR_NOT_INITIALIZED,

    /*
     * 当前资源正忙。
     *
     * 典型情况：
     *      当前存在尚未结束的 RAMWR Transaction；
     *      DMA 正在传输；
     *      DMA Complete 后尚未 Finalize。
     */
    ILI9341_ERROR_BUSY,

    /* 底层硬件等待超时。 */
    ILI9341_ERROR_TIMEOUT,

    /* DMA 本次传输失败。 */
    ILI9341_ERROR_DMA,

    /* 底层硬件已经进入不可继续按正常路径使用的状态。 */
    ILI9341_ERROR_HW,

    /*
     * Core API 调用顺序错误。
     *
     * 例如：
     *      没有 BeginWrite() 就直接 WritePixelsBlocking()；
     *      DMAStart 后还没有 Finalize 就 EndWrite()；
     *      准备结束 RAMWR 时总 Byte 数仍为奇数。
     */
    ILI9341_ERROR_STATE

} ILI9341_Status_t;


/* ============================================================================
 * 3. 显示方向
 * ============================================================================
 *
 * 下面四个方向保持当前旧工程已经验证过的四种显示模式语义：
 *
 *      0 -> 竖屏正向
 *      1 -> 竖屏翻转
 *      2 -> 横屏
 *      3 -> 横屏翻转
 *
 * 对应 MADCTL 值在 ili9341.c 内部统一管理。
 *
 * 注意：
 *
 * “正向”的视觉方向最终还与 PCB 上 LCD 的实际安装方向有关。
 */
typedef enum
{
    ILI9341_ROTATION_PORTRAIT = 0,

    ILI9341_ROTATION_PORTRAIT_FLIPPED,

    ILI9341_ROTATION_LANDSCAPE,

    ILI9341_ROTATION_LANDSCAPE_FLIPPED

} ILI9341_Rotation_t;


/* ============================================================================
 * 4. Core DMA 事件
 * ============================================================================
 *
 * Port 层的 DMA Event 不直接暴露到更高层。
 *
 * Core 收到 Port Callback 后，转换成下面的 Core Event 再继续通知上层。
 */
typedef enum
{
    /*
     * DMA 已完成 RAM -> SPI 外设的数据搬运。
     *
     * 注意：
     *      这仍然不等价于 SPI 最后一个 bit 已经物理发送结束。
     *
     * 上层收到该事件后，仍然需要在普通任务/线程上下文调用：
     *
     *      ILI9341_DMAFinalize()
     */
    ILI9341_DMA_EVENT_COMPLETE = 0,

    /* DMA 本次搬运发生错误。 */
    ILI9341_DMA_EVENT_ERROR

} ILI9341_DMAEvent_t;


/* ============================================================================
 * 5. Core DMA Callback 类型
 * ============================================================================
 *
 * event：
 *      本次 DMA 发生了什么。
 *
 * user_context：
 *      上层注册 Callback 时交给 Core 保存的用户上下文。
 *
 * 非常重要：
 *
 * 当前该 Callback 最终由 Port DMA IRQ 路径触发，
 * 因此它运行在 ISR 上下文。
 *
 * 后面 FreeRTOS 层应该在这里使用 xxxFromISR() API，
 * 而不是执行耗时操作。
 */
typedef void (*ILI9341_DMACallback_t)
(
    ILI9341_DMAEvent_t event,
    void *user_context
);


/* ============================================================================
 * 6. 生命周期
 * ============================================================================
 */

/**
 * @brief 初始化 ILI9341 Controller Core。
 *
 * 完整过程包括：
 *
 *      1. 初始化 Port；
 *      2. 注册 Port DMA Callback bridge；
 *      3. 关闭背光；
 *      4. ILI9341 Software Reset；
 *      5. 执行当前屏幕已经验证过的寄存器初始化序列；
 *      6. Sleep Out；
 *      7. 设置默认显示方向；
 *      8. Display On；
 *      9. 清黑屏；
 *     10. 最后打开背光。
 *
 * @return ILI9341_Status_t
 */
ILI9341_Status_t ILI9341_Init(void);


/**
 * @brief 关闭 Core，并反初始化底层 Port。
 *
 * 本函数会让背光关闭，并清理 Core 内部事务状态与 Callback。
 */
ILI9341_Status_t ILI9341_DeInit(void);


/* ============================================================================
 * 7. 显示开关与背光
 * ============================================================================
 */

/**
 * @brief 发送 ILI9341 Display ON 命令（0x29）。
 */
ILI9341_Status_t ILI9341_DisplayOn(void);


/**
 * @brief 发送 ILI9341 Display OFF 命令（0x28）。
 */
ILI9341_Status_t ILI9341_DisplayOff(void);


/**
 * @brief 打开 LCD 背光。
 */
ILI9341_Status_t ILI9341_BacklightOn(void);


/**
 * @brief 关闭 LCD 背光。
 */
ILI9341_Status_t ILI9341_BacklightOff(void);


/* ============================================================================
 * 8. 显示方向与逻辑尺寸
 * ============================================================================
 */

/**
 * @brief 设置 ILI9341 显示方向。
 *
 * 成功以后：
 *
 *      GetWidth()
 *      GetHeight()
 *
 * 会自动反映新的逻辑宽高。
 */
ILI9341_Status_t ILI9341_SetRotation(ILI9341_Rotation_t rotation);


/**
 * @brief 获取当前显示方向。
 */
ILI9341_Rotation_t ILI9341_GetRotation(void);


/**
 * @brief 获取当前逻辑宽度。
 *
 * 竖屏通常为 240；横屏通常为 320。
 */
uint16_t ILI9341_GetWidth(void);


/**
 * @brief 获取当前逻辑高度。
 *
 * 竖屏通常为 320；横屏通常为 240。
 */
uint16_t ILI9341_GetHeight(void);


/* ============================================================================
 * 9. Address Window
 * ============================================================================
 */

/**
 * @brief 仅设置 ILI9341 Address Window，不进入 RAMWR 数据写入状态。
 *
 * @param x       左上角 X。
 * @param y       左上角 Y。
 * @param width   窗口宽度，必须 > 0。
 * @param height  窗口高度，必须 > 0。
 *
 * 坐标范围按照当前 Rotation 后的逻辑 width / height 检查。
 */
ILI9341_Status_t ILI9341_SetAddressWindow(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height
);


/* ============================================================================
 * 10. RAMWR Transaction：Begin -> Write -> End
 * ============================================================================
 *
 * 这是 Core 层最重要的一组数据通路 API。
 *
 * 正常 Blocking 使用方式：
 *
 *      ILI9341_BeginWrite(...);
 *      ILI9341_WritePixelsBlocking(...);
 *      ILI9341_EndWrite();
 *
 * 正常 DMA 使用方式：
 *
 *      ILI9341_BeginWrite(...);
 *
 *      ILI9341_WriteBytesDMAStart(...);
 *          -> DMA IRQ
 *          -> Callback(COMPLETE)
 *
 *      ILI9341_DMAFinalize();
 *
 *      如果还有下一块：
 *          再次 DMAStart -> Finalize
 *
 *      ILI9341_EndWrite();
 *
 * BeginWrite() 成功以后：
 *
 *      CS 保持 Low；
 *      Address Window 已配置；
 *      RAMWR(0x2C) 已发送；
 *      DC 保持 Data Mode。
 *
 * EndWrite() 才会释放 CS。
 */

/**
 * @brief 开始一笔 ILI9341 GRAM/RAMWR 写入事务。
 */
ILI9341_Status_t ILI9341_BeginWrite(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height
);


/**
 * @brief 在已经 BeginWrite() 的事务中，阻塞发送原始像素 Byte Stream。
 *
 * @param data    原始 Byte 数据。
 * @param length  Byte 数。
 *
 * 对 RGB565 来说，ILI9341 线上顺序必须是：
 *
 *      Pixel high byte
 *      Pixel low byte
 *
 * 本 API 不对 data 做字节序转换。
 */
ILI9341_Status_t ILI9341_WriteBytesBlocking(
    const uint8_t *data,
    uint32_t length
);


/**
 * @brief 在已经 BeginWrite() 的事务中，阻塞发送 uint16_t RGB565 像素。
 *
 * 本函数会自动把 STM32 小端 uint16_t：
 *
 *      0xF800
 *
 * 转换成 ILI9341 线上需要的：
 *
 *      0xF8, 0x00
 *
 * 因此普通 uint16_t 像素数组可以安全使用该接口。
 */
ILI9341_Status_t ILI9341_WritePixelsBlocking(
    const uint16_t *pixels,
    uint32_t pixel_count
);


/**
 * @brief 在已经 BeginWrite() 的事务中，把同一个 RGB565 颜色重复写入若干像素。
 *
 * 典型用途：
 *
 *      Clear Screen；
 *      以后 GFX 层做纯色填充。
 */
ILI9341_Status_t ILI9341_WriteColorRepeatBlocking(
    uint16_t color,
    uint32_t pixel_count
);


/**
 * @brief 在已经 BeginWrite() 的事务中，异步启动一笔 DMA Byte Stream。
 *
 * @param data    DMA Buffer 首地址。
 * @param length  本次 DMA Byte 数，不能超过 GetDMAMaxTransfer()。
 *
 * 非常重要：
 *
 *      本函数返回 OK 只表示 DMA 已成功启动。
 *
 *      data 在 DMA 完成以前必须一直有效，不能释放、覆盖或修改。
 *
 *      如果 data 是 RGB565 像素数据，那么它必须已经按照 ILI9341
 *      线上顺序组织成 high-byte / low-byte Byte Stream。
 */
ILI9341_Status_t ILI9341_WriteBytesDMAStart(
    const uint8_t *data,
    uint32_t length
);


/**
 * @brief 对当前 Core DMA 分片执行最终收尾。
 *
 * 收到 ILI9341_DMA_EVENT_COMPLETE / ERROR 后，
 * 应该在普通任务/线程上下文调用本函数。
 *
 * 本函数不会结束整个 RAMWR Transaction，也不会拉高 CS。
 */
ILI9341_Status_t ILI9341_DMAFinalize(void);


/**
 * @brief 正常结束当前 RAMWR Transaction。
 *
 * 调用条件：
 *
 *      没有尚未 Finalize 的 DMA；
 *      Port 不处于 Busy。
 *
 * 成功以后 CS 被释放，Core 回到 READY。
 */
ILI9341_Status_t ILI9341_EndWrite(void);


/**
 * @brief 强制终止当前 RAMWR Transaction。
 *
 * 典型用途：
 *
 *      DMA Error；
 *      上层取消刷新；
 *      错误恢复。
 *
 * 本函数会尝试 Abort Port DMA，并释放 LCD CS。
 */
ILI9341_Status_t ILI9341_AbortWrite(void);


/* ============================================================================
 * 11. 常用像素基础能力
 * ============================================================================
 *
 * 这些仍然属于 Controller Core 的“最基础像素通路”，
 * 不包含线、圆、矩形、文字等 GFX 算法。
 */

/**
 * @brief 写一个 RGB565 像素。
 */
ILI9341_Status_t ILI9341_WritePixel(
    uint16_t x,
    uint16_t y,
    uint16_t color
);


/**
 * @brief 使用一个 RGB565 颜色清满当前逻辑屏幕。
 */
ILI9341_Status_t ILI9341_Clear(uint16_t color);


/* ============================================================================
 * 12. DMA 查询与 Callback
 * ============================================================================
 */

/**
 * @brief 查询当前 Core 是否不适合开始新的独立 LCD 操作。
 *
 * @return
 *      0：Core READY 且 Port DMA 不忙。
 *      非 0：存在 RAMWR Transaction、DMA、ERROR/FAULT 等状态。
 */
uint8_t ILI9341_IsBusy(void);


/**
 * @brief 获取底层 Port 单笔 DMA 最大 Byte 数。
 */
uint32_t ILI9341_GetDMAMaxTransfer(void);


/**
 * @brief 注册 Core DMA 事件回调。
 *
 * 更高层（例如以后 FreeRTOS 层）应该注册 Core Callback，
 * 而不是越层直接注册 Port Callback。
 */
void ILI9341_SetDMACallback(
    ILI9341_DMACallback_t callback,
    void *user_context
);


#ifdef __cplusplus
}
#endif

#endif /* ILI9341_H */
