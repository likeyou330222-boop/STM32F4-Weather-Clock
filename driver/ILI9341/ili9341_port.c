/**
 * @file    ili9341_port.c
 * @brief   ILI9341 LCD 驱动的 STM32F407 硬件适配层实现
 * ============================================================================
 * 一、文件定位
 * ============================================================================
 * 本文件是整个 ILI9341 驱动中，唯一应该直接接触 STM32F407 硬件资源的核心文件之一。
 * 本文件负责：
 *      GPIO
 *      SPI1
 *      DMA2_Stream3
 *      DMA_Channel_3
 *      NVIC
 *      DWT Delay
 * 上层：
 *      ili9341.c
 *      ili9341_gfx.c
 *      ili9341_freertos.c
 * 不应该直接操作：  GPIOA   SPI1  DMA2_Stream3
 * 而应该统一通过 ili9341_port.h 中定义的接口访问硬件。
 * ============================================================================
 * 二、Port 层的核心职责
 * ============================================================================
 * Port 层只负责：  “如何把 Byte 从 STM32F407 可靠地发送给 LCD。”
 * Port 层不知道：
 *      0x2A 是 CASET    0x2B 是 PASET    0x2C 是 RAMWR
 *      RGB565 是什么   汉字是什么   图片是什么
 * 上述内容属于更高层。
 * ============================================================================
 * 三、当前硬件连接
 * ============================================================================
 *      STM32F407                  ILI9341
 *      PA5 / SPI1_SCK    ------> SCK
 *      PA7 / SPI1_MOSI   ------> MOSI
 *      PA4               ------> CS
 *      PC5               ------> DC
 *      PB1               ------> BL
 * SPI1 TX DMA：
 *      SPI1_TX
 *          |
 *          v
 *      DMA2
 *          |
 *          v
 *      Stream3
 *          |
 *          v
 *      Channel3
 * ============================================================================
 * 四、关于 FreeRTOS
 * ============================================================================
 * 本文件故意不包含：    FreeRTOS.h   task.h  semphr.h
 * DMA完成以后，本层只产生 Callback。
 * FreeRTOS 层以后可以在 Callback 中通过：  vTaskNotifyGiveFromISR()  唤醒任务。
 * 这样 Port 层仍然保持 RTOS 无关。
 * ============================================================================
 */


//                     ili9341_port.c
//                            │
//         ┌──────────────────┼─────────────────┐
//         │                  │                 │
//         ▼                  ▼                 ▼
//       GPIO                SPI               DMA
//         │                  │                 │
//         │                  │                 │
//  CS / DC / BL         SCK / MOSI       DMA2_Stream3
//                                              │
//                                              ▼
//                                          DMA IRQ
//                                              │
//                                              ▼
//                                          Callback


#include "ili9341_port.h"
/* ============================================================================
 * STM32F4 Standard Peripheral Library
 * ============================================================================
 * 注意：这些 STM32 专有头文件只出现在 .c 实现文件中。
 * ili9341_port.h 中故意不包含它们，以保证 Port 接口本身不绑定 STM32F4。
 */

#include "stm32f4xx.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_spi.h"
#include "stm32f4xx_dma.h"
#include "misc.h"

/* ============================================================================
 * 1. 当前 STM32F407 硬件适配配置
 * ============================================================================
 * 如果以后：  仍然使用 STM32F407，但是 PCB 引脚发生变化，大部分情况下只需要修改这一块。
 * 如果以后：  STM32F407 -> STM32H743那么应该重新实现 ili9341_port.c，而不是去修改 ili9341.c。
 */

/* --------------------------------------------------------------------------
 * SPI1
 * --------------------------------------------------------------------------
 */
/* LCD 使用 SPI1。 */
#define ILI9341_PORT_SPI                     SPI1
/* SPI1 外设时钟位于 APB2。 */
#define ILI9341_PORT_SPI_RCC                 RCC_APB2Periph_SPI1
/* --------------------------------------------------------------------------
 * SPI GPIO
 * --------------------------------------------------------------------------
 */
/* SPI1：
 * PA5 -> SCK
 * PA7 -> MOSI
 */
#define ILI9341_PORT_SPI_GPIO                GPIOA
#define ILI9341_PORT_SPI_GPIO_RCC            RCC_AHB1Periph_GPIOA

#define ILI9341_PORT_SPI_SCK_PIN             GPIO_Pin_5       //配置 GPIO 本身
#define ILI9341_PORT_SPI_SCK_SOURCE          GPIO_PinSource5  //配置 GPIO 的复用功能

#define ILI9341_PORT_SPI_MOSI_PIN            GPIO_Pin_7
#define ILI9341_PORT_SPI_MOSI_SOURCE         GPIO_PinSource7

#define ILI9341_PORT_SPI_AF                  GPIO_AF_SPI1


/* --------------------------------------------------------------------------
 * CS
 * --------------------------------------------------------------------------
 */
/* PA4 -> LCD CS
 * 当前 LCD 的 CS 为低电平有效：
 *      CS = 0 -> LCD 被选中
 *      CS = 1 -> LCD 未选中
 */
#define ILI9341_PORT_CS_GPIO                 GPIOA
#define ILI9341_PORT_CS_GPIO_RCC             RCC_AHB1Periph_GPIOA
#define ILI9341_PORT_CS_PIN                  GPIO_Pin_4


/* --------------------------------------------------------------------------
 * DC
 * --------------------------------------------------------------------------
 */
/*PC5 -> LCD DC
 * 当前 ILI9341：
 *      DC = 0 -> Command
 *      DC = 1 -> Data
 */
#define ILI9341_PORT_DC_GPIO                 GPIOC
#define ILI9341_PORT_DC_GPIO_RCC             RCC_AHB1Periph_GPIOC
#define ILI9341_PORT_DC_PIN                  GPIO_Pin_5


/* --------------------------------------------------------------------------
 * Backlight
 * --------------------------------------------------------------------------
 */
/* PB1 -> LCD BL
 * 当前硬件：
 *      BL = 1 -> 背光打开
 *      BL = 0 -> 背光关闭
 */
#define ILI9341_PORT_BL_GPIO                 GPIOB
#define ILI9341_PORT_BL_GPIO_RCC             RCC_AHB1Periph_GPIOB
#define ILI9341_PORT_BL_PIN                  GPIO_Pin_1


/* ============================================================================
 * 2. SPI 参数
 * ============================================================================
 */
/*保留当前已经验证过能够正常驱动 LCD 的 SPI 模式：
 *      CPOL = 1    CPHA = 1
 即通常所说的：    SPI Mode 3
 */
#define ILI9341_PORT_SPI_CPOL                SPI_CPOL_High
#define ILI9341_PORT_SPI_CPHA                SPI_CPHA_2Edge


/*
 * 当前 SPI1 时钟分频：   SPI_CLK = SPI1 Peripheral Clock / 2
 * 注意：真正 SPI 时钟是多少 MHz， 还取决于系统 APB2 时钟。
 */
#define ILI9341_PORT_SPI_PRESCALER           SPI_BaudRatePrescaler_2


/* ============================================================================
 * 3. DMA 参数
 * ============================================================================
 */
/*
 * STM32F407 SPI1_TX：   DMA2   Stream3   Channel3
 */
#define ILI9341_PORT_DMA                     DMA2

#define ILI9341_PORT_DMA_RCC                 RCC_AHB1Periph_DMA2

#define ILI9341_PORT_DMA_STREAM              DMA2_Stream3

#define ILI9341_PORT_DMA_CHANNEL             DMA_Channel_3

#define ILI9341_PORT_DMA_IRQn                DMA2_Stream3_IRQn   //DMA 中断号


/*
 * STM32F4 DMA NDTR 为 16 bit。
 * 因此：    最大传输计数 = 0xFFFF = 65535
 * 当前 SPI/DMA 单位为 Byte，所以最大就是：  65535 Byte
 */
#define ILI9341_PORT_DMA_MAX_TRANSFER        65535UL


/* ============================================================================
 * 4. DMA Stream3 对应的 Flag（状态标志位）
 * ============================================================================
 * STM32F4 的 DMA flag 与 Stream 绑定。
 * Stream3 对应：
 *      FEIF3
 *      DMEIF3
 *      TEIF3
 *      HTIF3
 *      TCIF3
 */
//‘\’告诉预处理器：这一行还没结束，下一行仍然属于这个宏。
#define ILI9341_PORT_DMA_ALL_FLAGS           \
    (DMA_FLAG_FEIF3  |                       \
     DMA_FLAG_DMEIF3 |                       \
     DMA_FLAG_TEIF3  |                       \
     DMA_FLAG_HTIF3  |                       \
     DMA_FLAG_TCIF3)

/* ============================================================================
 * 5. DMA IRQ 优先级（DMA 发出的中断请求的优先级）
 * ============================================================================
 * 后面 FreeRTOS 层的 Callback 可能会调用：  xxxFromISR()
 * 所以这个中断优先级必须处于 FreeRTOS 允许调用系统 API 的范围。
 * 当前工程默认使用优先级：    6
 * 注意：NVIC Priority Group 属于整个系统级配置。
 * 不建议驱动偷偷修改整个系统的 Priority Group。
 * 推荐在 board / system 初始化阶段统一：    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
 */

#define ILI9341_PORT_DMA_IRQ_PREEMPT_PRIORITY    6U    // U 代表无符号整型
#define ILI9341_PORT_DMA_IRQ_SUB_PRIORITY        0U


/* ============================================================================
 * 6. 硬件等待超时
 * ============================================================================
 * Blocking SPI、DMA Abort 等硬件轮询操作不能无限 while。
 * 如果硬件异常： SPI BSY 永远不清零   无限 while 会导致系统彻底卡死。
 * 因此设置一个硬件级保护超时。
 * 这里使用：   100 ms
 * 这个超时不是 FreeRTOS timeout，而是底层硬件死锁保护。
 */

#define ILI9341_PORT_HW_TIMEOUT_US            100000UL   //  UL  代表 无符号 long 类型


/* ============================================================================
 * 7. Port 内部 DMA 状态机
 * ============================================================================
 * 注意：我们没有只使用：  uint8_t dma_busy;
 * 因为 DMA 完成以后还有一个非常重要的阶段：
 *      DMA Complete
 *          !=
 *      SPI Complete
 * 所以我们至少需要区分：
 *      IDLE
 *      ACTIVE
 *      COMPLETE
 *      ERROR
 */
typedef enum
{
    /*
     * 当前没有 DMA Transaction。
     *
     * 可以启动新的 DMA。
     */
    ILI9341_PORT_DMA_STATE_IDLE = 0,

    /*
     * DMA 正在搬运数据。
     */
    ILI9341_PORT_DMA_STATE_ACTIVE,

    /*
     * DMA 已经完成，
     * 但是还没有执行 DMAFinalize()。
     *
     * 此时 SPI 最后几个 bit 可能仍然在发送。
     */
    ILI9341_PORT_DMA_STATE_COMPLETE,

    /*
     * DMA 发生异常。
     */
    ILI9341_PORT_DMA_STATE_ERROR,


    /*
     * 硬件恢复本身失败。
     * 例如：   DMA Stream 无法关闭；  SPI BSY 长时间不清零。
     *
     * 此时不能再认为 Port 可以直接使用，
     * 通常需要重新初始化。
     */
    ILI9341_PORT_DMA_STATE_FAULT

} ILI9341_PortDMAState_t;


/* ============================================================================
 * 8. Port 内部状态变量
 * ============================================================================
 */
/*
 * Port 是否已经初始化。
 * volatile：虽然当前主要由任务/普通上下文访问，但 Port 与 ISR 存在共享状态，因此明确声明为 volatile。
 */
static volatile uint8_t s_port_initialized = 0U;


/*
 * 当前 DMA 状态。
 * ISR 会修改这个变量，
 * 普通上下文也会读取。
 */
static volatile ILI9341_PortDMAState_t s_dma_state =  ILI9341_PORT_DMA_STATE_IDLE;



static ILI9341_PortStatus_t prvWaitDMAStreamEnabled(void);



/*
 * 上层注册的 DMA Callback。
 */
static ILI9341_PortDMACallback_t s_dma_callback = NULL;


/*
 * Callback 对应的用户上下文。
 */
static void *s_dma_callback_context = NULL;


/*
 * DMA_Init() 初始化时需要一个合法的内存地址。
 * 真正传输开始时：   M0AR会被重新设置成真实 buffer。
 */
static uint8_t s_dma_dummy_byte = 0U;


/* ============================================================================
 * 9. 内部辅助函数声明
 * ============================================================================
 * static：表示这些函数只允许在本 ili9341_port.c 内部使用。 不向其他模块暴露。
 */

static void prvDWTInit(void);

static uint32_t prvGetTimeoutCycles(uint32_t timeout_us);

static ILI9341_PortStatus_t prvWaitSPIFlag(uint16_t flag, FlagStatus expected_state);

static ILI9341_PortStatus_t prvWaitDMAStreamDisabled(void);

static void prvClearDMAFlags(void);

static void prvGPIOInit(void);

static void prvSPICoreInit(void);

static ILI9341_PortStatus_t prvDMAInit(void);

static void prvGPIODeInit(void);

static void prvNotifyDMAEvent(ILI9341_PortDMAEvent_t event);


/* ============================================================================
 * 10. DWT 初始化
 * ============================================================================
 */
/**
 * @brief 打开 Cortex-M4 DWT Cycle Counter
 * DWT CYCCNT 是 Cortex-M4 内核自带的 CPU 周期计数器。
 * 我们使用它实现：  微秒级硬件超时   毫秒 Delay
 * 最大好处：   不占用 SysTick
 * 因此不会和以后 FreeRTOS 的 SysTick 发生直接冲突。
 */
static void prvDWTInit(void)
{
    /*想用 DWT 这个硬件计数器，必须先把 Cortex-M 内核里的 Trace 功能总开关打开。
     * CoreDebug DEMCR 中的 TRCENA：Trace Enable。
     * DWT 模块属于 CoreSight Trace 系统，因此必须先打开这一位。
     */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;// |=  按位或之后，再赋值回去。

    /*
     * 打开 DWT CYCCNT。打开 DWT 的 CPU 周期计数功能。
     * 注意： 这里不把 CYCCNT 强制清零。
     * 原因： 其他模块可能也在使用 CYCCNT。
     * 我们通过：  current - start计算时间差， 所以完全不需要依赖计数器从 0 开始。
     */
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}


/* ============================================================================
 * 11. timeout_us 转换成 CPU Cycle
 * ============================================================================
 */
static uint32_t prvGetTimeoutCycles(uint32_t timeout_us)
{
    uint32_t cycles_per_us;
    /*
     * SystemCoreClock：表示当前 Cortex-M4 内核时钟频率。
     * 例如：   168 MHz
     * 那么：   1 us = 168 Cycle
     */
    cycles_per_us = SystemCoreClock / 1000000UL;

    /*
     * 理论上 STM32F407 不可能低于 1 MHz。
     * 这里仍然做保护，
     * 防止 SystemCoreClock 配置异常后得到 0。
     */
    if (cycles_per_us == 0UL)
    {
        cycles_per_us = 1UL;
    }

    return cycles_per_us * timeout_us;
}


/* ============================================================================
 * 12. 等待 SPI Flag
 * ============================================================================
 */
/**
 * @brief 等待 SPI Flag 达到指定状态
 * 例如：  等 TXE == SET  或   等 BSY == RESET
 * 为什么单独封装：  不希望代码中到处复制：
 *      while (...)
 *      {
 *          ...
 *      }
 *
 * 同时统一增加超时保护。
 */
static ILI9341_PortStatus_t prvWaitSPIFlag(uint16_t flag, FlagStatus expected_state)
{
    uint32_t start_cycle;
    uint32_t timeout_cycles;

    /*
     * 记录进入等待前的 CPU Cycle。
     */
    start_cycle = DWT->CYCCNT;

    /*
     * 将硬件超时时间转换为 CPU Cycle。
     */
    timeout_cycles =
        prvGetTimeoutCycles(ILI9341_PORT_HW_TIMEOUT_US);

    /*
     * 如果当前 Flag 还没有达到期望状态，
     * 就继续等待。
     */
    while (
        SPI_I2S_GetFlagStatus(
            ILI9341_PORT_SPI,
            flag
        ) != expected_state
    )
    {
        /*
         * uint32_t 无符号减法天然支持 CYCCNT 溢出回绕。
         * 例如：
         *      start   = 0xFFFFFFF0
         *      current = 0x00000020
         * unsigned subtraction 仍然可以得到正确时间差。
         */
        if (
            (uint32_t)(DWT->CYCCNT - start_cycle)
            >= timeout_cycles
        )
        {
            return ILI9341_PORT_ERROR_TIMEOUT;
        }
    }

    return ILI9341_PORT_OK;
}




static ILI9341_PortStatus_t prvWaitDMAStreamEnabled(void)
{
    uint32_t start_cycle;
    uint32_t timeout_cycles;


    start_cycle =
        DWT->CYCCNT;


    timeout_cycles =
        prvGetTimeoutCycles(
            ILI9341_PORT_HW_TIMEOUT_US
        );


    /*
     * 等待 DMA Stream 的 EN bit
     * 真正进入 ENABLE 状态。
     */
    while (
        DMA_GetCmdStatus(
            ILI9341_PORT_DMA_STREAM
        ) != ENABLE
    )
    {
        if (
            (uint32_t)(
                DWT->CYCCNT -
                start_cycle
            ) >= timeout_cycles
        )
        {
            return
                ILI9341_PORT_ERROR_TIMEOUT;
        }
    }


    return ILI9341_PORT_OK;
}





/* ============================================================================
 * 13. 等待 DMA Stream 真正关闭
 * ============================================================================
 */
static ILI9341_PortStatus_t prvWaitDMAStreamDisabled(void)
{
    uint32_t start_cycle;
    uint32_t timeout_cycles;

    start_cycle = DWT->CYCCNT;

    timeout_cycles =
        prvGetTimeoutCycles(ILI9341_PORT_HW_TIMEOUT_US);

    /*
     * STM32F4 DMA Stream 被 Disable 后，EN bit 不一定在当前 CPU 指令执行完的一瞬间立刻清零。
     * 所以重新配置：   M0AR   NDTR  之前必须确认 Stream 已经真正 Disabled。
     */
    while (
        DMA_GetCmdStatus(
            ILI9341_PORT_DMA_STREAM
        ) != DISABLE
    )
    {
        if (
            (uint32_t)(DWT->CYCCNT - start_cycle)
            >= timeout_cycles
        )
        {
            return ILI9341_PORT_ERROR_TIMEOUT;
        }
    }

    return ILI9341_PORT_OK;
}


/* ============================================================================
 * 14. 清理 DMA Flag
 * ============================================================================
 */
static void prvClearDMAFlags(void)
{
    /*
     * 启动新 DMA 之前必须把上一笔留下来的 Flag 清掉。
     * 否则可能出现：
     *      DMA 刚启动
     *          |
     *          v
     *      旧 TC Flag 仍然存在
     *          |
     *          v
     *      立即误进 DMA Complete IRQ
     */
    DMA_ClearFlag(
        ILI9341_PORT_DMA_STREAM,
        ILI9341_PORT_DMA_ALL_FLAGS
    );
}


/* ============================================================================
 * 15. GPIO 初始化  prv 可以理解成：private  也就是：私有内部函数。
 * ============================================================================
 */
static void prvGPIOInit(void)
{
    GPIO_InitTypeDef gpio;
    /*
     * ------------------------------------------------------------------------
     * 第一步：打开 GPIO 时钟
     * ------------------------------------------------------------------------
     * SPI1 SCK/MOSI：   GPIOA
     * CS： GPIOA
     * DC： GPIOC
     * BL： GPIOB
     */
    RCC_AHB1PeriphClockCmd(
        ILI9341_PORT_SPI_GPIO_RCC |
        ILI9341_PORT_CS_GPIO_RCC  |
        ILI9341_PORT_DC_GPIO_RCC  |
        ILI9341_PORT_BL_GPIO_RCC,
        ENABLE
    );


    /*
     * ------------------------------------------------------------------------
     * 第二步：配置 SPI1 SCK + MOSI
     * ------------------------------------------------------------------------
     */
    GPIO_StructInit(&gpio);
    /*
     * 同时配置：   PA5   PA7
     */
    gpio.GPIO_Pin =
        ILI9341_PORT_SPI_SCK_PIN |
        ILI9341_PORT_SPI_MOSI_PIN;

    /*
     * AF = Alternate Function。
     * 表示 PA5 / PA7 不再由普通 GPIO 控制，
     * 而是交给 SPI1 外设。
     */
    gpio.GPIO_Mode = GPIO_Mode_AF;

    /*
     * 推挽输出。
     */
    gpio.GPIO_OType = GPIO_OType_PP;

    /*
     * 高速 GPIO。
     * SPI 时钟较高，所以设置成高速。
     */
    gpio.GPIO_Speed = GPIO_Speed_100MHz;

    /*
     * 当前保持原工程使用方式。
     */
    gpio.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(
        ILI9341_PORT_SPI_GPIO,
        &gpio
    );


    /*
     * GPIO 设置为 AF 模式只是说明：  “我要使用复用功能。”  还需要明确告诉 STM32：  “具体复用到 SPI1。”
     */

    GPIO_PinAFConfig(
        ILI9341_PORT_SPI_GPIO,
        ILI9341_PORT_SPI_SCK_SOURCE,
        ILI9341_PORT_SPI_AF
    );

    GPIO_PinAFConfig(
        ILI9341_PORT_SPI_GPIO,
        ILI9341_PORT_SPI_MOSI_SOURCE,
        ILI9341_PORT_SPI_AF
    );


    /*
     * ------------------------------------------------------------------------
     * 第三步：配置 CS
     * ------------------------------------------------------------------------
     */

    GPIO_StructInit(&gpio);

    gpio.GPIO_Pin = ILI9341_PORT_CS_PIN;

    gpio.GPIO_Mode = GPIO_Mode_OUT;

    gpio.GPIO_OType = GPIO_OType_PP;

    gpio.GPIO_Speed = GPIO_Speed_100MHz;

    gpio.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(
        ILI9341_PORT_CS_GPIO,
        &gpio
    );


    /*
     * ------------------------------------------------------------------------
     * 第四步：配置 DC
     * ------------------------------------------------------------------------
     */

    GPIO_StructInit(&gpio);

    gpio.GPIO_Pin = ILI9341_PORT_DC_PIN;

    gpio.GPIO_Mode = GPIO_Mode_OUT;

    gpio.GPIO_OType = GPIO_OType_PP;

    gpio.GPIO_Speed = GPIO_Speed_100MHz;

    gpio.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(
        ILI9341_PORT_DC_GPIO,
        &gpio
    );


    /*
     * ------------------------------------------------------------------------
     * 第五步：配置背光
     * ------------------------------------------------------------------------
     */

    GPIO_StructInit(&gpio);

    gpio.GPIO_Pin = ILI9341_PORT_BL_PIN;

    gpio.GPIO_Mode = GPIO_Mode_OUT;

    gpio.GPIO_OType = GPIO_OType_PP;

    gpio.GPIO_Speed = GPIO_Speed_100MHz;

    gpio.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(
        ILI9341_PORT_BL_GPIO,
        &gpio
    );


    /*
     * ------------------------------------------------------------------------
     * 第六步：设置安全默认电平
     * ------------------------------------------------------------------------
     * LCD 初始化以前：   CS = High  防止 SPI 初始化过程中产生的信号被 LCD 当成有效命令。
     */
    ILI9341_Port_Unselect();


    /*
     * 默认进入 Data Mode。
     *
     * 真正发命令时，
     * 上层会显式调用：
     *
     *      ILI9341_Port_CommandMode()
     */
    ILI9341_Port_DataMode();


    /*
     * 初始化过程中先关闭背光。
     * 原因：ILI9341 GRAM 在刚上电时可能包含随机内容。
     * 如果立即打开背光，用户可能会看到：   花屏   白屏闪烁   随机颜色
     * 正确做法通常是：
     *      初始化 Controller
     *          |
     *      清屏
     *          |
     *      最后打开背光
     */
    ILI9341_Port_BacklightOff();
}


/* ============================================================================
 * 16. SPI 初始化
 * ============================================================================
 */

static void prvSPICoreInit(void)
{
    SPI_InitTypeDef spi;

    /*
     * SPI1 位于 APB2。
     * 使用 SPI1 之前必须先打开外设时钟。
     */
    RCC_APB2PeriphClockCmd(
        ILI9341_PORT_SPI_RCC,
        ENABLE
    );


    /*
     * 将 SPI1 恢复到初始状态。
     * 这样驱动不依赖：  “之前有没有其他代码修改过 SPI1。”
     */
    SPI_I2S_DeInit(
        ILI9341_PORT_SPI
    );


    /*
     * 获取 SPL 默认配置。
     * 然后只修改我们真正需要的字段。
     */
    SPI_StructInit(&spi);


    /*
     * 1Line Tx：
     * 当前 LCD 只需要：  MCU -> LCD
     * 不使用 MISO。
     */
    spi.SPI_Direction = SPI_Direction_1Line_Tx;


    /*
     * STM32 是 SPI Master。 时钟由 STM32 产生。
     */
    spi.SPI_Mode = SPI_Mode_Master;


    /*
     * SPI 每次传输：   8 bit = 1 Byte   后面的 DMA 也配置为 Byte。
     */
    spi.SPI_DataSize = SPI_DataSize_8b;


    /*
     * SPI Mode 3：
     *      CPOL = 1
     *      CPHA = 1
     */
    spi.SPI_CPOL = ILI9341_PORT_SPI_CPOL;

    spi.SPI_CPHA = ILI9341_PORT_SPI_CPHA;


    /*
     * NSS 使用软件管理。
     * 因为我们的 LCD CS：  PA4是通过普通 GPIO 手动控制。
     */
    spi.SPI_NSS = SPI_NSS_Soft;


    /*
     * SPI 时钟分频。
     */
    spi.SPI_BaudRatePrescaler = ILI9341_PORT_SPI_PRESCALER;


    /*
     * 高位先发送：
     * bit7 -> ... -> bit0
     */
    spi.SPI_FirstBit = SPI_FirstBit_MSB;


    /*
     * 本工程不使用 SPI CRC。
     * SPL 结构体仍要求填写 CRC polynomial。
     */
    spi.SPI_CRCPolynomial = 7U;


    /*
     * 将配置真正写入 SPI1 寄存器。
     */
    SPI_Init(ILI9341_PORT_SPI,&spi);


    /*
     * 半双工单线模式明确指定发送方向。
     */
    SPI_BiDirectionalLineConfig(ILI9341_PORT_SPI, SPI_Direction_Tx);


    /*
     * NSS 使用内部软件方式保持 Master 状态。
     */
    SPI_NSSInternalSoftwareConfig(ILI9341_PORT_SPI, SPI_NSSInternalSoft_Set);


    /*
     * 最后真正打开 SPI1。
     */
    SPI_Cmd(ILI9341_PORT_SPI,ENABLE);
}


/* ============================================================================
 * 17. DMA 初始化
 * ============================================================================
 */
static ILI9341_PortStatus_t prvDMAInit(void)
{
    DMA_InitTypeDef dma;

    NVIC_InitTypeDef nvic;

    ILI9341_PortStatus_t status;


    /*
     * ------------------------------------------------------------------------
     * 第一步：打开 DMA2 时钟
     * ------------------------------------------------------------------------
     */
    RCC_AHB1PeriphClockCmd(ILI9341_PORT_DMA_RCC,ENABLE);


    /*
     * ------------------------------------------------------------------------
     * 第二步：确保 DMA Stream 已经关闭
     * ------------------------------------------------------------------------
     */

    DMA_Cmd(ILI9341_PORT_DMA_STREAM,DISABLE);

    status = prvWaitDMAStreamDisabled();

    if (status != ILI9341_PORT_OK)
    {
        return status;
    }


    /*
     * 把 Stream3 恢复到默认状态。
     */
    DMA_DeInit(ILI9341_PORT_DMA_STREAM);


    /*
     * ------------------------------------------------------------------------
     * 第三步：填写 DMA 配置
     * ------------------------------------------------------------------------
     */

    DMA_StructInit(&dma);


    /*
     * SPI1_TX 对应 DMA Channel3。
     */
    dma.DMA_Channel = ILI9341_PORT_DMA_CHANNEL;


    /*
     * 外设地址固定为：
     *
     *      SPI1->DR
     *
     * DMA 的目标永远是 SPI1 数据寄存器。
     */
    dma.DMA_PeripheralBaseAddr = (uint32_t)&ILI9341_PORT_SPI->DR;


    /*
     * 初始化阶段先提供一个合法的 dummy 地址。
     *
     * 真正 DMAStart 时会动态修改 M0AR。
     */
    dma.DMA_Memory0BaseAddr = (uint32_t)&s_dma_dummy_byte;


    /*
     * 数据方向： Memory -> Peripheral
     * 即：  RAM -> SPI1->DR
     */
    dma.DMA_DIR = DMA_DIR_MemoryToPeripheral;


    /*
     * 初始化时先设置为 1。
     * DMAStart 时会动态设置： NDTR = length
     */
    dma.DMA_BufferSize = 1U;


    /*
     * 外设地址不递增。
     * 每个 Byte 的目标始终都是：  SPI1->DR
     */
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;


    /*
     * 内存地址递增。
     * buffer：
     *      data[0]
     *      data[1]
     *      data[2]
     *      ...
     *
     * 每发送一个 Byte，DMA 自动读取下一个内存地址。
     */
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;


    /*
     * SPI 当前为 8 bit。所以 DMA Peripheral Size：   Byte
     */
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;


    /*
     * 内存数据单位同样：    Byte
     */
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;


    /*
     * LCD 数据发送属于：  一次发送  一次结束 所以使用 Normal Mode。
     * 不使用 Circular。
     */
    dma.DMA_Mode = DMA_Mode_Normal;


    /*
     * LCD 刷屏属于高吞吐数据，
     * DMA Priority 设置为 High。
     * 注意：这是 DMA Controller 内部优先级，不是 NVIC IRQ 优先级。
     */
    dma.DMA_Priority = DMA_Priority_High;


    /*
     * 当前使用 Direct Mode。
     * 对这种简单：   Memory -> SPI   Byte stream 非常合适。
     */
    dma.DMA_FIFOMode = DMA_FIFOMode_Disable;


    /*
     * FIFO Disable 时，Threshold 实际不会起主要作用。这里仍按 SPL 结构体要求给出合法值。
     */
    dma.DMA_FIFOThreshold = DMA_FIFOThreshold_1QuarterFull;


    /*
     * Memory Burst：单次。
     */
    dma.DMA_MemoryBurst = DMA_MemoryBurst_Single;


    /*
     * Peripheral Burst：单次。
     */
    dma.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;


    /*
     * 将配置写入 DMA2 Stream3。
     */
    DMA_Init(ILI9341_PORT_DMA_STREAM, &dma);


    /*
     * ------------------------------------------------------------------------
     * 第四步：清理旧 Flag
     * ------------------------------------------------------------------------
     */

    prvClearDMAFlags();


    /*
     * ------------------------------------------------------------------------
     * 第五步：打开需要的 DMA Interrupt （这些事件发生时，允许 DMA2 Stream3 产生中断请求。）
     * ------------------------------------------------------------------------
     * TC：  Transfer Complete
     * TE：  Transfer Error
     * DME： Direct Mode Error
     * FE：  FIFO Error
     * 为什么不只开 TC：工程代码必须考虑异常路径。
     */
    DMA_ITConfig(
        ILI9341_PORT_DMA_STREAM,
        DMA_IT_TC |
        DMA_IT_TE |
        DMA_IT_DME |
        DMA_IT_FE,
        ENABLE
    );


    /*
     * ------------------------------------------------------------------------
     * 第六步：配置 NVIC
     * ------------------------------------------------------------------------
     */

    nvic.NVIC_IRQChannel = ILI9341_PORT_DMA_IRQn;//指定当前要配置的是“哪一路中断”。

    nvic.NVIC_IRQChannelPreemptionPriority = ILI9341_PORT_DMA_IRQ_PREEMPT_PRIORITY;// 设置该中断的“抢占优先级”。

    nvic.NVIC_IRQChannelSubPriority = ILI9341_PORT_DMA_IRQ_SUB_PRIORITY;// 设置该中断的“子优先级”。

    nvic.NVIC_IRQChannelCmd = ENABLE;//设置是否使能这一路 NVIC 中断。

    NVIC_Init(&nvic);


    /*
     * 初始化阶段不要打开 SPI DMA Request。
     * 真正 DMAStart 时才打开。
     */
    SPI_I2S_DMACmd(ILI9341_PORT_SPI, SPI_I2S_DMAReq_Tx, DISABLE); //关闭 SPI 的“发送 DMA 请求”


    return ILI9341_PORT_OK;
}


/* ============================================================================
 * 18. GPIO DeInit
 * ============================================================================
 *
 * 当前主要给 ILI9341_Port_DeInit() 使用。
 */

static void prvGPIODeInit(void)
{
    GPIO_InitTypeDef gpio;

    /*
     * DeInit 前先把 LCD 放到安全状态：
     *
     *      CS High
     *      Backlight Off
     */
    ILI9341_Port_Unselect();

    ILI9341_Port_BacklightOff();


    /*
     * SPI GPIO 改为普通输入。
     *
     * 这样 Port 释放以后，
     * 不再主动驱动这些引脚。
     */
    GPIO_StructInit(&gpio);

    gpio.GPIO_Pin =
        ILI9341_PORT_SPI_SCK_PIN |
        ILI9341_PORT_SPI_MOSI_PIN;

    gpio.GPIO_Mode = GPIO_Mode_IN;

    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;

    GPIO_Init(ILI9341_PORT_SPI_GPIO, &gpio);


    /*
     * CS。
     */
    GPIO_StructInit(&gpio);

    gpio.GPIO_Pin = ILI9341_PORT_CS_PIN;

    gpio.GPIO_Mode = GPIO_Mode_IN;

    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;

    GPIO_Init(ILI9341_PORT_CS_GPIO, &gpio
    );


    /*
     * DC。
     */
    GPIO_StructInit(&gpio);

    gpio.GPIO_Pin = ILI9341_PORT_DC_PIN;

    gpio.GPIO_Mode = GPIO_Mode_IN;

    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;

    GPIO_Init(ILI9341_PORT_DC_GPIO, &gpio);


    /*
     * BL。
     */
    GPIO_StructInit(&gpio);

    gpio.GPIO_Pin = ILI9341_PORT_BL_PIN;

    gpio.GPIO_Mode = GPIO_Mode_IN;

    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;

    GPIO_Init(ILI9341_PORT_BL_GPIO, &gpio);
}


/* ============================================================================
 * 19. Callback 通知辅助函数
 * ============================================================================
 */

static void prvNotifyDMAEvent(ILI9341_PortDMAEvent_t event)
{
    /*
     * Callback 是可选的。
     * 如果上层没有注册，就什么也不做。
     */
    if (s_dma_callback != NULL)
    {
        /*
         * 注意：本函数可能在 DMA ISR 内被调用。
         * 所以实际 callback 必须遵守 ISR 规则。
         */
        s_dma_callback(event, s_dma_callback_context);
    }
}


/* ============================================================================
 * 20. Port Init
 * ============================================================================
 */

ILI9341_PortStatus_t ILI9341_Port_Init(void)
{
    ILI9341_PortStatus_t status;
    /*
     * Init 设计成幂等操作。
     * 如果已经初始化，再次调用直接返回 OK。
     * 好处：上层不用特别害怕重复初始化。
     */
    if (s_port_initialized != 0U)
    {
            /*
        * Port 虽然曾经完成初始化，
        * 但是现在已经处于 FAULT。
        *
        * 不能再告诉上层“一切正常”。
        *
        * 此时应该：
        *
        *      DeInit()
        *          ↓
        *      Init()
        */
        if (
            s_dma_state ==
            ILI9341_PORT_DMA_STATE_FAULT
        )
        {
            return ILI9341_PORT_ERROR_HW;
        }


        /*
        * 已经初始化且当前不是 FAULT，
        * 重复 Init 视为成功。
        */
        return ILI9341_PORT_OK;

    }
    /*
     * 更新 CMSIS 中保存的 SystemCoreClock。
     * DWT Delay / Timeout 需要知道真实 CPU 时钟。
     */
    SystemCoreClockUpdate();

    /*
     * 初始化 DWT Cycle Counter。
     */
    prvDWTInit();


    /*
     * 先初始化内部软件状态。
     */
    s_dma_state = ILI9341_PORT_DMA_STATE_IDLE;



    /*
     * 初始化 GPIO。
     */
    prvGPIOInit();


    /*
     * 初始化 SPI1。
     */
    prvSPICoreInit();


    /*
     * 初始化 DMA。
     */
    status = prvDMAInit();

    if (status != ILI9341_PORT_OK)
    {
        /*
         * DMA 初始化失败，
         * Port 整体不能认为初始化成功。
         */
        SPI_Cmd(ILI9341_PORT_SPI, DISABLE );

        return status;
    }


    /*
     * 所有硬件准备完毕以后，
     * 最后才把 initialized 置 1。
     * 这样可以避免半初始化状态被上层误认为可用。
     */
    s_port_initialized = 1U;


    return ILI9341_PORT_OK;
}


/* ============================================================================
 * 21. Port DeInit
 * ============================================================================
 */

ILI9341_PortStatus_t
ILI9341_Port_DeInit(void)
{
    ILI9341_PortStatus_t status;


    if (s_port_initialized == 0U)
    {
        return ILI9341_PORT_OK;
    }


    /*
     * ------------------------------------------------------------
     * 1. 首先阻止 DMA ISR 再进入。
     * ------------------------------------------------------------
     */
    NVIC_DisableIRQ(
        ILI9341_PORT_DMA_IRQn
    );


    /*
     * 禁止 DMA 再产生 TC/TE/DME/FE 中断。
     */
    DMA_ITConfig(
        ILI9341_PORT_DMA_STREAM,
        DMA_IT_TC |
        DMA_IT_TE |
        DMA_IT_DME |
        DMA_IT_FE,
        DISABLE
    );


    /*
     * ------------------------------------------------------------
     * 2. 然后同步停止 DMA / SPI。
     *
     * DMAAbort 本身不依赖 IRQ，
     * 所以中断关闭后仍然可以正常 Abort。
     * ------------------------------------------------------------
     */
    status =
        ILI9341_Port_DMAAbort();


    /*
     * 清掉可能残留的 NVIC Pending。
     */
    NVIC_ClearPendingIRQ(
        ILI9341_PORT_DMA_IRQn
    );


    /*
     * 关闭 SPI。
     */
    SPI_Cmd(
        ILI9341_PORT_SPI,
        DISABLE
    );


    /*
     * 释放 GPIO。
     */
    prvGPIODeInit();


    s_dma_state =
        ILI9341_PORT_DMA_STATE_IDLE;

    s_dma_callback =
        NULL;

    s_dma_callback_context =
        NULL;

    s_port_initialized =
        0U;


    return status;
}

/* ============================================================================
 * 22. CS
 * ============================================================================
 */

void ILI9341_Port_Select(void)
{
    /*
     * 当前硬件：  CS = Low表示选中 LCD。
     */
    GPIO_ResetBits(ILI9341_PORT_CS_GPIO, ILI9341_PORT_CS_PIN);
}


void ILI9341_Port_Unselect(void)
{
    /*
     * 当前硬件： CS = High  表示释放 LCD。
     */
    GPIO_SetBits(ILI9341_PORT_CS_GPIO, ILI9341_PORT_CS_PIN);
}


/* ============================================================================
 * 23. DC
 * ============================================================================
 */

void ILI9341_Port_CommandMode(void)
{
    /*
     * ILI9341：  DC = 0
     * 接下来的 SPI Byte 被解释为 Command。
     */
    GPIO_ResetBits(ILI9341_PORT_DC_GPIO, ILI9341_PORT_DC_PIN);
}


void ILI9341_Port_DataMode(void)
{
    /*
     * ILI9341：  DC = 1
     * 接下来的 SPI Byte 被解释为 Data。
     */
    GPIO_SetBits(ILI9341_PORT_DC_GPIO, ILI9341_PORT_DC_PIN);
}


/* ============================================================================
 * 24. Backlight
 * ============================================================================
 */

void ILI9341_Port_BacklightOn(void)
{
    GPIO_SetBits(ILI9341_PORT_BL_GPIO, ILI9341_PORT_BL_PIN);
}


void ILI9341_Port_BacklightOff(void)
{
    GPIO_ResetBits(ILI9341_PORT_BL_GPIO, ILI9341_PORT_BL_PIN);
}


/* ============================================================================
 * 25. Blocking SPI
 * ============================================================================
 */

ILI9341_PortStatus_t ILI9341_Port_WriteBlocking(const uint8_t *data, uint32_t length)
{
    uint32_t index;

    ILI9341_PortStatus_t status;

    /*
     * 参数检查。
     */
    if (
        (data == NULL) ||
        (length == 0UL)
    )
    {
        return ILI9341_PORT_ERROR_PARAM;
    }


    /*
     * Port 必须已经完成初始化。
     */
    if (s_port_initialized == 0U)
    {
        return ILI9341_PORT_ERROR_NOT_INITIALIZED;
    }


    /*
     * DMA Transaction 尚未完全结束时，
     * 不允许 Blocking SPI 与 DMA 同时操作 SPI1。
     * 否则： CPU   DMA  会同时向 SPI1->DR 写数据。
     */
    if (
        s_dma_state !=
        ILI9341_PORT_DMA_STATE_IDLE
    )
    {
        return ILI9341_PORT_ERROR_BUSY;
    }


    /*
     * 按 Byte 顺序发送整个 Buffer。
     */
    for (index = 0UL;
         index < length;
         ++index)
    {
        /*
         * 等待：  TXE = 1
         * 表示发送数据寄存器可以接受新的数据。
         */
        status = prvWaitSPIFlag(SPI_I2S_FLAG_TXE, SET);

        if (status != ILI9341_PORT_OK)
        {
            return status;
        }


        /*
         * 把当前 Byte 写入：  SPI1->DR
         * 然后 SPI 硬件自动通过 MOSI 发送。
         */
        SPI_I2S_SendData(
            ILI9341_PORT_SPI,
            data[index]
        );
    }


    /*
     * 所有 Byte 都已经写入 SPI 发送路径以后，
     * 再确认一次 TXE。
     */
    status =
        prvWaitSPIFlag(
            SPI_I2S_FLAG_TXE,
            SET
        );

    if (status != ILI9341_PORT_OK)
    {
        return status;
    }


    /*
     * 最后等待：  BSY = 0
     * 表示 SPI 最后一个 bit 已经真正离开 MOSI。
     * 注意：我们没有每发送一个 Byte 都等待 BSY，那样会严重降低效率。
     * 正确方式是：
     *      连续灌数据
     *          |
     *      最后统一等 BSY
     */
    status =
        prvWaitSPIFlag(
            SPI_I2S_FLAG_BSY,
            RESET
        );


    return status;
}


/* ============================================================================
 * 26. DMA Start
 * ============================================================================
 */

ILI9341_PortStatus_t ILI9341_Port_DMAStart(const uint8_t *data, uint32_t length)
{
    ILI9341_PortStatus_t status;


    /*
     * Buffer 必须有效。
     */
    if (
        (data == NULL) ||
        (length == 0UL)
    )
    {
        return ILI9341_PORT_ERROR_PARAM;
    }


    /*
     * 当前硬件一次最大只允许： 65535 Byte
     */
    if (
        length >
        ILI9341_PORT_DMA_MAX_TRANSFER
    )
    {
        return ILI9341_PORT_ERROR_PARAM;
    }


    /*
     * Port 必须已经初始化。
     */
    if (s_port_initialized == 0U)
    {
        return ILI9341_PORT_ERROR_NOT_INITIALIZED;
    }


    /*
     * 只有 IDLE 状态才能开始下一笔 DMA。
     *
     * COMPLETE 也不能开始。
     *
     * 因为 COMPLETE 之后：
     *
     *      SPI 可能还没 Finalize。
     */
    if (
        s_dma_state !=
        ILI9341_PORT_DMA_STATE_IDLE
    )
    {
        return ILI9341_PORT_ERROR_BUSY;
    }


    /*
     * 确认 Stream 真的处于 Disable 状态。
     */
    DMA_Cmd(ILI9341_PORT_DMA_STREAM, DISABLE);

    status = prvWaitDMAStreamDisabled();

    if (status != ILI9341_PORT_OK)
    {
        return status;
    }


    /*
     * 开始新的 DMA 前，
     * 清理上一笔的所有 flag。
     */
    prvClearDMAFlags();


    /*
     * ------------------------------------------------------------------------
     * 设置真实内存起始地址
     * ------------------------------------------------------------------------
     * M0AR：   Memory 0 Address Register
     */
    ILI9341_PORT_DMA_STREAM->M0AR =
        (uint32_t)data;


    /*
     * ------------------------------------------------------------------------
     * 设置本次传输长度
     * ------------------------------------------------------------------------
     * NDTR： Number of Data to Transfer
     * 当前单位为 Byte。
     */
    ILI9341_PORT_DMA_STREAM->NDTR = length;


    /*
     * 在真正打开 DMA 前，先把软件状态切换成 ACTIVE。
     * 为什么不是启动 DMA 后再改：DMA 很快。
     * 极端情况下：
     *      DMA 启动
     *      -> 立即完成
     *      -> IRQ
     *
     * 如果软件状态还没更新，
     * ISR 就可能观察到错误状态。
     */

    s_dma_state = ILI9341_PORT_DMA_STATE_ACTIVE;



        /*
     * 最后真正启动 DMA2 Stream3。
     */
    DMA_Cmd( ILI9341_PORT_DMA_STREAM, ENABLE);


        /*
    * 确认 DMA Stream 确实成功 Enable。
    */
    status =
        prvWaitDMAStreamEnabled();

    if (status != ILI9341_PORT_OK)
    {
        /*
        * 连 DMA Stream 都无法启动，
        * 当前硬件状态已经不值得直接继续使用。
        */
        s_dma_state =
            ILI9341_PORT_DMA_STATE_FAULT;

        return status;
    }


    /*
     * 允许 SPI1 TX 产生 DMA Request。
     *
     * SPI每需要一个新的发送Byte时，
     * 会向 DMA 发出 Request。
     */
    SPI_I2S_DMACmd(ILI9341_PORT_SPI, SPI_I2S_DMAReq_Tx, ENABLE);





    /*
     * 到这里立即返回。
     * 注意：
     * return OK 只说明：  DMA成功开始。
     * 完全不代表：   数据已经发送完成。
     */
    return ILI9341_PORT_OK;
}


/* ============================================================================
 * 27. DMA Finalize （给“一次 LCD 的 DMA 发送”做最后收尾）
 * DMA 中断已经告诉我们“完成了”或者“出错了”之后，这个函数再来把 SPI 和 DMA 的尾巴收干净，然后把整个 DMA Transaction 状态恢复成 IDLE。
 * ============================================================================
 */

ILI9341_PortStatus_t
ILI9341_Port_DMAFinalize(void)
{
    ILI9341_PortStatus_t status;


    if (s_port_initialized == 0U)
    {
        return ILI9341_PORT_ERROR_NOT_INITIALIZED;
    }


    /*
     * DMA 还在搬数据。
     *
     * 现在还不能 Finalize。
     */
    if (
        s_dma_state ==
        ILI9341_PORT_DMA_STATE_ACTIVE
    )
    {
        return ILI9341_PORT_ERROR_BUSY;
    }


    /*
     * 已经完全空闲。
     *
     * 再次 Finalize 直接视为成功。
     */
    if (
        s_dma_state ==
        ILI9341_PORT_DMA_STATE_IDLE
    )
    {
        return ILI9341_PORT_OK;
    }


    /*
     * FAULT 状态不能假装进行正常 Finalize。
     *
     * 此时应该通过重新初始化等方式恢复。
     */
    if (
        s_dma_state ==
        ILI9341_PORT_DMA_STATE_FAULT
    )
    {
        return ILI9341_PORT_ERROR_HW;
    }


    /*
     * 不再允许 SPI 产生新的 TX DMA Request。
     */
    SPI_I2S_DMACmd(
        ILI9341_PORT_SPI,
        SPI_I2S_DMAReq_Tx,
        DISABLE
    );


    /*
     * ------------------------------------------------------------
     * 如果之前 DMA IRQ 已经报告 ERROR，
     * 这里主要负责清理硬件。
     * ------------------------------------------------------------
     */
    if (
        s_dma_state ==
        ILI9341_PORT_DMA_STATE_ERROR
    )
    {
        DMA_Cmd(
            ILI9341_PORT_DMA_STREAM,
            DISABLE
        );

        status =
            prvWaitDMAStreamDisabled();

        prvClearDMAFlags();


        if (status != ILI9341_PORT_OK)
        {
            s_dma_state =
                ILI9341_PORT_DMA_STATE_FAULT;

            return status;
        }

         /*
        * DMA 虽然已经停止，
        * 但是之前已经送入 SPI 的最后一个数据
        * 仍然可能正在通过 MOSI 发送。
        *
        * 所以还必须确认 SPI 真正不忙。
        */
        status =
            prvWaitSPIFlag(
                SPI_I2S_FLAG_BSY,
                RESET
            );


            /*
        * SPI 无法恢复为空闲状态，
        * 同样进入 FAULT。
        */
        if (status != ILI9341_PORT_OK)
        {
            s_dma_state =
                ILI9341_PORT_DMA_STATE_FAULT;

            return status;
        }

        /*
         * 错误现场已经清理完成。
         *
         * Port 可以重新使用，
         * 因此回到 IDLE。
         *
         * 但是本次 DMA 仍然是失败的。
         */
        s_dma_state =
            ILI9341_PORT_DMA_STATE_IDLE;

        return ILI9341_PORT_ERROR_DMA;
    }


    /*
     * ------------------------------------------------------------
     * 正常 COMPLETE 路径。
     *
     * 第一步：
     * 等待 TXE。
     * ------------------------------------------------------------
     */
    status =
        prvWaitSPIFlag(
            SPI_I2S_FLAG_TXE,
            SET
        );

    if (status != ILI9341_PORT_OK)
    {
        s_dma_state =
            ILI9341_PORT_DMA_STATE_FAULT;

        return status;
    }


    /*
     * ------------------------------------------------------------
     * 第二步：
     * 等待 BSY 清零。
     *
     * 到这里才表示最后一个 bit
     * 真正完成物理发送。
     * ------------------------------------------------------------
     */
    status =
        prvWaitSPIFlag(
            SPI_I2S_FLAG_BSY,
            RESET
        );

    if (status != ILI9341_PORT_OK)
    {
        s_dma_state =
            ILI9341_PORT_DMA_STATE_FAULT;

        return status;
    }


    /*
     * ------------------------------------------------------------
     * 第三步：
     * 明确关闭 DMA Stream。
     * ------------------------------------------------------------
     */
    DMA_Cmd(
        ILI9341_PORT_DMA_STREAM,
        DISABLE
    );


    status =
        prvWaitDMAStreamDisabled();

    if (status != ILI9341_PORT_OK)
    {
        s_dma_state =
            ILI9341_PORT_DMA_STATE_FAULT;

        return status;
    }


    /*
     * 清掉当前 DMA Transaction 留下的 Flag。
     */
    prvClearDMAFlags();


    /*
     * 所有硬件收尾都成功后，
     * 才真正进入 IDLE。
     */
    s_dma_state =
        ILI9341_PORT_DMA_STATE_IDLE;


    return ILI9341_PORT_OK;
}


/* ============================================================================
 * 28. DMA Abort（强制终止当前 DMA 发送）
 * ============================================================================
 */

ILI9341_PortStatus_t ILI9341_Port_DMAAbort(void)
{
    ILI9341_PortStatus_t status;


    /*
     * Port 没有初始化，不能操作 DMA/SPI。
     */
    if (s_port_initialized == 0U)
    {
        return ILI9341_PORT_ERROR_NOT_INITIALIZED;
    }


    /*
     * 已经 IDLE：不需要做任何事情。
     */
    if (s_dma_state == ILI9341_PORT_DMA_STATE_IDLE)
    {
        return ILI9341_PORT_OK;
    }


    /*
     * 第一步： 禁止 SPI 继续产生 DMA Request。
     */
    SPI_I2S_DMACmd(ILI9341_PORT_SPI, SPI_I2S_DMAReq_Tx, DISABLE);


    /*
     * 第二步：请求 DMA Stream 停止。
     */
    DMA_Cmd( ILI9341_PORT_DMA_STREAM, DISABLE);


    /*
     * 第三步：确认真正停止。
     */
    status = prvWaitDMAStreamDisabled();


    /*
     * 第四步：
     * 不管成功还是失败，都尽量清理 DMA Flag。
     */
    prvClearDMAFlags();


     /*
     * DMA Stream 如果连关闭都失败，
     * 就绝对不能进入 IDLE。
     */
    if (status != ILI9341_PORT_OK)
    {
        s_dma_state = ILI9341_PORT_DMA_STATE_FAULT;

        return status;
    }


    /*
     * DMA 已经停止，
     * 但是之前已经进入 SPI 的数据可能仍在发送。
     *
     * 等待 SPI 真正不忙。
     */
    status = prvWaitSPIFlag(SPI_I2S_FLAG_BSY, RESET);


    /*
     * SPI 长时间无法进入空闲，
     * 同样说明硬件状态已经不可完全信任。
     */
    if (status != ILI9341_PORT_OK)
    {
        s_dma_state = ILI9341_PORT_DMA_STATE_FAULT;

        return status;
    }


    /*
     * DMA 已关闭，
     * SPI 也已经停止工作。
     *
     * 到这里才能真正宣布：
     *
     *      Port 已经重新空闲。
     */
    s_dma_state = ILI9341_PORT_DMA_STATE_IDLE;


    return ILI9341_PORT_OK;
}


/* ============================================================================
 * 29. DMA Busy（查询 DMA 当前是否正在工作）
 * ============================================================================
 */

uint8_t ILI9341_Port_IsDMABusy(void)
{
    /*
     * 注意：COMPLETE 状态这里仍然返回 Busy。
     * 为什么？ 因为在 DMAFinalize() 之前，
     * Transaction 并没有真正结束。
     * 所以：
     *      ACTIVE
     *      COMPLETE
     *      ERROR awaiting cleanup
     * 都不能开启新的 DMA。
     */
    return (s_dma_state == ILI9341_PORT_DMA_STATE_IDLE) ? 0U : 1U;  //DMA 当前是 IDLE 空闲状态就返回0U否则返回1U
}


/* ============================================================================
 * 30. DMA 最大长度
 * ============================================================================
 */

uint32_t ILI9341_Port_GetDMAMaxTransfer(void)
{
    /*
     * 当前 STM32F407：  NDTR = 16 bit
     * 所以： 65535 Byte
     */
    return ILI9341_PORT_DMA_MAX_TRANSFER;
}


/* ============================================================================
 * 31. 注册 DMA Callback
 * ============================================================================
 */

void ILI9341_Port_SetDMACallback(ILI9341_PortDMACallback_t callback, void *user_context)
{
    /*
     * 推荐：
     * 在没有 DMA 正在执行时注册/修改 Callback。
     * 正常工程中通常只会在系统初始化阶段注册一次。
     */
    s_dma_callback = callback;

    s_dma_callback_context = user_context;
}


/* ============================================================================
 * 32. DMA IRQ Handler（ILI9341 DMA 中断处理入口）
 *  DMA 中断发生以后，统一判断“到底是出错了，还是正常传输完成了”，然后更新状态并通知上层。
 *  LCD DMA 驱动里的“中断事件处理中心”
 * ============================================================================
 */

void ILI9341_Port_DMAIRQHandler(void)
{
    uint8_t dma_error = 0U; //定义一个是否错误的标志位 0是时为没有错误

    /*
     * ------------------------------------------------------------------------
     * 先处理 检查 Transfer Error
     * ------------------------------------------------------------------------
     */
    if (DMA_GetITStatus(ILI9341_PORT_DMA_STREAM, DMA_IT_TEIF3) != RESET )
    {
        DMA_ClearITPendingBit(ILI9341_PORT_DMA_STREAM, DMA_IT_TEIF3);  //把这个中断挂起标志清掉。

        dma_error = 1U;
    }


    /*
     * ------------------------------------------------------------------------
     * Direct Mode Error
     * ------------------------------------------------------------------------
     */
    if (DMA_GetITStatus(ILI9341_PORT_DMA_STREAM, DMA_IT_DMEIF3) != RESET) //检查 Direct Mode Error
    {
        DMA_ClearITPendingBit(ILI9341_PORT_DMA_STREAM, DMA_IT_DMEIF3);

        dma_error = 1U;
    }


    /*
     * ------------------------------------------------------------------------
     * FIFO Error
     * ------------------------------------------------------------------------
     */
    if (DMA_GetITStatus(ILI9341_PORT_DMA_STREAM, DMA_IT_FEIF3) != RESET)  //检查 Direct Mode Error
    {
        DMA_ClearITPendingBit(ILI9341_PORT_DMA_STREAM, DMA_IT_FEIF3);

        dma_error = 1U;
    }


    /*
     * 如果任何 Error 出现：Error 的优先级高于 Complete。
     */
    if (dma_error != 0U)
    {
        /*
         * 禁止 SPI 继续向 DMA 申请数据。
         */
        SPI_I2S_DMACmd(ILI9341_PORT_SPI, SPI_I2S_DMAReq_Tx, DISABLE);


        /*
         * 请求 DMA 停止。
         * 注意：ISR 中不做长时间 while 等待。
         */
        DMA_Cmd(ILI9341_PORT_DMA_STREAM, DISABLE);


            /*
        * 非常重要：
        *
        * 当前已经确定本次 DMA 失败。
        * 把 TC / HT / TE / DME / FE 全部清掉，
        * 避免错误情况下残留 TC Flag
        * 随后再次触发 COMPLETE 路径。
        */
        prvClearDMAFlags();



        s_dma_state = ILI9341_PORT_DMA_STATE_ERROR; //state属于是内部的 外部（上层）不需要知道


        /*
         * 通知上层： DMA Error。
         * 以后 FreeRTOS Callback 可以在这里间接：    vTaskNotifyGiveFromISR()
         */
        prvNotifyDMAEvent(ILI9341_PORT_DMA_EVENT_ERROR);


        /*
         * Error 已处理，
         * 不继续处理 TC。
         */
        return;
    }


    /*
     * ------------------------------------------------------------------------
     * Transfer Complete
     * ------------------------------------------------------------------------
     */
    if (
        DMA_GetITStatus(
            ILI9341_PORT_DMA_STREAM,
            DMA_IT_TCIF3
        ) != RESET
    )
    {
        /*
         * 第一件事永远是清 Interrupt Pending Bit。
         */
        DMA_ClearITPendingBit(
            ILI9341_PORT_DMA_STREAM,
            DMA_IT_TCIF3
        );


        /*
         * DMA已经搬运完毕，
         * 不再允许 SPI 产生新的 DMA Request。
         */
        SPI_I2S_DMACmd(
            ILI9341_PORT_SPI,
            SPI_I2S_DMAReq_Tx,
            DISABLE
        );


        /*
         * 注意：
         *
         * 这里只切换成 COMPLETE，
         * 而不是 IDLE。
         *
         * 因为：
         *
         *      DMA Complete
         *          !=
         *      SPI Complete
         *
         * 上层醒来以后还必须：
         *
         *      DMAFinalize()
         */

        s_dma_state =
            ILI9341_PORT_DMA_STATE_COMPLETE;


        /*
         * 通知上层。
         */
        prvNotifyDMAEvent(
            ILI9341_PORT_DMA_EVENT_COMPLETE
        );
    }
}


/* ============================================================================
 * 33. 毫秒 Delay
 * ============================================================================
 */

void ILI9341_Port_DelayMs(uint32_t ms)
{
    uint32_t millisecond;

    uint32_t start_cycle;

    uint32_t cycles_per_ms;


    /*
     * 0 ms：
     *
     * 直接返回。
     */
    if (ms == 0UL)
    {
        return;
    }


    /*
     * 防御性处理：即使调用者在 Port_Init 以前误用了 Delay，这里仍然尝试打开 DWT。
     */
    prvDWTInit();


    /*
     * 计算：  1 ms 对应多少 CPU Cycle。
     *
     * 例如：   SystemCoreClock = 168 MHz
     *
     * 那么：  168000 Cycle / ms
     */
    cycles_per_ms = SystemCoreClock / 1000UL;


    if (cycles_per_ms == 0UL)
    {
        cycles_per_ms = 1UL;
    }


    /*
     * 为什么按 1 ms 一段一段等待：
     *
     * 避免：
     *
     *      ms * cycles_per_ms
     *
     * 在较长 Delay 下发生 uint32_t 乘法溢出。
     */
    for ( millisecond = 0UL; millisecond < ms; ++millisecond)
    {
        start_cycle = DWT->CYCCNT;

        while (
            (uint32_t)(
                DWT->CYCCNT -
                start_cycle
            ) < cycles_per_ms
        )
        {
            /*
             * Busy wait。
             *
             * 不关闭中断。
             *
             * 所以中断仍然可以正常响应。
             */
        }
    }
}




/*
 * DMA2 Stream3 真正的硬件中断入口。
 *
 * SPI1 TX DMA 传输完成或发生 DMA 错误时，
 * Cortex-M4 会通过中断向量调用本函数。
 *
 * 将硬件中断转交给已有的 Port 层处理函数。
 */
void DMA2_Stream3_IRQHandler(void)
{
    ILI9341_Port_DMAIRQHandler();
}


