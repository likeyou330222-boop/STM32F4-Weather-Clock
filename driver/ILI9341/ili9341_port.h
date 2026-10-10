#ifndef ILI9341_PORT_H
#define ILI9341_PORT_H





// Port  六类API
// │
// ├── 1. 生命周期
// │     Init
// │     DeInit
// │
// ├── 2. LCD控制线
// │     Select
// │     Unselect
// │     CommandMode
// │     DataMode
// │     Backlight
// │
// ├── 3. 普通SPI
// │     WriteBlocking
// │
// ├── 4. DMA
// │     DMAStart
// │     DMAFinalize
// │     DMAAbort
// │     IsDMABusy
// │     GetDMAMaxTransfer
// │
// ├── 5. 异步事件
// │     SetDMACallback
// │     DMAIRQHandler
// │
// └── 6. 时间
//       DelayMs




/**
 * @file    ili9341_port.h
 * @brief   ILI9341 LCD 驱动的硬件适配层接口
 * ============================================================================
 * 1. 本文件的作用
 * ============================================================================
 * 本文件定义 ILI9341 上层驱动访问底层 MCU 硬件时所需要的统一接口。
 * 上层 ILI9341 驱动不能直接访问：
 *      GPIO
 *      SPI
 *      DMA
 *      NVIC
 * 而必须通过本文件提供的接口访问底层硬件。
 * ============================================================================
 * 2. 为什么需要 Port 层
 * ============================================================================
 * 假设当前 MCU 为 STM32F407：
 *      PA5  -> SPI1_SCK
 *      PA7  -> SPI1_MOSI
 *      PA4  -> LCD_CS
 *      PC5  -> LCD_DC
 *      PB1  -> LCD_BL
 *
 *      SPI1_TX
 *          |
 *          v
 *      DMA2_Stream3
 *      DMA_Channel_3
 *
 * 如果未来更换为 STM32H743，以上硬件资源可能全部变化。
 * 但是 ILI9341 本身：
 *      CASET
 *      PASET
 *      RAMWR
 *      MADCTL
 *      RGB565
 * 这些工作原理并不会因为 MCU 更换而改变。
 *
 * 因此我们把：
 *      “MCU 怎么操作硬件”   和   “ILI9341 怎么工作”   完全分开。
 *
 * ============================================================================
 * 3. 本层负责什么
 * ============================================================================
 * 本层负责向上提供以下硬件能力：
 *      1. LCD 硬件接口初始化
 *      2. LCD 片选 CS 控制
 *      3. LCD 命令/数据 DC 控制
 *      4. LCD 背光控制
 *      5. 阻塞式 SPI 数据发送
 *      6. DMA 异步发送
 *      7. DMA 状态查询
 *      8. DMA 异常终止
 *      9. DMA 完成事件回调
 *     10. 毫秒延时
 * ============================================================================
 * 4. 本层明确不负责什么
 * ============================================================================
 * 本层不负责：
 *      1. ILI9341 寄存器定义
 *      2. ILI9341 初始化命令序列
 *      3. 设置显示区域
 *      4. RGB565 颜色处理
 *      5. 汉字
 *      6. 数字
 *      7. 图片
 *      8. 图形绘制
 *      9. FreeRTOS Queue
 *     10. FreeRTOS Mutex
 *     11. FreeRTOS Task Notification
 * 这些功能属于更高层。
 * ============================================================================
 */


/* ============================================================================
 * 标准头文件
 * ============================================================================
 * 注意： 这里故意不包含任何 STM32 头文件，例如：stm32f4xx.h  stm32f4xx_spi.h   stm32f4xx_dma.h
 * 因为本接口必须保持 MCU 无关。
 */

#include <stdint.h>
#include <stddef.h>

//__cplusplus 是 C++ 编译器自动定义的宏   只有 C++ 编译时才生效  c语言编译时会自动忽略
//如果当前文件是用 C++ 编译器编译的   那么#ifdef __cplusplus条件成立 就会执行extern "C" { 即按照C语言方式进行编译
//原因是  C 和 C++ 对函数名字的处理方式不同。 编译器编译的结果不太一样
#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================================
 * Port 层状态码
 * ============================================================================
 */
/**
 * @brief ILI9341 Port 层函数返回状态
 * 所有可能失败的 Port API 尽量使用统一状态码返回。
 * 好处：上层不需要根据不同函数判断不同的错误表达方式。
 * 例如不要出现：
 *      某函数成功返回 0
 *      某函数成功返回 1
 *      某函数失败返回 -1
 * 我们统一使用这个枚举。
 */
typedef enum
{
    ILI9341_PORT_OK = 0,  //操作成功。

    /**
     * 输入参数非法。
     * 例如：
     *      data == NULL
     *      length == 0
     */
    ILI9341_PORT_ERROR_PARAM,

    /**
     * Port 尚未完成初始化。
     * 例如在 ILI9341_Port_Init() 之前尝试发送数据。
     */
    ILI9341_PORT_ERROR_NOT_INITIALIZED,

    /**
     * 当前硬件正在忙。
     * 典型情况：
     *      DMA 正在发送，
     *      又发起了新的 DMA。
     */
    ILI9341_PORT_ERROR_BUSY,

    /**
     * 操作发生超时。
     * 例如：
     *      SPI TXE 长时间不置位
     *      SPI BSY 长时间不清零
     */
    ILI9341_PORT_ERROR_TIMEOUT,

    ILI9341_PORT_ERROR_DMA,  //DMA 硬件发生错误

    ILI9341_PORT_ERROR_HW  //其他硬件异常

} ILI9341_PortStatus_t;


/* ============================================================================
 * DMA 事件
 * ============================================================================
 */
/**
 * @brief DMA 异步传输产生的事件
 * Port 层完成 DMA ISR 处理以后，通过 callback 把事件通知给上层。
 * 注意： callback 很可能运行在中断上下文。
 * 因此 callback 中禁止：长时间循环   printf 大量日志   普通阻塞 API
 * 后面接入 FreeRTOS 时，我们会在 callback 中使用 FromISR API
 * 唤醒正在等待 DMA 的任务。
 */
typedef enum
{
    /**
     * DMA 已完成内存到 SPI 外设的数据搬运。
     * 注意：DMA_COMPLETE 并不绝对等价于 SPI 的最后一个 bit 已经发送完。
     * DMA 完成之后仍然需要执行 SPI Finalize，
     * 检查 TXE / BSY。
     */
    ILI9341_PORT_DMA_EVENT_COMPLETE = 0,

    /**
     * DMA 传输发生硬件异常。
     * STM32F4 中可能对应：
     *      TE  - Transfer Error
     *      DME - Direct Mode Error
     *      FE  - FIFO Error
     */
    ILI9341_PORT_DMA_EVENT_ERROR

} ILI9341_PortDMAEvent_t;


/* ============================================================================
 * DMA Callback
 * ============================================================================
 */

/**
 * @brief DMA 事件回调函数类型
 * @param event
 *      DMA 事件。
 * @param user_context
 *      用户上下文指针。
 *
 * ============================================================================
 * 为什么要有 user_context
 * ============================================================================
 * 如果只有：callback(event)
 * 那么 callback（回调） 想知道：
 *      谁在等待 DMA？
 *      哪一个对象发起了传输？         就只能依赖全局变量。
 * 增加： void *user_context  以后上层可以传入：
 *      对象指针   TaskHandle    状态结构体
 * Port 层不需要知道它是什么类型。
 * ============================================================================
 * 非常重要
 * ============================================================================
 * 该 callback 可能从 DMA 中断服务函数中调用。
 * 所以后面接 FreeRTOS 时：
 *      可以使用 xxxFromISR()    不能使用普通阻塞 API。
 */
typedef void (*ILI9341_PortDMACallback_t)  //定义一种回调函数指针类型
(
    ILI9341_PortDMAEvent_t event,
    void *user_context
);


/* ============================================================================
 * 初始化接口
 * ============================================================================
 */
/**
 * @brief 初始化 ILI9341 所需要的 MCU 硬件资源
 * 当前 STM32F407 实现中，未来这个函数主要会初始化： GPIO   SPI1  DMA2   NVIC
 * 以及设置： CS  DC  BL  的安全默认状态。
 * @return
 *      ILI9341_PORT_OK  初始化成功
 *其他状态   初始化失败
 */
ILI9341_PortStatus_t ILI9341_Port_Init(void);  //ILI9341_PortStatus_t是函数的返回值类型  此处就相当于是声明


/**
 * @brief 反初始化 ILI9341 Port
 * 用于：  关闭 SPI   关闭 DMA    恢复 GPIO   清理内部状态
 * 当前项目可能暂时用不到，但工程接口建议预留。
 * 例如未来：   Bootloader   低功耗   动态关闭显示模块
 * 都可能使用。
 */
ILI9341_PortStatus_t ILI9341_Port_DeInit(void);


/* ============================================================================
 * LCD 片选 CS
 * ============================================================================
 */
/**
 * @brief 选中 LCD
 * 当前硬件中：  CS = Low   表示 ILI9341 被选中。
 * 为什么不用： CS_LOW()
 * 而使用：  Select()
 * 因为 Select 表示“行为语义”，而不是具体电平。 如果未来硬件电路发生变化，上层代码无需修改。
 */
void ILI9341_Port_Select(void);


/**
 * @brief 取消选中 LCD
 * 当前硬件中： CS = High
 * 表示 SPI Transaction 结束。
 */
void ILI9341_Port_Unselect(void);


/* ============================================================================
 * LCD D/C 控制
 * ============================================================================
 */
/**
 * @brief 将 LCD 接口切换到 Command 模式
 * 当前 ILI9341： DC = 0
 * SPI 接下来发送的 Byte 会被解释为命令。
 * 例如：    0x2A   0x2B    0x2C
 */
void ILI9341_Port_CommandMode(void);


/**
 * @brief 将 LCD 接口切换到 Data 模式
 * 当前 ILI9341：   DC = 1
 * SPI 接下来发送的 Byte 会被解释为参数或像素数据。
 */
void ILI9341_Port_DataMode(void);


/* ============================================================================
 * 背光
 * ============================================================================
 */
/**
 * @brief 打开 LCD 背光
 */
void ILI9341_Port_BacklightOn(void);


/**
 * @brief 关闭 LCD 背光
 */
void ILI9341_Port_BacklightOff(void);




/* ============================================================================
 * 阻塞式发送
 * ============================================================================
 */
/**
 * @brief 通过 SPI 阻塞发送一段 Byte 数据
 * @param data
 *      待发送数据首地址。
 * @param length
 *      数据长度，单位： Byte
 * @return
 *      ILI9341_PORT_OK      ILI9341_PORT_ERROR_PARAM 
 * ILI9341_PORT_ERROR_BUSY     ILI9341_PORT_ERROR_TIMEOUT
 * ============================================================================
 * 为什么不是 WriteByte()
 * ============================================================================
 * 我们不把底层核心接口设计成： WriteByte(uint8_t value)
 * 而是：   WriteBlocking(data, length)
 * 因为 LCD 经常一次需要发送多个参数  如果只能 WriteByte：   上层需要频繁调用函数。
 * Buffer 接口更加通用。
 * ============================================================================
 * 注意
 * ============================================================================
 * 本函数：   不操作 CS   不操作 DC
 * 谁调用它，谁负责提前设置：   Select()   CommandMode()或   DataMode()
 * 这样我们就可以保持一整个 LCD Transaction 的连续性。
 */
ILI9341_PortStatus_t ILI9341_Port_WriteBlocking(const uint8_t *data, uint32_t length);


/* ============================================================================
 * DMA 异步发送
 * ============================================================================
 */
/**
 * @brief 启动一次 DMA 异步 SPI 发送
 * @param data
 *      数据首地址。
 * @param length
 *      本次 DMA 数据长度，单位 Byte。
 * @return
 *      ILI9341_PORT_OK
 *          DMA 已成功启动。
 *
 *      ILI9341_PORT_ERROR_PARAM
 *          data / length 非法，或者 length 超过硬件上限。
 *
 *      ILI9341_PORT_ERROR_NOT_INITIALIZED
 *          Port 尚未初始化。
 *
 *      ILI9341_PORT_ERROR_BUSY
 *          上一笔 DMA 尚未完全结束。
 *
 *      ILI9341_PORT_ERROR_TIMEOUT
 *          DMA Stream 无法在规定时间内进入期望状态。
 *
 * ============================================================================
 * 非常重要
 * ============================================================================
 * 这是“异步启动”。
 * 函数返回：   ILI9341_PORT_OK
 * 只表示    DMA 已经成功开始工作。
 * 不代表：  所有像素已经到达 LCD。
 * DMA 完成后： 
 *      DMA IRQ
 *          |
 *          v
 *      Port ISR
 *          |
 *          v
 *      callback
 * 上层再继续执行后续操作。
 * ============================================================================
 * 本函数不负责什么
 * ============================================================================
 * 本函数不负责自动分片。
 * 如果硬件最大 DMA 长度为：   65535 Byte
 * 那么：   153600 Byte  应该由上一层拆成：  65535    65535    22530
 * Port 每次只处理“一笔真实 DMA Transaction”。
 */
ILI9341_PortStatus_t ILI9341_Port_DMAStart(
    const uint8_t *data,
    uint32_t length
);


/**
 * @brief 对一次已完成的 DMA SPI 传输进行最终收尾
 * 为什么 DMA 完成后还需要 Finalize：
 *  DMA Transfer Complete 只说明：   内存中的最后一个 Byte已经被 DMA 搬入 SPI 外设。
 * 但是 SPI 内部移位寄存器可能还在发送最后几个 bit。因此这里需要进一步确认：  TXE == SET
 *以及：   BSY == RESET之后才能认为：   SPI 物理传输真正结束。
 * 这一点尤其重要，因为只有在真正结束以后， 上层才可以安全： CS High  或者切换下一笔 Transaction。
 */
ILI9341_PortStatus_t ILI9341_Port_DMAFinalize(void);


/**
 * @brief 强制终止当前 DMA 发送
 * 主要用于    timeout   DMA error   系统恢复
 * Abort 后 Port 必须尽量恢复到可再次传输的状态。
 */
ILI9341_PortStatus_t ILI9341_Port_DMAAbort(void);


/**
 * @brief 查询当前 Port 是否允许开始新的 DMA Transaction
 *
 * @return
 *      0
 *          当前状态为 IDLE，可以开始新 DMA。
 *
 *      非 0
 *          当前存在未完成事务、待清理错误或 FAULT，
 *          不允许启动新的 DMA。
 */
uint8_t ILI9341_Port_IsDMABusy(void);


/**
 * @brief 获取当前硬件一次 DMA 所支持的最大传输长度
 * @return
 *      最大传输长度，单位 Byte。
 * 例如 STM32F407： NDTR = 16 bit
 * 因此通常：  65535 Byte
 * 为什么不直接在上层写死 65535：  因为未来换 MCU 后：   DMA Counter可能不同。
 * 上层 Transport 可以通过这个 API 自动决定如何分片。
 */
uint32_t ILI9341_Port_GetDMAMaxTransfer(void);


/* ============================================================================
 * DMA Callback
 * ============================================================================
 */

/**
 * @brief 注册 DMA 事件回调
 * @param callback
 *      DMA 事件回调函数。
 * @param user_context
 *      用户自定义上下文。
 * 后面 FreeRTOS 层可以注册：   ILI9341_RTOS_DMACallback()
 * DMA 完成以后：
 *      Port IRQ
 *          |
 *          v
 *      callback
 *          |
 *          v
 *      vTaskNotifyGiveFromISR()
 * 从而唤醒正在等待 DMA 的 LCD Task。
 */
void ILI9341_Port_SetDMACallback(
    ILI9341_PortDMACallback_t callback,
    void *user_context
);


/* ============================================================================
 * DMA IRQ
 * ============================================================================
 */
/**
 * @brief ILI9341 DMA 中断处理入口
 * MCU 真正的中断函数以后可以写得非常薄：
 * @code
 * void DMA2_Stream3_IRQHandler(void)
 * {
 *     ILI9341_Port_DMAIRQHandler();
 * }
 * @endcode
 * 好处：  stm32f4xx_it.c不需要塞入大量 LCD 驱动逻辑。
 * 所有 DMA 状态处理仍然留在 Port 层。
 */
void ILI9341_Port_DMAIRQHandler(void);


/* ============================================================================
 * Delay
 * ============================================================================
 */
/**
 * @brief 毫秒延时
 * @param ms
 *      延时时间，单位毫秒。
 * ILI9341 初始化过程中会存在：
 *      Software Reset
 *          |
 *          v
 *      Delay
 *
 *      Sleep Out
 *          |
 *          v
 *      Delay
 *
 * 等时序要求。
 * Core 层不应该直接调用：
 *      HAL_Delay()
 *      delay_ms()
 *      vTaskDelay()
 * 因为这些都是平台相关 API。
 * Core 层只调用：    ILI9341_Port_DelayMs()
 * 后面我们还会继续讨论：
 * 在 FreeRTOS Scheduler 启动前和启动后，
 * Delay 应该怎么实现才比较合理。
 */
void ILI9341_Port_DelayMs(uint32_t ms);


#ifdef __cplusplus
}
#endif

#endif /* ILI9341_PORT_H */

