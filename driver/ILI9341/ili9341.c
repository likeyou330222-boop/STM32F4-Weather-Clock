/**
 * ============================================================================
 * @file    ili9341.c
 * @brief   ILI9341 LCD Controller Core 层实现
 * ============================================================================
 *
 * 一、文件定位
 * ---------------------------------------------------------------------------
 *
 * 本文件位于：
 *
 *      GFX / FreeRTOS 等更高层
 *               |
 *               v
 *          ili9341.c                 <- 当前文件
 *               |
 *               v
 *        ili9341_port.c
 *               |
 *               v
 *     STM32F407 SPI / DMA / GPIO
 *
 * 本文件已经开始真正理解 ILI9341 Controller 协议。
 *
 * Port 层只知道：
 *
 *      Select / Unselect
 *      CommandMode / DataMode
 *      WriteBlocking
 *      DMAStart / DMAFinalize / DMAAbort
 *
 * 本层则知道：
 *
 *      0x2A = Column Address Set
 *      0x2B = Page Address Set
 *      0x2C = Memory Write
 *      0x36 = Memory Access Control
 *      0x3A = Pixel Format
 *
 * 二、当前设计的最重要边界
 * ---------------------------------------------------------------------------
 *
 * 1. Core 决定完整 LCD Transaction。
 *
 *    因此：
 *
 *        CS 什么时候 Low；
 *        CS 什么时候 High；
 *        DC 什么时候 Command；
 *        DC 什么时候 Data；
 *
 *    都由本层组织。
 *
 * 2. Port 只执行硬件动作。
 *
 * 3. GFX 以后只组合 Core 提供的窗口/像素能力，
 *    不应该直接碰 SPI1 / DMA2_Stream3。
 *
 * 4. FreeRTOS 以后只负责：
 *
 *        Mutex
 *        Queue
 *        Task Notification
 *
 *    Core 本身不依赖 RTOS。
 *
 * 三、当前 DMA 模型
 * ---------------------------------------------------------------------------
 *
 * BeginWrite()
 *      |
 *      v
 * CS Low + Window + RAMWR + DataMode
 *      |
 *      +-----------------------------+
 *      |                             |
 *      v                             v
 * Blocking                       DMAStart
 *                                    |
 *                                    v
 *                                DMA IRQ
 *                                    |
 *                                    v
 *                                Callback
 *                                    |
 *                                    v
 *                               DMAFinalize
 *      |                             |
 *      +-------------+---------------+
 *                    v
 *                EndWrite()
 *                    |
 *                    v
 *                 CS High
 *
 * ============================================================================
 */

#include "ili9341.h"
#include "ili9341_port.h"


/* ============================================================================
 * 1. ILI9341 Command 定义
 * ============================================================================
 *
 * 这些 Command 属于 ILI9341 Controller 协议，
 * 所以只应该出现在 Core 层，不应该放进 Port 层。
 */

/* Software Reset。 */
#define ILI9341_CMD_SWRESET                   0x01U

/* Display OFF。 */
#define ILI9341_CMD_DISPLAY_OFF               0x28U

/* Display ON。 */
#define ILI9341_CMD_DISPLAY_ON                0x29U

/* Column Address Set。（列地址设置，即设置 X 方向的起点和终点） */
#define ILI9341_CMD_CASET                     0x2AU

/* Page Address Set。（Y 方向地址范围） */
#define ILI9341_CMD_PASET                     0x2BU

/* Memory Write / RAMWR。（显存写入 / 存储器写入） */
#define ILI9341_CMD_RAMWR                     0x2CU

/* Memory Access Control / MADCTL。 （显存访问控制）*/
#define ILI9341_CMD_MADCTL                    0x36U

/* Pixel Format Set。（像素格式） */
#define ILI9341_CMD_PIXFMT                    0x3AU

/* Sleep Out。 （退出睡眠）*/
#define ILI9341_CMD_SLPOUT                    0x11U


/* ============================================================================
 * 2. ILI9341 扩展初始化寄存器 Command
 * ============================================================================
 *
 * 当前值保持原工程中已经能够正常驱动这块 LCD 的初始化序列。
 */
#define ILI9341_CMD_POWER_CONTROL_A           0xCBU  //电源控制
#define ILI9341_CMD_POWER_CONTROL_B           0xCFU  //电源控制
#define ILI9341_CMD_POWER_ON_SEQUENCE         0xEDU  //上电时序
#define ILI9341_CMD_DRIVER_TIMING_A           0xE8U  //驱动时序
#define ILI9341_CMD_PUMP_RATIO_CONTROL        0xF7U  //电荷泵倍率控制
#define ILI9341_CMD_DRIVER_TIMING_B           0xEAU  //驱动时序
#define ILI9341_CMD_POWER_CONTROL_1           0xC0U  //电源控制
#define ILI9341_CMD_POWER_CONTROL_2           0xC1U  //电源控制
#define ILI9341_CMD_VCOM_CONTROL_1            0xC5U  //公共电极电压控制
#define ILI9341_CMD_VCOM_CONTROL_2            0xC7U  //公共电极电压控制
#define ILI9341_CMD_FRAME_RATE_CONTROL        0xB1U  //刷新率控制
#define ILI9341_CMD_DISPLAY_FUNCTION_CONTROL  0xB6U  //显示功能控制
#define ILI9341_CMD_3GAMMA_DISABLE            0xF2U
#define ILI9341_CMD_GAMMA_CURVE               0x26U
#define ILI9341_CMD_POSITIVE_GAMMA            0xE0U
#define ILI9341_CMD_NEGATIVE_GAMMA            0xE1U


/* ============================================================================
 * 3. MADCTL bit 定义（显存访问控制）
 * ============================================================================
 * 一个 Byte 有：8个二进制位
 * ILI9341 MADCTL（0x36）主要相关位：
 *
 *      MY   bit7：Y 方向镜像
 *      MX   bit6：X 方向镜像
 *      MV   bit5：X / Y 交换
 *      BGR  bit3：BGR Color Order
 *
 * 当前屏幕旧工程一直使用 BGR bit = 1。
 */
#define ILI9341_MADCTL_MY                     0x80U  //第七个二进制位
#define ILI9341_MADCTL_MX                     0x40U  //第六个二进制位
#define ILI9341_MADCTL_MV                     0x20U  //第五个二进制位
#define ILI9341_MADCTL_BGR                    0x08U  //第三个二进制位


/* ============================================================================
 * 4. Core 内部配置
 * ============================================================================
 */

/*
 * 初始化最后清屏使用白色 RGB565。
 *
 * 0xFFFF = White。
 */
#define ILI9341_INIT_CLEAR_COLOR              0xFFFFU

/*
 * Blocking uint16_t Pixel 转 Byte 时的临时分块大小。
 *
 * 32 Pixel x 2 Byte = 64 Byte stack buffer。
 *
 * 这个尺寸足够小，不会明显增加嵌入式任务栈压力；
 * 同时又比“每个 Pixel 调一次 Port”高效得多。
 */
#define ILI9341_CONVERT_CHUNK_PIXELS          32UL


/* ============================================================================
 * 5. Core 内部状态机
 * ============================================================================
 *
 * UNINITIALIZED
 *      |
 *      | Init 成功
 *      v
 * READY
 *      |
 *      | BeginWrite
 *      v
 * WRITE_ACTIVE
 *      |
 *      | EndWrite / AbortWrite
 *      v
 * READY
 *
 * 如果底层硬件出现不可继续安全使用的问题：
 *
 *      -> FAULT
 *
 * FAULT 建议通过：
 *
 *      DeInit()
 *          -> Init()
 *
 * 重新建立完整硬件状态。
 */
typedef enum
{
    ILI9341_CORE_STATE_UNINITIALIZED = 0,

    ILI9341_CORE_STATE_READY,

    ILI9341_CORE_STATE_WRITE_ACTIVE,

    ILI9341_CORE_STATE_FAULT

} ILI9341_CoreState_t;


/* ============================================================================
 * 6. Core 内部状态变量
 * ============================================================================
 */

/* 当前 Core 生命周期 / Transaction 状态。 */
static volatile ILI9341_CoreState_t s_core_state = ILI9341_CORE_STATE_UNINITIALIZED;

/* 当前逻辑宽度。默认值先按竖屏原生宽度初始化。 */
static uint16_t s_width = ILI9341_NATIVE_WIDTH;

/* 当前逻辑高度。默认值先按竖屏原生高度初始化。 */
static uint16_t s_height = ILI9341_NATIVE_HEIGHT;

/* 当前显示方向。 */
static ILI9341_Rotation_t s_rotation = ILI9341_ROTATION_PORTRAIT;

/*
 * 当前 BeginWrite() Window 理论上最多可以写入多少 Byte。
 *
 * RGB565：    1 Pixel = 2 Byte
 *
 * 所以：   limit = width * height * 2
 */
static uint32_t s_write_limit_bytes = 0UL;

/*
 * 当前 RAMWR Transaction 中，
 * 已经确认成功完成的 Byte 数。
 */
static uint32_t s_write_committed_bytes = 0UL;

/*
 * 当前已经 DMAStart()，
 * 但是还没有通过 DMAFinalize() 最终确认成功的 Byte 数。
 * pending 待定
 */
static uint32_t s_dma_pending_bytes = 0UL;

/*
 * 当前 RAMWR Transaction 是否已经出现 DMA Error。
 *
 * 这个变量可能由 DMA ISR Callback 修改，
 * 所以声明 volatile。
 */
static volatile uint8_t s_write_failed = 0U;

/* 上层注册的 Core DMA Callback。 */
static ILI9341_DMACallback_t s_dma_callback = NULL;

/* 与 Core DMA Callback 对应的用户上下文。 */
static void *s_dma_callback_context = NULL;


/* ============================================================================
 * 7. Core 内部辅助函数声明
 * ============================================================================
 */


/**
 * @brief
 *      将 Port 层状态码转换成 Core 层状态码。
 *
 * @param port_status
 *      Port 层返回的状态，例如：
 *
 *          ILI9341_PORT_OK
 *          ILI9341_PORT_ERROR_BUSY
 *          ILI9341_PORT_ERROR_TIMEOUT
 *          ILI9341_PORT_ERROR_DMA
 *          ...
 *
 * @return
 *      对应的 Core 层 ILI9341_Status_t。
 *
 * @note
 *      本函数只负责“状态码翻译”。
 *
 *      它不会修改 Core 状态机，
 *      也不会自动进入 FAULT。
 */
static ILI9341_Status_t prvMapPortStatus(ILI9341_PortStatus_t port_status);


/**
 * @brief
 *      将 Port 状态码转换成 Core 状态码，
 *      并在检测到严重底层硬件错误时，
 *      将 Core 状态锁存为 FAULT。
 *
 * @param port_status
 *      Port 层返回的状态码。
 *
 * @return
 *      转换后的 Core 层状态码。
 *
 * @note
 *      与 prvMapPortStatus() 的区别：
 *
 *      prvMapPortStatus()
 *          只翻译错误码；
 *
 *      prvMapPortStatusAndLatchFault()
 *          除了翻译错误码，
 *          对 TIMEOUT / HW 这种严重错误，
 *          还会把 Core 锁入 FAULT。
 *
 *      Latch：
 *          中文可理解为“锁存”。
 *
 *      即严重故障发生后，
 *      不能因为当前函数返回了就假装系统恢复正常。
 */
static ILI9341_Status_t prvMapPortStatusAndLatchFault(ILI9341_PortStatus_t port_status);


/**
 * @brief
 *      检查当前 Core 是否处于 READY 状态。
 *
 * @return
 *      ILI9341_OK
 *          当前 Core 已初始化且空闲，可以开始新的独立操作。
 *
 *      ILI9341_ERROR_NOT_INITIALIZED
 *          Core 尚未初始化。
 *
 *      ILI9341_ERROR_HW
 *          Core 已进入 FAULT。
 *
 *      ILI9341_ERROR_BUSY
 *          Core 当前不是 READY，
 *          例如正处于 WRITE_ACTIVE。
 *
 * @note
 *      很多只能在“完全空闲”状态下执行的公开 API，
 *      会复用这个检查函数。
 */
static ILI9341_Status_t prvRequireReady(void);


/**
 * @brief
 *      检查一个矩形显示区域是否完整落在
 *      当前逻辑屏幕范围内。
 *
 * @param x
 *      矩形左上角 X 坐标。
 *
 * @param y
 *      矩形左上角 Y 坐标。
 *
 * @param width
 *      矩形宽度，必须大于 0。
 *
 * @param height
 *      矩形高度，必须大于 0。
 *
 * @return
 *      ILI9341_OK
 *          区域合法。
 *
 *      ILI9341_ERROR_PARAM
 *          宽高为 0、起点越界或矩形末端越界。
 *
 * @note
 *      判断依据使用当前 Rotation 对应的：
 *
 *          s_width
 *          s_height
 *
 *      因此横屏和竖屏会自动使用不同的逻辑边界。
 */
static ILI9341_Status_t prvValidateArea(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height
);


/**
 * @brief
 *      清空当前 RAMWR 写事务的软件统计信息。
 *
 * @note
 *      会清零：
 *
 *          s_write_limit_bytes
 *          s_write_committed_bytes
 *          s_dma_pending_bytes
 *          s_write_failed
 *
 *      它只清理“一笔写事务”的统计，
 *      不会把整个 Core 反初始化。
 */
static void prvResetWriteSession(void);


/**
 * @brief
 *      在 LCD 已经处于 Selected 状态，
 *      即 CS 已经为 Low 的前提下，
 *      发送一条 ILI9341 Command 以及其可选参数。
 *
 * @param command
 *      要发送的 ILI9341 命令 Byte。
 *
 * @param parameters
 *      命令后面的参数 Buffer。
 *
 *      如果 parameter_length == 0，
 *      parameters 可以为 NULL。
 *
 * @param parameter_length
 *      参数区长度，单位为 Byte。
 *
 * @return
 *      Core 层状态码。
 *
 * @note
 *      Selected：
 *          中文可理解为“已经选中设备”。
 *
 *      本函数不会负责：
 *
 *          CS Low
 *          CS High
 *
 *      它假设调用者已经执行了：
 *
 *          ILI9341_Port_Select();
 *
 *      这样可以在同一个 CS Low Transaction 中
 *      连续发送 CASET、PASET、RAMWR 等命令。
 */
static ILI9341_Status_t prvWriteCommandSelected(
    uint8_t command,
    const uint8_t *parameters,
    uint32_t parameter_length
);


/**
 * @brief
 *      完整发送一条独立的 ILI9341 Command Transaction。
 *
 * @param command
 *      要发送的 ILI9341 命令。
 *
 * @param parameters
 *      命令参数 Buffer。
 *
 * @param parameter_length
 *      参数长度，单位为 Byte。
 *
 * @return
 *      Core 层状态码。
 *
 * @note
 *      与 prvWriteCommandSelected() 不同，
 *      本函数自己负责完整的：
 *
 *          CS Low
 *              ↓
 *          Command
 *              ↓
 *          Parameters
 *              ↓
 *          CS High
 *
 *      因此适合发送一条完全独立的普通命令。
 */
static ILI9341_Status_t prvWriteCommand(
    uint8_t command,
    const uint8_t *parameters,
    uint32_t parameter_length
);


/**
 * @brief
 *      在 CS 已经保持 Low 的前提下，
 *      设置 ILI9341 当前 Address Window。
 *
 * @param x
 *      窗口左上角 X。
 *
 * @param y
 *      窗口左上角 Y。
 *
 * @param width
 *      窗口宽度。
 *
 * @param height
 *      窗口高度。
 *
 * @return
 *      Core 层状态码。
 *
 * @note
 *      Address Window：
 *          中文可以理解为“显存地址窗口”。
 *
 *      本函数最终会发送：
 *
 *          CASET 0x2A
 *              设置 X 起点 / 终点
 *
 *          PASET 0x2B
 *              设置 Y 起点 / 终点
 *
 *      但不会发送 RAMWR。
 */
static ILI9341_Status_t prvSetWindowSelected(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height
);


/**
 * @brief
 *      根据指定的显示方向，
 *      计算对应的 MADCTL 参数以及逻辑宽高。
 *
 * @param rotation
 *      目标显示方向。
 *
 * @param madctl
 *      输出：
 *      对应 ILI9341 MADCTL(0x36) 的参数 Byte。
 *
 * @param width
 *      输出：
 *      该 Rotation 下的逻辑屏幕宽度。
 *
 * @param height
 *      输出：
 *      该 Rotation 下的逻辑屏幕高度。
 *
 * @return
 *      ILI9341_OK 或 ILI9341_ERROR_PARAM。
 *
 * @note
 *      Config：
 *          Configuration，配置。
 *
 *      本函数只“计算配置”，
 *      本身不会向 LCD 发送命令。
 */
static ILI9341_Status_t prvGetRotationConfig(
    ILI9341_Rotation_t rotation,
    uint8_t *madctl,
    uint16_t *width,
    uint16_t *height
);


/**
 * @brief
 *      真正把指定 Rotation 应用到 ILI9341 Controller。
 *
 * @param rotation
 *      需要应用的目标显示方向。
 *
 * @return
 *      Core 层状态码。
 *
 * @note
 *      Apply：
 *          中文为“应用 / 实施”。
 *
 *      本函数会使用 prvGetRotationConfig()
 *      得到 MADCTL 和逻辑宽高，
 *      再真正向 ILI9341 写入 MADCTL，
 *      并更新 Core 内部的：
 *
 *          s_rotation
 *          s_width
 *          s_height
 */
static ILI9341_Status_t prvApplyRotation(
    ILI9341_Rotation_t rotation
);


/**
 * @brief
 *      执行 ILI9341 Controller 的整套寄存器初始化序列。
 *
 * @return
 *      Core 层状态码。
 *
 * @note
 *      Sequence：
 *          中文为“序列 / 顺序”。
 *
 *      本函数负责按既定顺序发送：
 *
 *          电源配置
 *          驱动时序
 *          VCOM
 *          Pixel Format
 *          Frame Rate
 *          Gamma
 *          ...
 *
 *      这些参数保持当前旧工程已经验证过的初始化配置。
 */
static ILI9341_Status_t prvRunInitSequence(void);


/**
 * @brief
 *      Port DMA Callback 到 Core DMA Callback 之间的桥接函数。
 *
 * @param event
 *      Port 层产生的 DMA Event。
 *
 * @param user_context
 *      Port 回传的用户上下文。
 *
 * @note
 *      Bridge：
 *          中文可以理解为“桥接”。
 *
 *      调用关系：
 *
 *          DMA IRQ
 *              ↓
 *          Port Callback
 *              ↓
 *          prvPortDMACallback()
 *              ↓
 *          转换成 Core Event
 *              ↓
 *          上层 Core Callback
 *
 *      当前函数运行在 DMA ISR 上下文。
 */
static void prvPortDMACallback(
    ILI9341_PortDMAEvent_t event,
    void *user_context
);


/**
 * @brief
 *      将整个 Core 软件状态恢复到默认的
 *      “未初始化”状态。
 *
 * @note
 *      会恢复：
 *
 *          Core State -> UNINITIALIZED
 *          Width      -> 原生宽度
 *          Height     -> 原生高度
 *          Rotation   -> 默认竖屏
 *
 *      并调用 prvResetWriteSession()
 *      清除写事务统计。
 */
static void prvResetCoreState(void);


/**
 * @brief
 *      当 ILI9341_Init() 中途失败时，
 *      统一执行失败清理流程。
 *
 * @param failure_status
 *      导致 Init 失败的原始 Core 状态码。
 *
 * @return
 *      返回最终应报告给上层的状态码。
 *
 * @note
 *      Cleanup：
 *          中文为“清理 / 收尾”。
 *
 *      本函数用于避免 Init 的每一个失败分支
 *      都重复写一整套：
 *
 *          Backlight Off
 *          Callback 清除
 *          Port DeInit
 *          Core State Reset
 */
static ILI9341_Status_t prvInitFailCleanup(
    ILI9341_Status_t failure_status
);


/* ============================================================================
 * 8. Port Status -> Core Status（把 Port 状态转换成 Core 状态）
 * ============================================================================
 */
static ILI9341_Status_t prvMapPortStatus(ILI9341_PortStatus_t port_status)
{
    switch (port_status)
    {
        case ILI9341_PORT_OK:
            return ILI9341_OK;

        case ILI9341_PORT_ERROR_PARAM:
            return ILI9341_ERROR_PARAM;

        case ILI9341_PORT_ERROR_NOT_INITIALIZED:
            return ILI9341_ERROR_NOT_INITIALIZED;

        case ILI9341_PORT_ERROR_BUSY:
            return ILI9341_ERROR_BUSY;

        case ILI9341_PORT_ERROR_TIMEOUT:
            return ILI9341_ERROR_TIMEOUT;

        case ILI9341_PORT_ERROR_DMA:
            return ILI9341_ERROR_DMA;

        case ILI9341_PORT_ERROR_HW:
        default:
            return ILI9341_ERROR_HW;
    }
}


/* ============================================================================
 * 9. 映射 Port Status，并在严重硬件错误时锁存 Core FAULT（即把 Port 状态转换成 Core 状态，并在必要时把严重故障锁存下来。）
 * ============================================================================
 */
static ILI9341_Status_t prvMapPortStatusAndLatchFault(ILI9341_PortStatus_t port_status)
{
    ILI9341_Status_t status;

    /*
     * 先把 Port 层状态翻译成 Core 层状态。
     */
    status = prvMapPortStatus(port_status);

    /*
     * TIMEOUT / HW 不是普通业务错误。
     *
     * 它们通常意味着：
     *
     *      SPI Flag 长时间不变化；
     *      DMA Stream 无法进入期望状态；
     *      底层已经无法确认硬件处于正常状态。
     *
     * 所以只要 Core 已经完成初始化，
     * 就把它锁存为 FAULT。
     */
    if (
        (port_status == ILI9341_PORT_ERROR_TIMEOUT) ||
        (port_status == ILI9341_PORT_ERROR_HW)
    )
    {
        if (s_core_state != ILI9341_CORE_STATE_UNINITIALIZED)
        {
            s_core_state = ILI9341_CORE_STATE_FAULT;

            /*
             * 出现严重底层错误时，
             * 最起码先释放 LCD CS，
             * 避免 LCD 继续把后续异常时钟理解成当前 Transaction。
             */
            ILI9341_Port_Unselect();
        }
    }

    return status;
}


/* ============================================================================
 * 10. 要求 Core 当前必须处于 READY
 * ============================================================================
 */
static ILI9341_Status_t prvRequireReady(void)
{
    if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    if (s_core_state == ILI9341_CORE_STATE_FAULT)
    {
        return ILI9341_ERROR_HW;
    }

    if (s_core_state != ILI9341_CORE_STATE_READY)
    {
        return ILI9341_ERROR_BUSY;
    }

    return ILI9341_OK;
}


/* ============================================================================
 * 11. 校验一快矩形显示区域是否合法
 * ============================================================================
 */
static ILI9341_Status_t prvValidateArea(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height)
{
    uint32_t end_x_exclusive;
    uint32_t end_y_exclusive;

    /* Window 尺寸不能为 0。 */
    if ((width == 0U) || (height == 0U))
    {
        return ILI9341_ERROR_PARAM;
    }

    /* 起点本身必须在当前逻辑屏幕范围内。 */
    if ((x >= s_width) || (y >= s_height))
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * 使用 uint32_t 做加法，
     * 避免 uint16_t x + width 发生 16 bit 回绕。
     */
    end_x_exclusive = (uint32_t)x + (uint32_t)width;
    end_y_exclusive = (uint32_t)y + (uint32_t)height;

    /*
     * 采用“右开区间”判断：
     *
     *      [x, x + width)
     *
     * 必须完整落在：
     *
     *      [0, s_width)
     */
    if (
        (end_x_exclusive > (uint32_t)s_width) ||
        (end_y_exclusive > (uint32_t)s_height)
    )
    {
        return ILI9341_ERROR_PARAM;
    }

    return ILI9341_OK;
}


/* ============================================================================
 * 12. 清空当前 RAMWR Transaction 的软件统计状态
 * ============================================================================
 */
static void prvResetWriteSession(void)
{
    s_write_limit_bytes = 0UL;

    s_write_committed_bytes = 0UL;

    s_dma_pending_bytes = 0UL;

    s_write_failed = 0U;
}


/* ============================================================================
 * 13. 在“CS 已经选中”的前提下发送一个 ILI9341 Command
 * ============================================================================
 *
 * 这个函数不负责 Select / Unselect。
 *
 * 这样 BeginWrite() 就可以保持：
 *
 *      CS 一直 Low
 *
 * 连续发送：
 *
 *      CASET
 *      PASET
 *      RAMWR
 *
 * 然后继续像素数据。
 */
static ILI9341_Status_t prvWriteCommandSelected(uint8_t command, const uint8_t *parameters, uint32_t parameter_length)
{
    ILI9341_PortStatus_t port_status;

    /*
     * 如果有参数，parameters 必须有效。
     *
     * parameter_length == 0 时，
     * parameters == NULL 是完全合法的。
     */
    if ((parameter_length > 0UL) && (parameters == NULL))  //如果你告诉我“后面有参数”，但是又没有给我参数数据的地址，那么参数非法。
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * DC = 0。
     *
     * 接下来的 1 Byte 被 ILI9341 解释成 Command。 也就是切换成命令模式
     */
    ILI9341_Port_CommandMode();

    /*
     * 发送 Command 本身。
     */
    port_status = ILI9341_Port_WriteBlocking(&command, 1UL);

    if (port_status != ILI9341_PORT_OK)
    {
        return prvMapPortStatusAndLatchFault(port_status);
    }

    /*
     * 如果 Command 后面还有参数，
     * 再切到 DC = 1 发送参数区。
     */
    if (parameter_length > 0UL)
    {
        ILI9341_Port_DataMode();

        port_status =
            ILI9341_Port_WriteBlocking(
                parameters,
                parameter_length
            );

        if (port_status != ILI9341_PORT_OK)
        {
            return prvMapPortStatusAndLatchFault(port_status);
        }
    }

    return ILI9341_OK;
}


/* ============================================================================
 * 14. 完整发送一个独立 ILI9341 Command Transaction
 * ============================================================================
 */
static ILI9341_Status_t prvWriteCommand(uint8_t command, const uint8_t *parameters, uint32_t parameter_length)
{
    ILI9341_Status_t status;

    /*
     * 一笔独立 Command Transaction 开始：
     *
     *      CS = Low
     */
    ILI9341_Port_Select();

    /*
     * 真正发送 Command + Parameters。
     */
    status =
        prvWriteCommandSelected(
            command,
            parameters,
            parameter_length
        );

    /*
     * 不论发送成功还是失败，
     * 当前独立 Command Transaction 都在这里释放 CS。
     */
    ILI9341_Port_Unselect();

    return status;
}


/* ============================================================================
 * 15. 在“CS 已选中”的前提下设置 Address Window
 * ============================================================================
 */
static ILI9341_Status_t prvSetWindowSelected(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height
)
{
    uint16_t x_end;
    uint16_t y_end;

    uint8_t column_data[4];
    uint8_t page_data[4];

    ILI9341_Status_t status;

    /*
     * 上层在进入本函数前已经做过区域校验，
     * 因此这里可以安全计算 inclusive end coordinate：
     *
     *      end = start + size - 1
     */
    x_end =
        (uint16_t)(
            (uint32_t)x +
            (uint32_t)width -
            1UL
        );

    y_end =
        (uint16_t)(
            (uint32_t)y +
            (uint32_t)height -
            1UL
        );

    /*
     * CASET（设置 X 方向范围）(0x2A) 参数要求：
     *
     *      x起点高八位[15:8]
     *      x起点低八位[7:0]
     *      x终点高八位[15:8]
     *      x终点高八位[7:0]
     *
     * ILI9341 线上采用高字节在前。
     */
    column_data[0] = (uint8_t)(x >> 8);
    column_data[1] = (uint8_t)(x & 0x00FFU);
    column_data[2] = (uint8_t)(x_end >> 8);
    column_data[3] = (uint8_t)(x_end & 0x00FFU);

    status =
        prvWriteCommandSelected(
            ILI9341_CMD_CASET,
            column_data,
            4UL
        );

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * PASET(0x2B) 参数格式与 CASET 相同，
     * 只不过这里设置的是 Y / Page 范围。
     *      y起点高八位[15:8]
     *      y起点低八位[7:0]
     *      y终点高八位[15:8]
     *      y终点高八位[7:0]
     */
    page_data[0] = (uint8_t)(y >> 8);
    page_data[1] = (uint8_t)(y & 0x00FFU);
    page_data[2] = (uint8_t)(y_end >> 8);
    page_data[3] = (uint8_t)(y_end & 0x00FFU);

    status =
        prvWriteCommandSelected(
            ILI9341_CMD_PASET,
            page_data,
            4UL
        );

    return status;
}


/* ============================================================================
 * 16. 根据 Rotation 计算 MADCTL 与逻辑宽高(根据指定的显示方向，获取这一方向对应的配置。)
 * ============================================================================
 *
 * madtcl 是 显存访问控制寄存器
 * 
 * 这里严格保持旧工程中已经验证过的四种模式：
 *
 * mode 0：L2R_U2D -> MADCTL = 0x08  //X 方向从左到右，Y 方向从上到下。
 * mode 1：R2L_D2U -> MADCTL = 0xC8  //X 方向从右到左，Y 方向从下到上。
 * mode 2：U2D_R2L -> MADCTL = 0x68  //X 方向从右到左，Y 方向从上到下。
 * mode 3：D2U_L2R -> MADCTL = 0xA8  //X 方向从左到右，Y 方向从下到上。
 */
static ILI9341_Status_t prvGetRotationConfig(
    ILI9341_Rotation_t rotation,
    uint8_t *madctl,
    uint16_t *width,
    uint16_t *height
)
{
    if ((madctl == NULL) || (width == NULL) || (height == NULL))
    {
        return ILI9341_ERROR_PARAM;
    }

    switch (rotation)
    {
        case ILI9341_ROTATION_PORTRAIT:  //(竖屏正向)
            *madctl = ILI9341_MADCTL_BGR;
            *width = ILI9341_NATIVE_WIDTH;
            *height = ILI9341_NATIVE_HEIGHT;
            break;

        case ILI9341_ROTATION_PORTRAIT_FLIPPED:
            *madctl =
                ILI9341_MADCTL_MY |
                ILI9341_MADCTL_MX |
                ILI9341_MADCTL_BGR;

            *width = ILI9341_NATIVE_WIDTH;
            *height = ILI9341_NATIVE_HEIGHT;
            break;

        case ILI9341_ROTATION_LANDSCAPE:
            *madctl =
                ILI9341_MADCTL_MX |
                ILI9341_MADCTL_MV |
                ILI9341_MADCTL_BGR;

            *width = ILI9341_NATIVE_HEIGHT;
            *height = ILI9341_NATIVE_WIDTH;
            break;

        case ILI9341_ROTATION_LANDSCAPE_FLIPPED:
            *madctl =
                ILI9341_MADCTL_MY |
                ILI9341_MADCTL_MV |
                ILI9341_MADCTL_BGR;

            *width = ILI9341_NATIVE_HEIGHT;
            *height = ILI9341_NATIVE_WIDTH;
            break;

        default:
            return ILI9341_ERROR_PARAM;
    }

    return ILI9341_OK;
}


/* ============================================================================
 * 17. 真正把 Rotation 写入 ILI9341
 * ============================================================================
 */
static ILI9341_Status_t prvApplyRotation(ILI9341_Rotation_t rotation)
{
    uint8_t madctl;

    uint16_t new_width;
    uint16_t new_height;

    ILI9341_Status_t status;

    /*
     * 第一步：
     * 先计算这一个 Rotation 对应的寄存器值与逻辑尺寸。
     */
    status =
        prvGetRotationConfig(
            rotation,
            &madctl,
            &new_width,
            &new_height
        );

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * 第二步：
     * 发送 MADCTL(0x36)。
     */
    status =
        prvWriteCommand(
            ILI9341_CMD_MADCTL,
            &madctl,
            1UL
        );

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * 只有硬件寄存器写成功以后，
     * 才同步更新软件记录。
     */
    s_rotation = rotation;

    s_width = new_width;

    s_height = new_height;

    return ILI9341_OK;
}


/* ============================================================================
 * 18. ILI9341 Controller 初始化寄存器序列（执行 ILI9341 的内部初始化配置序列）
 * ============================================================================
 *
 * 这一组参数不是随意换成网上另一套“看起来也能用”的值。
 *
 * 当前值直接保持用户原工程已经实际使用过的屏幕初始化序列。
 *
 * Software Reset / Sleep Out / MADCTL / Display On
 * 会在 ILI9341_Init() 中单独组织，
 * 因此这里主要处理其余 Controller 参数。
 */
static ILI9341_Status_t prvRunInitSequence(void)
{
    ILI9341_Status_t status;

    /* ------------------------------------------------------------------------
     * Power Control B：0xCF
     * ------------------------------------------------------------------------
     */
    {

        //static const data[] = 一组长期存在、运行时不修改的固定配置数据。
        static const uint8_t data[] =
        {
            0x00U,
            0xD9U,
            0x30U
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_POWER_CONTROL_B,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Power On Sequence Control：0xED
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x64U,
            0x03U,
            0x12U,
            0x81U
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_POWER_ON_SEQUENCE,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Driver Timing Control A：0xE8
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x85U,
            0x10U,
            0x78U
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_DRIVER_TIMING_A,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Power Control A：0xCB
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x39U,
            0x2CU,
            0x00U,
            0x34U,
            0x02U
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_POWER_CONTROL_A,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Pump Ratio Control：0xF7
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] = { 0x20U };

        status =
            prvWriteCommand(
                ILI9341_CMD_PUMP_RATIO_CONTROL,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Driver Timing Control B：0xEA
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x00U,
            0x00U
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_DRIVER_TIMING_B,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Power Control 1：0xC0
     * 旧工程使用 VRH = 0x21。
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] = { 0x21U };

        status =
            prvWriteCommand(
                ILI9341_CMD_POWER_CONTROL_1,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Power Control 2：0xC1
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] = { 0x12U };

        status =
            prvWriteCommand(
                ILI9341_CMD_POWER_CONTROL_2,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * VCOM Control 1：0xC5
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x32U,
            0x3CU
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_VCOM_CONTROL_1,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * VCOM Control 2：0xC7
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] = { 0xC1U };

        status =
            prvWriteCommand(
                ILI9341_CMD_VCOM_CONTROL_2,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Pixel Format：0x3A
     *
     * 0x55 = 16 bit / Pixel，即 RGB565。
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] = { 0x55U };

        status =
            prvWriteCommand(
                ILI9341_CMD_PIXFMT,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Frame Rate Control：0xB1
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x00U,
            0x18U
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_FRAME_RATE_CONTROL,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Display Function Control：0xB6
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x0AU,
            0xA2U
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_DISPLAY_FUNCTION_CONTROL,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * 3Gamma Function Disable：0xF2
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] = { 0x00U };

        status =
            prvWriteCommand(
                ILI9341_CMD_3GAMMA_DISABLE,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Gamma Curve Selected：0x26
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] = { 0x01U };

        status =
            prvWriteCommand(
                ILI9341_CMD_GAMMA_CURVE,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Positive Gamma Correction：0xE0
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x0FU,
            0x20U,
            0x1EU,
            0x09U,
            0x12U,
            0x0BU,
            0x50U,
            0xBAU,
            0x44U,
            0x09U,
            0x14U,
            0x05U,
            0x23U,
            0x21U,
            0x00U
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_POSITIVE_GAMMA,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    /* ------------------------------------------------------------------------
     * Negative Gamma Correction：0xE1
     * ------------------------------------------------------------------------
     */
    {
        static const uint8_t data[] =
        {
            0x00U,
            0x19U,
            0x19U,
            0x00U,
            0x12U,
            0x07U,
            0x2DU,
            0x28U,
            0x3FU,
            0x02U,
            0x0AU,
            0x08U,
            0x25U,
            0x2DU,
            0x0FU
        };

        status =
            prvWriteCommand(
                ILI9341_CMD_NEGATIVE_GAMMA,
                data,
                (uint32_t)sizeof(data)
            );

        if (status != ILI9341_OK)
        {
            return status;
        }
    }

    return ILI9341_OK;
}


/* ============================================================================
 * 19. Port DMA Callback Bridge
 * ============================================================================
 *
 * Port 层不应该直接知道 GFX / FreeRTOS。
 *
 * 所以 Port Callback 先进入 Core：
 *
 *      Port Event
 *          -> Core Event
 *          -> 更高层 Callback
 *
 * 注意：本函数从 DMA ISR 路径调用。
 */
static void prvPortDMACallback(ILI9341_PortDMAEvent_t event, void *user_context)
{
    ILI9341_DMAEvent_t core_event;

    /*
     * 当前 bridge 不需要使用 Port 传回来的 context。
     *
     * Core 自己保存着真正要转发给上层的 context。
     */
    (void)user_context;

    if (event == ILI9341_PORT_DMA_EVENT_COMPLETE)
    {
        core_event = ILI9341_DMA_EVENT_COMPLETE;
    }
    else
    {
        /*
         * 任何 Port Error Event 都统一映射成 Core DMA ERROR。
         */
        core_event = ILI9341_DMA_EVENT_ERROR;

        /*
         * 记录当前 RAMWR Transaction 已经出现过 DMA Error。
         *
         * 这里只写一个简单 volatile byte，
         * 不在 ISR 中做耗时恢复。
         */
        s_write_failed = 1U;
    }

    /*
     * 如果更高层注册了 Core Callback，
     * 就继续把 Event 传上去。
     */
    if (s_dma_callback != NULL)
    {
        s_dma_callback(
            core_event,
            s_dma_callback_context
        );
    }
}


/* ============================================================================
 * 20. Core 软件状态恢复到“未初始化”默认值
 * ============================================================================
 */
static void prvResetCoreState(void)
{
    s_core_state = ILI9341_CORE_STATE_UNINITIALIZED;

    s_width = ILI9341_NATIVE_WIDTH;

    s_height = ILI9341_NATIVE_HEIGHT;

    s_rotation = ILI9341_ROTATION_PORTRAIT;

    prvResetWriteSession();
}


/* ============================================================================
 * 21. Init 失败统一清理
 * ============================================================================
 */
static ILI9341_Status_t prvInitFailCleanup(ILI9341_Status_t failure_status)
{
    /* 初始化失败时先关闭背光。 */
    ILI9341_Port_BacklightOff();

    /* 不再让 Port ISR 向 Core 转发 Callback。 */
    ILI9341_Port_SetDMACallback(NULL, NULL);

    /* 尽量释放 Port 硬件。 */
    (void)ILI9341_Port_DeInit();

    /* Core 自己也恢复成未初始化状态。 */
    prvResetCoreState();

    /*
     * 返回真正导致初始化失败的原始 Core Error，
     * 不让后续 cleanup 结果覆盖它。
     */
    return failure_status;
}


/* ============================================================================
 * 22. Core Init
 * ============================================================================
 */
ILI9341_Status_t ILI9341_Init(void)
{
    ILI9341_PortStatus_t port_status;

    ILI9341_Status_t status;

    /*
     * 已经 READY：
     * 重复 Init 直接视为成功。
     */
    if (s_core_state == ILI9341_CORE_STATE_READY)
    {
        return ILI9341_OK;
    }

    /*
     * 当前有未结束的写事务：
     * 不允许用 Init 粗暴覆盖现有状态。
     */
    if (s_core_state == ILI9341_CORE_STATE_WRITE_ACTIVE)
    {
        return ILI9341_ERROR_BUSY;
    }

    /*
     * FAULT 状态明确要求：
     *
     *      DeInit()
     *          -> Init()
     *
     * 而不是原地再次 Init。
     */
    if (s_core_state == ILI9341_CORE_STATE_FAULT)
    {
        return ILI9341_ERROR_HW;
    }

    /*
     * ------------------------------------------------------------------------
     * 第一步：初始化底层 Port。
     * ------------------------------------------------------------------------
     */
    port_status = ILI9341_Port_Init();

    if (port_status != ILI9341_PORT_OK)
    {
        return prvMapPortStatus(port_status);
    }

    /*
     * ------------------------------------------------------------------------
     * 第二步：建立 DMA Callback Bridge。
     * ------------------------------------------------------------------------
     *
     * 以后：
     *
     *      DMA2 Stream3 IRQ
     *          -> Port Callback
     *          -> prvPortDMACallback()
     *          -> Core Callback
     */
    ILI9341_Port_SetDMACallback(
        prvPortDMACallback,
        NULL
    );

    /*
     * ------------------------------------------------------------------------
     * 第三步：初始化过程中保持背光关闭。
     * ------------------------------------------------------------------------
     */
    ILI9341_Port_BacklightOff();

    /*
     * ------------------------------------------------------------------------
     * 第四步：Software Reset。
     * ------------------------------------------------------------------------
     *
     * 当前硬件没有独立可控 LCD RESET GPIO，
     * 所以使用 ILI9341 Command 0x01。
     */
    status =
        prvWriteCommand(
            ILI9341_CMD_SWRESET,
            NULL,
            0UL
        );

    if (status != ILI9341_OK)
    {
        return prvInitFailCleanup(status);
    }

    /*
     * 保持旧工程已经验证过的 50 ms Software Reset 等待。
     */
    ILI9341_Port_DelayMs(50UL);

    /*
     * ------------------------------------------------------------------------
     * 第五步：执行 Controller 参数初始化序列。
     * ------------------------------------------------------------------------
     */
    status = prvRunInitSequence();

    if (status != ILI9341_OK)
    {
        return prvInitFailCleanup(status);
    }

    /*
     * ------------------------------------------------------------------------
     * 第六步：Sleep Out（0x11）。
     * ------------------------------------------------------------------------
     */
    status =
        prvWriteCommand(
            ILI9341_CMD_SLPOUT,
            NULL,
            0UL
        );

    if (status != ILI9341_OK)
    {
        return prvInitFailCleanup(status);
    }

    /*
     * ILI9341 从 Sleep Out 到下一阶段需要较长稳定时间。
     * 保持旧工程已经验证过的 120 ms。
     */
    ILI9341_Port_DelayMs(120UL);

    /*
     * ------------------------------------------------------------------------
     * 第七步：设置默认显示方向。
     * ------------------------------------------------------------------------
     *
     * 默认保持旧工程 Mode 0：
     *
     *      竖屏
     *      240 x 320
     *      MADCTL = 0x08
     */
    status =
        prvApplyRotation(
            ILI9341_ROTATION_PORTRAIT
        );

    if (status != ILI9341_OK)
    {
        return prvInitFailCleanup(status);
    }

    /*
     * ------------------------------------------------------------------------
     * 第八步：Display ON（0x29）。
     * ------------------------------------------------------------------------
     *
     * 注意：
     * LCD Controller 显示功能已经打开，
     * 但是物理背光仍然保持关闭。
     */
    status =
        prvWriteCommand(
            ILI9341_CMD_DISPLAY_ON,
            NULL,
            0UL
        );

    if (status != ILI9341_OK)
    {
        return prvInitFailCleanup(status);
    }

    /*
     * 到这里 Controller 已经真正完成初始化。
     *
     * 先让 Core 进入 READY，
     * 后面的 Clear() 才能使用标准公开数据路径。
     */
    s_core_state = ILI9341_CORE_STATE_READY;

    /*
     * ------------------------------------------------------------------------
     * 第九步：背光仍然关闭时，先清成白屏。
     * ------------------------------------------------------------------------
     *
     * 这样可以避免用户看到上电后 GRAM 中的随机花屏内容。
     */
    status = ILI9341_Clear(ILI9341_INIT_CLEAR_COLOR);

    if (status != ILI9341_OK)
    {
        return prvInitFailCleanup(status);
    }

    /*
     * ------------------------------------------------------------------------
     * 第十步：所有初始化和清屏都成功以后，最后打开背光。
     * ------------------------------------------------------------------------
     */
    ILI9341_Port_BacklightOn();

    return ILI9341_OK;
}


/* ============================================================================
 * 23. Core DeInit
 * ============================================================================
 */
ILI9341_Status_t ILI9341_DeInit(void)
{
    ILI9341_PortStatus_t port_status;

    /*
     * Core 本来就没初始化：
     * DeInit 设计成幂等操作，直接成功。
     */
    if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
    {
        return ILI9341_OK;
    }

    /*
     * DeInit 一开始先关闭物理背光。
     */
    ILI9341_Port_BacklightOff();

    /*
     * 防止 Port 在 DeInit 过程中继续把 IRQ Event 转发到上层。
     */
    ILI9341_Port_SetDMACallback(NULL, NULL);

    /*
     * Port_DeInit() 自己负责：
     *
     *      Disable IRQ
     *      Abort DMA
     *      Disable SPI
     *      Release GPIO
     */
    port_status = ILI9341_Port_DeInit();

    /*
     * 不论 Port_DeInit 最终返回什么，
     * Core 软件状态都必须结束当前生命周期。
     */
    prvResetCoreState();

    return prvMapPortStatus(port_status);
}


/* ============================================================================
 * 24. Display ON
 * ============================================================================
 */
ILI9341_Status_t ILI9341_DisplayOn(void)
{
    ILI9341_Status_t status;

    status = prvRequireReady();

    if (status != ILI9341_OK)
    {
        return status;
    }

    return
        prvWriteCommand(
            ILI9341_CMD_DISPLAY_ON,
            NULL,
            0UL
        );
}


/* ============================================================================
 * 25. Display OFF
 * ============================================================================
 */
ILI9341_Status_t ILI9341_DisplayOff(void)
{
    ILI9341_Status_t status;

    status = prvRequireReady();

    if (status != ILI9341_OK)
    {
        return status;
    }

    return
        prvWriteCommand(
            ILI9341_CMD_DISPLAY_OFF,
            NULL,
            0UL
        );
}


/* ============================================================================
 * 26. Backlight ON
 * ============================================================================
 */
ILI9341_Status_t ILI9341_BacklightOn(void)
{
    ILI9341_Status_t status;

    status = prvRequireReady();

    if (status != ILI9341_OK)
    {
        return status;
    }

    ILI9341_Port_BacklightOn();

    return ILI9341_OK;
}


/* ============================================================================
 * 27. Backlight OFF
 * ============================================================================
 */
ILI9341_Status_t ILI9341_BacklightOff(void)
{
    /*
     * 如果连 Port/Core 都没有初始化，
     * 就没有必要访问硬件。
     */
    if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    /*
     * 与 BacklightOn 不同：
     *
     * 即使 Core 进入 FAULT，
     * 关闭背光仍然是一个安全动作。
     */
    ILI9341_Port_BacklightOff();

    return ILI9341_OK;
}


/* ============================================================================
 * 28. Set Rotation
 * ============================================================================
 */
ILI9341_Status_t ILI9341_SetRotation(ILI9341_Rotation_t rotation)
{
    ILI9341_Status_t status;

    status = prvRequireReady();

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * 先真正写 MADCTL，
     * 成功后 prvApplyRotation() 会同步新的逻辑 width / height。
     */
    status = prvApplyRotation(rotation);

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * 旋转完成以后，
     * 顺便把 Address Window 恢复为当前逻辑全屏范围。
     *
     * 这样软件尺寸与 Controller 窗口保持一致。
     */
    status =
        ILI9341_SetAddressWindow(
            0U,
            0U,
            s_width,
            s_height
        );

    return status;
}


/* ============================================================================
 * 29. Get Rotation
 * ============================================================================
 */
ILI9341_Rotation_t ILI9341_GetRotation(void)
{
    return s_rotation;
}


/* ============================================================================
 * 30. Get Width
 * ============================================================================
 */
uint16_t ILI9341_GetWidth(void)
{
    return s_width;
}


/* ============================================================================
 * 31. Get Height
 * ============================================================================
 */
uint16_t ILI9341_GetHeight(void)
{
    return s_height;
}


/* ============================================================================
 * 32. Public Set Address Window
 * ============================================================================
 */
ILI9341_Status_t ILI9341_SetAddressWindow(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height
)
{
    ILI9341_Status_t status;

    /*
     * 独立设置 Window 只能在 READY 状态进行。
     *
     * 如果已经 BeginWrite，
     * 不能中途偷偷改掉当前 RAMWR 的 Window。
     */
    status = prvRequireReady();

    if (status != ILI9341_OK)
    {
        return status;
    }

    status =
        prvValidateArea(
            x,
            y,
            width,
            height
        );

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * CASET + PASET 可以放在同一次 CS Low Transaction 中。
     */
    ILI9341_Port_Select();

    status =
        prvSetWindowSelected(
            x,
            y,
            width,
            height
        );

    ILI9341_Port_Unselect();

    return status;
}


/* ============================================================================
 * 33. Begin RAMWR Transaction
 * ============================================================================
 */
ILI9341_Status_t ILI9341_BeginWrite(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height
)
{
    ILI9341_Status_t status;

    uint32_t pixel_count;

    uint8_t command;

    ILI9341_PortStatus_t port_status;

    /*
     * 一笔新的 RAMWR Transaction 只能从 READY 开始。
     */
    status = prvRequireReady();

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * Port 自己如果仍然报告 DMA Busy，
     * 说明上下两层状态不一致或上一笔硬件事务尚未彻底结束。
     */
    if (ILI9341_Port_IsDMABusy() != 0U)
    {
        return ILI9341_ERROR_BUSY;
    }

    /*
     * 检查目标区域合法性。
     */
    status =
        prvValidateArea(
            x,
            y,
            width,
            height
        );

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * 计算该 Window 最多容纳多少 RGB565 Byte。
     *
     * 最大值：
     *
     *      320 * 240 * 2 = 153600
     *
     * 远小于 uint32_t 上限。
     */
    pixel_count =
        (uint32_t)width *
        (uint32_t)height;

    prvResetWriteSession();

    s_write_limit_bytes =
        pixel_count * 2UL;

    /*
     * ------------------------------------------------------------------------
     * 从这里开始 CS 保持 Low，直到 EndWrite / AbortWrite。
     * ------------------------------------------------------------------------
     */
    ILI9341_Port_Select();

    /*
     * 先发送 CASET + PASET。
     */
    status =
        prvSetWindowSelected(
            x,
            y,
            width,
            height
        );

    if (status != ILI9341_OK)
    {
        ILI9341_Port_Unselect();
        prvResetWriteSession();
        return status;
    }

    /*
     * 然后发送 RAMWR(0x2C)。
     *
     * 这里故意不调用 prvWriteCommand()，
     * 因为那个函数会自动 Unselect。
     */
    command = ILI9341_CMD_RAMWR;

    ILI9341_Port_CommandMode();

    port_status =
        ILI9341_Port_WriteBlocking(
            &command,
            1UL
        );

    if (port_status != ILI9341_PORT_OK)
    {
        ILI9341_Port_Unselect();
        prvResetWriteSession();
        return prvMapPortStatusAndLatchFault(port_status);
    }

    /*
     * RAMWR 命令完成后立即切回 Data Mode。
     *
     * 之后 Blocking / DMA 发出的所有 Byte
     * 都会被 ILI9341 当成像素数据。
     */
    ILI9341_Port_DataMode();

    /*
     * 最后才真正宣布：
     * 当前 Core 已进入 RAMWR Transaction。
     */
    s_core_state = ILI9341_CORE_STATE_WRITE_ACTIVE;

    return ILI9341_OK;
}


/* ============================================================================
 * 34. Blocking Write Raw Byte Stream
 * ============================================================================
 */
ILI9341_Status_t ILI9341_WriteBytesBlocking(const uint8_t *data, uint32_t length)
{
    uint32_t remaining_bytes;

    ILI9341_PortStatus_t port_status;

    /*
     * data 必须有效，长度不能是 0。
     */
    if ((data == NULL) || (length == 0UL))
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * 只能在 BeginWrite() 以后调用。
     */
    if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    if (s_core_state == ILI9341_CORE_STATE_FAULT)
    {
        return ILI9341_ERROR_HW;
    }

    if (s_core_state != ILI9341_CORE_STATE_WRITE_ACTIVE)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * 当前事务已经发生 DMA Error 后，
     * 不再继续向同一 RAMWR Transaction 追加数据。
     */
    if (s_write_failed != 0U)
    {
        return ILI9341_ERROR_DMA;
    }

    /*
     * 如果还有一笔 DMA 没 Finalize，
     * CPU 不允许和 DMA 同时操作 SPI 数据流。
     */
    if (
        (s_dma_pending_bytes != 0UL) ||
        (ILI9341_Port_IsDMABusy() != 0U)
    )
    {
        return ILI9341_ERROR_BUSY;
    }

    /*
     * 防止本次写入超过 BeginWrite() 所声明的 Window 容量。
     */
    remaining_bytes =
        s_write_limit_bytes -
        s_write_committed_bytes;

    if (length > remaining_bytes)
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * 真正把这段 Byte Stream 交给 Port Blocking SPI。
     */
    port_status =
        ILI9341_Port_WriteBlocking(
            data,
            length
        );

    if (port_status != ILI9341_PORT_OK)
    {
        return prvMapPortStatusAndLatchFault(port_status);
    }

    /*
     * Blocking 函数返回 OK 时，
     * 最后一个 bit 已经经过 Port 的 BSY 检查。
     *
     * 所以这段 Byte 可以直接计入 committed。
     */
    s_write_committed_bytes += length;

    return ILI9341_OK;
}


/* ============================================================================
 * 35. Blocking Write uint16_t RGB565 Pixels
 * ============================================================================
 */
ILI9341_Status_t ILI9341_WritePixelsBlocking(const uint16_t *pixels, uint32_t pixel_count)
{
    uint8_t byte_buffer[ILI9341_CONVERT_CHUNK_PIXELS * 2UL];

    uint32_t pixel_index;
    uint32_t chunk_pixels;
    uint32_t chunk_index;

    uint16_t color;

    ILI9341_Status_t status;

    if ((pixels == NULL) || (pixel_count == 0UL))
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * 先做一次整体容量检查。
     *
     * 使用除法形式，避免 pixel_count * 2 先发生溢出。
     */
    if (s_core_state != ILI9341_CORE_STATE_WRITE_ACTIVE)
    {
        if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
        {
            return ILI9341_ERROR_NOT_INITIALIZED;
        }

        if (s_core_state == ILI9341_CORE_STATE_FAULT)
        {
            return ILI9341_ERROR_HW;
        }

        return ILI9341_ERROR_STATE;
    }

    if (
        pixel_count >
        (
            (s_write_limit_bytes - s_write_committed_bytes) /
            2UL
        )
    )
    {
        return ILI9341_ERROR_PARAM;
    }

    pixel_index = 0UL;

    while (pixel_index < pixel_count)
    {
        /*
         * 本轮最多转换 32 Pixel。
         */
        chunk_pixels =
            pixel_count -
            pixel_index;

        if (chunk_pixels > ILI9341_CONVERT_CHUNK_PIXELS)
        {
            chunk_pixels = ILI9341_CONVERT_CHUNK_PIXELS;
        }

        /*
         * STM32F407 是小端 CPU。
         *
         * 不能简单把 uint16_t* 强转成 uint8_t* 直接发送，
         * 否则：
         *
         *      0xF800
         *
         * 内存中通常是：
         *
         *      00 F8
         *
         * 但 ILI9341 线上要的是：
         *
         *      F8 00
         *
         * 所以这里明确转换成 high-byte first。
         */
        for (chunk_index = 0UL;
             chunk_index < chunk_pixels;
             ++chunk_index)
        {
            color =
                pixels[
                    pixel_index +
                    chunk_index
                ];

            byte_buffer[chunk_index * 2UL] =
                (uint8_t)(color >> 8);

            byte_buffer[
                chunk_index * 2UL +
                1UL
            ] =
                (uint8_t)(color & 0x00FFU);
        }

        /*
         * 复用已经写好的 Raw Byte Blocking API。
         *
         * 这样：
         *
         *      Busy 检查
         *      Window 容量检查
         *      committed 统计
         *
         * 都只保留一套逻辑。
         */
        status =
            ILI9341_WriteBytesBlocking(
                byte_buffer,
                chunk_pixels * 2UL
            );

        if (status != ILI9341_OK)
        {
            return status;
        }

        pixel_index += chunk_pixels;
    }

    return ILI9341_OK;
}


/* ============================================================================
 * 36. Blocking Repeat One RGB565 Color
 * ============================================================================
 */
ILI9341_Status_t ILI9341_WriteColorRepeatBlocking(uint16_t color, uint32_t pixel_count)
{
    uint8_t byte_buffer[ILI9341_CONVERT_CHUNK_PIXELS * 2UL];

    uint8_t high_byte;
    uint8_t low_byte;

    uint32_t index;
    uint32_t remaining_pixels;
    uint32_t chunk_pixels;

    ILI9341_Status_t status;

    if (pixel_count == 0UL)
    {
        return ILI9341_ERROR_PARAM;
    }

    if (s_core_state != ILI9341_CORE_STATE_WRITE_ACTIVE)
    {
        if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
        {
            return ILI9341_ERROR_NOT_INITIALIZED;
        }

        if (s_core_state == ILI9341_CORE_STATE_FAULT)
        {
            return ILI9341_ERROR_HW;
        }

        return ILI9341_ERROR_STATE;
    }

    /*
     * 先检查整个 pixel_count 是否还能放入当前 Window。
     */
    if (
        pixel_count >
        (
            (s_write_limit_bytes - s_write_committed_bytes) /
            2UL
        )
    )
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * RGB565 拆成 ILI9341 线上 high-byte / low-byte。
     */
    high_byte = (uint8_t)(color >> 8);
    low_byte = (uint8_t)(color & 0x00FFU);

    /*
     * 先一次性把 32 Pixel 的重复 Pattern 准备好。
     */
    for (index = 0UL;
         index < ILI9341_CONVERT_CHUNK_PIXELS;
         ++index)
    {
        byte_buffer[index * 2UL] = high_byte;

        byte_buffer[
            index * 2UL +
            1UL
        ] = low_byte;
    }

    remaining_pixels = pixel_count;

    while (remaining_pixels > 0UL)
    {
        chunk_pixels = remaining_pixels;

        if (chunk_pixels > ILI9341_CONVERT_CHUNK_PIXELS)
        {
            chunk_pixels = ILI9341_CONVERT_CHUNK_PIXELS;
        }

        status =
            ILI9341_WriteBytesBlocking(
                byte_buffer,
                chunk_pixels * 2UL
            );

        if (status != ILI9341_OK)
        {
            return status;
        }

        remaining_pixels -= chunk_pixels;
    }

    return ILI9341_OK;
}


/* ============================================================================
 * 37. DMA Start Raw Byte Stream
 * ============================================================================
 */
ILI9341_Status_t ILI9341_WriteBytesDMAStart(const uint8_t *data, uint32_t length)
{
    uint32_t remaining_bytes;
    uint32_t dma_max;

    ILI9341_PortStatus_t port_status;

    if ((data == NULL) || (length == 0UL))
    {
        return ILI9341_ERROR_PARAM;
    }

    if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    if (s_core_state == ILI9341_CORE_STATE_FAULT)
    {
        return ILI9341_ERROR_HW;
    }

    if (s_core_state != ILI9341_CORE_STATE_WRITE_ACTIVE)
    {
        return ILI9341_ERROR_STATE;
    }

    if (s_write_failed != 0U)
    {
        return ILI9341_ERROR_DMA;
    }

    /*
     * 一次只能存在一笔尚未 Finalize 的 DMA。
     */
    if (
        (s_dma_pending_bytes != 0UL) ||
        (ILI9341_Port_IsDMABusy() != 0U)
    )
    {
        return ILI9341_ERROR_BUSY;
    }

    /*
     * 先检查 Controller Window 剩余容量。
     */
    remaining_bytes =
        s_write_limit_bytes -
        s_write_committed_bytes;

    if (length > remaining_bytes)
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * 再检查单笔 DMA 硬件上限。
     */
    dma_max = ILI9341_Port_GetDMAMaxTransfer();

    if (length > dma_max)
    {
        return ILI9341_ERROR_PARAM;
    }

    /*
     * 真正启动异步 DMA。
     */
    port_status =
        ILI9341_Port_DMAStart(
            data,
            length
        );

    if (port_status != ILI9341_PORT_OK)
    {
        return prvMapPortStatusAndLatchFault(port_status);
    }

    /*
     * 注意：
     *
     * 这里不能立刻把 length 加到 committed。
     *
     * 因为 DMA 只是“开始了”，
     * 并没有被 DMAFinalize() 最终确认成功。
     *
     * 所以先进入 pending。
     */
    s_dma_pending_bytes = length;

    return ILI9341_OK;
}


/* ============================================================================
 * 38. Core DMA Finalize
 * ============================================================================
 */
ILI9341_Status_t ILI9341_DMAFinalize(void)
{
    ILI9341_PortStatus_t port_status;

    ILI9341_Status_t status;

    if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    if (s_core_state == ILI9341_CORE_STATE_FAULT)
    {
        return ILI9341_ERROR_HW;
    }

    if (s_core_state != ILI9341_CORE_STATE_WRITE_ACTIVE)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * Core 根本没有记录一笔待 Finalize DMA：
     * 调用顺序错误。
     */
    if (s_dma_pending_bytes == 0UL)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * Port Finalize 会区分：
     *
     *      ACTIVE   -> BUSY
     *      COMPLETE -> 真正等 TXE / BSY 并收尾
     *      ERROR    -> 清理错误现场并返回 ERROR_DMA
     */
    port_status = ILI9341_Port_DMAFinalize();

    /*
     * DMA 仍然 ACTIVE：
     * pending 必须保留，等待以后再次 Finalize。
     */
    if (port_status == ILI9341_PORT_ERROR_BUSY)
    {
        return ILI9341_ERROR_BUSY;
    }

    /*
     * 正常完成：
     * pending 才正式进入 committed。
     */
    if (port_status == ILI9341_PORT_OK)
    {
        s_write_committed_bytes +=
            s_dma_pending_bytes;

        s_dma_pending_bytes = 0UL;

        return ILI9341_OK;
    }

    /*
     * DMA Error：
     *
     * Port 已经做完错误清理，
     * 但这笔数据不能计入 committed。
     */
    if (port_status == ILI9341_PORT_ERROR_DMA)
    {
        s_dma_pending_bytes = 0UL;

        s_write_failed = 1U;

        return ILI9341_ERROR_DMA;
    }

    /*
     * 其他错误：
     * 先清 pending，避免上层把这一块误认为还可以正常继续。
     */
    s_dma_pending_bytes = 0UL;

    status =
        prvMapPortStatusAndLatchFault(
            port_status
        );

    return status;
}


/* ============================================================================
 * 39. End RAMWR Transaction
 * ============================================================================
 */
ILI9341_Status_t ILI9341_EndWrite(void)
{
    ILI9341_Status_t final_status;

    if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    if (s_core_state == ILI9341_CORE_STATE_FAULT)
    {
        return ILI9341_ERROR_HW;
    }

    if (s_core_state != ILI9341_CORE_STATE_WRITE_ACTIVE)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * 还有 DMA pending，或者 Port 仍然 Busy：
     * 不能拉高 CS。
     *
     * 否则可能在 SPI 还发最后几个 bit 时把 Transaction 截断。
     */
    if (
        (s_dma_pending_bytes != 0UL) ||
        (ILI9341_Port_IsDMABusy() != 0U)
    )
    {
        return ILI9341_ERROR_BUSY;
    }

    /*
     * RGB565 = 2 Byte / Pixel。
     *
     * 如果总 committed Byte 数为奇数，
     * 说明当前最后一个 Pixel 只写了一半。
     *
     * 这种状态不能被当成一个正常完成的 RAMWR Transaction。
     */
    if ((s_write_committed_bytes & 1UL) != 0UL)
    {
        return ILI9341_ERROR_STATE;
    }

    /*
     * 如果中间出现过 DMA Error，
     * 允许这里结束 Transaction、释放 CS，
     * 但最终必须把 ERROR_DMA 告诉上层。
     */
    if (s_write_failed != 0U)
    {
        final_status = ILI9341_ERROR_DMA;
    }
    else
    {
        final_status = ILI9341_OK;
    }

    /*
     * 真正结束当前 LCD RAMWR Transaction。
     */
    ILI9341_Port_Unselect();

    prvResetWriteSession();

    s_core_state = ILI9341_CORE_STATE_READY;

    return final_status;
}


/* ============================================================================
 * 40. Abort RAMWR Transaction
 * ============================================================================
 */
ILI9341_Status_t ILI9341_AbortWrite(void)
{
    ILI9341_PortStatus_t port_status;

    ILI9341_Status_t status;

    ILI9341_CoreState_t state_before_abort;

    if (s_core_state == ILI9341_CORE_STATE_UNINITIALIZED)
    {
        return ILI9341_ERROR_NOT_INITIALIZED;
    }

    /*
     * READY 下 AbortWrite 设计成幂等操作。
     */
    if (s_core_state == ILI9341_CORE_STATE_READY)
    {
        return ILI9341_OK;
    }

    state_before_abort = s_core_state;

    /*
     * 不论当前 Port DMA 是否真的 ACTIVE，
     * Port_DMAAbort() 都设计成：
     *
     *      IDLE -> OK
     *      非 IDLE -> 尝试恢复
     */
    port_status = ILI9341_Port_DMAAbort();

    /*
     * Abort 的语义是“当前 LCD Transaction 不再继续”。
     *
     * 所以不论 Port Abort 最终是否成功，
     * Core 都先释放 LCD CS。
     *
     * 如果底层仍异常，LCD 至少不会继续被选中解释后续异常时钟。
     */
    ILI9341_Port_Unselect();

    prvResetWriteSession();

    status = prvMapPortStatus(port_status);

    if (port_status == ILI9341_PORT_OK)
    {
        /*
         * 如果 Abort 前只是普通 WRITE_ACTIVE，
         * 现在可以重新回到 READY。
         */
        if (state_before_abort == ILI9341_CORE_STATE_WRITE_ACTIVE)
        {
            s_core_state = ILI9341_CORE_STATE_READY;
            return ILI9341_OK;
        }

        /*
         * 如果 Abort 前 Core 已经是 FAULT，
         * 即使这一次 Port_DMAAbort 恰好返回 OK，
         * 也不在这里自动解除 Core FAULT。
         *
         * 仍然要求完整 DeInit -> Init。
         */
        s_core_state = ILI9341_CORE_STATE_FAULT;
        return ILI9341_ERROR_HW;
    }

    /*
     * Port Abort 本身都失败：
     * Core 明确进入 FAULT。
     */
    s_core_state = ILI9341_CORE_STATE_FAULT;

    return status;
}


/* ============================================================================
 * 41. Write One Pixel
 * ============================================================================
 */
ILI9341_Status_t ILI9341_WritePixel(
    uint16_t x,
    uint16_t y,
    uint16_t color
)
{
    ILI9341_Status_t status;

    /*
     * 一个 Pixel 就是一个 1 x 1 Window。
     */
    status =
        ILI9341_BeginWrite(
            x,
            y,
            1U,
            1U
        );

    if (status != ILI9341_OK)
    {
        return status;
    }

    /*
     * 利用“重复颜色”接口写 1 Pixel，
     * 自动完成 RGB565 high-byte first 转换。
     */
    status =
        ILI9341_WriteColorRepeatBlocking(
            color,
            1UL
        );

    if (status != ILI9341_OK)
    {
        (void)ILI9341_AbortWrite();
        return status;
    }

    status = ILI9341_EndWrite();

    return status;
}


/* ============================================================================
 * 42. Clear Screen
 * ============================================================================
 */
ILI9341_Status_t ILI9341_Clear(uint16_t color)
{
    uint32_t pixel_count;

    ILI9341_Status_t status;

    /*
     * 当前逻辑屏幕尺寸可能是：
     *
     *      240 x 320
     * 或：
     *      320 x 240
     *
     * 两者 Pixel 总数都一样，
     * 但这里仍然严格使用当前 s_width / s_height。
     */
    pixel_count =
        (uint32_t)s_width *
        (uint32_t)s_height;

    status =
        ILI9341_BeginWrite(
            0U,
            0U,
            s_width,
            s_height
        );

    if (status != ILI9341_OK)
    {
        return status;
    }

    status =
        ILI9341_WriteColorRepeatBlocking(
            color,
            pixel_count
        );

    if (status != ILI9341_OK)
    {
        (void)ILI9341_AbortWrite();
        return status;
    }

    status = ILI9341_EndWrite();

    return status;
}


/* ============================================================================
 * 43. Is Busy
 * ============================================================================
 */
uint8_t ILI9341_IsBusy(void)
{
    /*
     * READY 只是 Core 软件状态的第一道条件。
     *
     * 同时还要求 Port DMA 真正不忙。
     */
    if (
        (s_core_state == ILI9341_CORE_STATE_READY) &&
        (ILI9341_Port_IsDMABusy() == 0U)
    )
    {
        return 0U;
    }

    return 1U;
}


/* ============================================================================
 * 44. Get DMA Max Transfer
 * ============================================================================
 */
uint32_t ILI9341_GetDMAMaxTransfer(void)
{
    /*
     * Core 不把 STM32F407 的 65535 写死给上层。
     *
     * 具体硬件能力仍然从 Port 查询。
     */
    return ILI9341_Port_GetDMAMaxTransfer();
}


/* ============================================================================
 * 45. Register Core DMA Callback
 * ============================================================================
 */
void ILI9341_SetDMACallback(ILI9341_DMACallback_t callback, void *user_context)
{
    /*
     * Core 只保存更高层 Callback。
     *
     * Port 自己始终注册的是：
     *
     *      prvPortDMACallback()
     *
     * 这样层级不会被破坏。
     */
    s_dma_callback = callback;

    s_dma_callback_context = user_context;
}
