#include "esp_at_port.h"

#include "stm32f4xx.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_dma.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_usart.h"

#include "misc.h"  //里边有NVIC的函数






/*
 * ============================================================================
 * ESP32-C3 USART 硬件资源配置
 * ============================================================================
 * STM32F407ZG：
 *      USART2_TX -> PA2
 *      USART2_RX -> PA3
 * USART 参数：
 *      Baud Rate : 115200
 *      Data Bits : 8
 *      Stop Bits : 1
 *      Parity    : None
 *      Flow Ctrl : None
 */
/*
 * ============================================================================
 * USART
 * ============================================================================
 */
#define ESP_AT_PORT_USART                      USART2
#define ESP_AT_PORT_USART_RCC                  RCC_APB1Periph_USART2

/*
 * ============================================================================
 * GPIO
 * ============================================================================
 */
#define ESP_AT_PORT_GPIO                       GPIOA
#define ESP_AT_PORT_GPIO_RCC                   RCC_AHB1Periph_GPIOA

/*
 * TX：
 *      STM32 PA2  ->  ESP32-C3 RX
 */
#define ESP_AT_PORT_TX_PIN                     GPIO_Pin_2
#define ESP_AT_PORT_TX_PIN_SOURCE              GPIO_PinSource2

/*
 * RX：
 *      ESP32-C3 TX  ->  STM32 PA3
 */
#define ESP_AT_PORT_RX_PIN                     GPIO_Pin_3
#define ESP_AT_PORT_RX_PIN_SOURCE              GPIO_PinSource3

/*
 * PA2 / PA3 都使用 USART2 Alternate Function。
 */
#define ESP_AT_PORT_GPIO_AF                    GPIO_AF_USART2

/*
 * ============================================================================
 * USART 参数
 * ============================================================================
 */
#define ESP_AT_PORT_BAUD_RATE                  115200U
#define ESP_AT_PORT_WORD_LENGTH                USART_WordLength_8b
#define ESP_AT_PORT_STOP_BITS                  USART_StopBits_1
#define ESP_AT_PORT_PARITY                     USART_Parity_No
#define ESP_AT_PORT_HARDWARE_FLOW_CONTROL      USART_HardwareFlowControl_None
#define ESP_AT_PORT_USART_MODE                 (USART_Mode_Rx | USART_Mode_Tx)


/*
 * ============================================================================
 * USART2 TX DMA 硬件资源
 * ============================================================================
 * STM32F407 的 DMA Request Mapping 决定：
 *      USART2_TX     DMA1 Stream6  Channel4
 */

//DMA1 外设时钟。
#define ESP_AT_PORT_DMA_RCC                   RCC_AHB1Periph_DMA1

//USART2 TX 使用 DMA1 Stream6。
#define ESP_AT_PORT_DMA_TX_STREAM             DMA1_Stream6

//USART2 TX 在 Stream6 上对应 Channel4。
#define ESP_AT_PORT_DMA_TX_CHANNEL            DMA_Channel_4


/*
 * DMA1 Stream6 的所有状态 Flag。
 * 当前初始化阶段只需要知道：
 *      在开始使用 Stream6 之前，把以前可能残留的状态全部清干净。
 */
#define ESP_AT_PORT_DMA_TX_ALL_FLAGS          \
    (DMA_FLAG_FEIF6  |                        \
     DMA_FLAG_DMEIF6 |                        \
     DMA_FLAG_TEIF6  |                        \
     DMA_FLAG_HTIF6  |                        \
     DMA_FLAG_TCIF6)


//等待 DMA Stream 真正关闭时使用的超时计数。
#define ESP_AT_PORT_DMA_DISABLE_TIMEOUT_LOOPS 100000UL


/*
 * ============================================================================
 * DMA 单次最大传输长度
 * ============================================================================
 * STM32F4 DMA 的 NDTR 是 16 bit。
 * 最大可表示：  0xFFFF  =  65535
 * 所以一次 DMA 传输最多配置 65535 个数据单位。
 * 当前数据单位为 Byte， 因此就是最多 65535 Byte。
 */
#define ESP_AT_PORT_DMA_MAX_TRANSFER_LENGTH    0xFFFFU


// DMA1 Stream6 IRQ 配置
// USART2 TX 使用 DMA1 Stream6， 所以对应的 Cortex-M IRQ 是： DMA1_Stream6_IRQn
#define ESP_AT_PORT_DMA_TX_IRQn                    DMA1_Stream6_IRQn   

// DMA 中断优先级   抢占优先级 = 6   子优先级   = 0
#define ESP_AT_PORT_DMA_TX_IRQ_PREEMPT_PRIORITY    6U
#define ESP_AT_PORT_DMA_TX_IRQ_SUB_PRIORITY        0U



/*
 * ============================================================================
 * USART2 RX IRQ 配置
 * ============================================================================
 */
#define ESP_AT_PORT_USART_IRQn                     USART2_IRQn
#define ESP_AT_PORT_USART_IRQ_PREEMPT_PRIORITY     6U  //抢占优先级
#define ESP_AT_PORT_USART_IRQ_SUB_PRIORITY         0U  //子优先级

/*
 * ============================================================================
 * USART2 RX Ring Buffer
 * ============================================================================
 * ESP32-C3 的返回数据不是固定长度的。
 * 例如：  OK\r\n 或： WIFI CONNECTED\r\n或更长的 HTTP / JSON 数据。
 * 所以不能只准备一个 Byte。
 */
#define ESP_AT_PORT_RX_BUFFER_SIZE                 1024U   //（单位字节）



/*
 * ============================================================================
 * Port 内部状态
 * ============================================================================
 * 0：  ESP-AT Port 尚未完成初始化。
 * 1：  GPIO / USART 等硬件已经初始化。
 */
static uint8_t s_initialized = 0U;

/*
 * 记录最近一次 USART2 TX DMA 的执行状态。
 * volatile：
 * DMA 中断处理程序可能异步修改这个变量，
 * 防止编译器把它当成始终不变的普通变量处理。
 * 注意：volatile 不等于线程同步机制。
 */
static volatile ESP_AT_PortTxState_t s_tx_state = ESP_AT_PORT_TX_IDLE;

/*
 * ============================================================================
 * TX DMA 状态
 * ============================================================================
 * 0：  当前没有正在进行的 USART2 TX DMA。
 * 1：   当前 DMA 正在发送数据。
 * 写 DMA IRQ 时， DMA 发送完成或者出错以后，会把这个状态重新清成 0。
 */
static volatile uint8_t s_tx_busy = 0U;

// USART2 RX Ring Buffer
static uint8_t s_rx_buffer[ESP_AT_PORT_RX_BUFFER_SIZE]; //真正保存 ESP32-C3 接收 Byte 的内存

static volatile uint16_t s_rx_head = 0U;  //head：ISR 下一次准备写入数据的位置

static volatile uint16_t s_rx_tail = 0U; //tail：上层下一次准备读取数据的位置

static volatile uint32_t s_rx_overflow_count = 0UL;  //Ring Buffer 满以后，新数据无法写入时累计次数。




/*
 * ============================================================================
 * DMA 初始化占位数据
 * ============================================================================
 * DMA_Init() 要求我们提供：
 *      Peripheral Address
 *      Memory Address
 *      Buffer Size
 * 但是在 Port_Init() 阶段，
 * 我们还没有真正要发送的 AT 数据。所以这里准备一个不会真正发送的 Dummy Byte，只是用于完成 DMA 的初始配置。
 * 注意： DMA Stream 此时不会 Enable，所以这个 Byte 不会被真正发送到 ESP32-C3。
 */
static uint8_t s_dma_dummy_byte = 0U;







/*
 * ============================================================================
 * 私有函数声明
 * ============================================================================
 */
static void prvGPIOInit(void); //初始化 PA2 / PA3

static void prvUSARTInit(void);  //初始化 USART2

static ESP_AT_PortStatus_t prvDMAInit(void);  //初始化 USART2 TX DMA

/*
 * 等待 DMA1 Stream6 真正进入 Disabled 状态。
 * 因为只有 Stream Disabled 时，才允许安全修改 M0AR / NDTR 等配置。
 */
static ESP_AT_PortStatus_t prvWaitDMAStreamDisabled(void);

//清除 DMA1 Stream6 上一笔传输遗留的所有 Flag。
static void prvClearDMAFlags(void);

static void prvNVICInit(void);  //初始化中断

static void prvRXPushByte(uint8_t byte);  //USART 中断收到一个 Byte，把它塞进 Ring Buffer






/*
 * ============================================================================
 * prvGPIOInit
 * ============================================================================
 *
 * 配置：
 *
 *      PA2 -> USART2_TX
 *      PA3 -> USART2_RX
 */
static void prvGPIOInit(void)
{
    GPIO_InitTypeDef gpio_init;
    RCC_AHB1PeriphClockCmd(ESP_AT_PORT_GPIO_RCC, ENABLE);  //打开 GPIOA 外设时钟
    //给 PA2 选择 USART2 Alternate Function
    GPIO_PinAFConfig(ESP_AT_PORT_GPIO, ESP_AT_PORT_TX_PIN_SOURCE, ESP_AT_PORT_GPIO_AF);
    //给 PA3 选择 USART2 Alternate Function
    GPIO_PinAFConfig(ESP_AT_PORT_GPIO, ESP_AT_PORT_RX_PIN_SOURCE, ESP_AT_PORT_GPIO_AF);

    GPIO_StructInit(&gpio_init);  //获取 GPIO 默认配置

    gpio_init.GPIO_Pin = ESP_AT_PORT_TX_PIN | ESP_AT_PORT_RX_PIN;  //PA2 和 PA3 一次配置

    gpio_init.GPIO_Mode = GPIO_Mode_AF;  //Alternate Function 模式

    gpio_init.GPIO_OType = GPIO_OType_PP;//推挽输出。

    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;

    gpio_init.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(ESP_AT_PORT_GPIO, &gpio_init);
}


/*
 * ============================================================================
 * prvUSARTInit
 * ============================================================================
 *
 * 初始化 STM32 USART2。
 */
static void prvUSARTInit(void)
{
    USART_InitTypeDef usart_init;

    RCC_APB1PeriphClockCmd(ESP_AT_PORT_USART_RCC, ENABLE);//打开 USART2 外设时钟

    USART_DeInit(ESP_AT_PORT_USART);  //把 USART2 恢复到复位状态

    USART_StructInit(&usart_init);  //获取 USART 默认配置

    usart_init.USART_BaudRate = ESP_AT_PORT_BAUD_RATE;//配置波特率

    usart_init.USART_WordLength = ESP_AT_PORT_WORD_LENGTH;  //配置数据位

    usart_init.USART_StopBits = ESP_AT_PORT_STOP_BITS;  //配置 Stop Bit

    usart_init.USART_Parity = ESP_AT_PORT_PARITY;  //不使用奇偶校验

    //不使用 RTS / CTS 硬件流控
    usart_init.USART_HardwareFlowControl = ESP_AT_PORT_HARDWARE_FLOW_CONTROL;

    usart_init.USART_Mode = ESP_AT_PORT_USART_MODE;  //同时启用发送和接收

    USART_Init(ESP_AT_PORT_USART, &usart_init);  //写入 USART2 配置

    USART_Cmd(ESP_AT_PORT_USART, ENABLE);//启用 USART2

    USART_ITConfig(ESP_AT_PORT_USART, USART_IT_RXNE, ENABLE);//允许 USART2 在收到新 Byte 时产生 RXNE 中断
}


/*
 * ============================================================================
 * prvWaitDMAStreamDisabled
 * ============================================================================
 */
static ESP_AT_PortStatus_t prvWaitDMAStreamDisabled(void)
{
    uint32_t timeout;
    /*
     * 设置一个软件超时计数。
     * 防止 DMA 硬件异常时，CPU 永远卡死在 while 中。
     */
    timeout = ESP_AT_PORT_DMA_DISABLE_TIMEOUT_LOOPS;

    /*
     * DMA_GetCmdStatus() 会检查 Stream 的 EN 状态。
     * 只有真正返回 DISABLE，我们才认为 Stream 已经可以重新配置。
     */
    while (DMA_GetCmdStatus(ESP_AT_PORT_DMA_TX_STREAM) != DISABLE)
    {
        if (timeout == 0UL)
        {
            return ESP_AT_PORT_ERROR_TIMEOUT;
        }

        --timeout;
    }


    return ESP_AT_PORT_OK;
}


/*
 * ============================================================================
 * prvClearDMAFlags
 * ============================================================================
 */
static void prvClearDMAFlags(void)
{
    //清除 Stream6 上一笔 DMA 可能留下的flag
    DMA_ClearFlag(ESP_AT_PORT_DMA_TX_STREAM, ESP_AT_PORT_DMA_TX_ALL_FLAGS);
}


/*
 * ============================================================================
 * prvDMAInit
 * ============================================================================
 *
 * 初始化：
 *
 *      USART2_TX
 *          ↓
 *      DMA1 Stream6
 *          ↓
 *      Channel4
 *
 * 当前这里只完成 DMA 的静态配置。
 *
 * 不会真正开始一次 DMA 传输。
 */
static ESP_AT_PortStatus_t prvDMAInit(void)
{
    DMA_InitTypeDef dma_init;

    RCC_AHB1PeriphClockCmd(ESP_AT_PORT_DMA_RCC, ENABLE);//打开 DMA1 时钟

    DMA_Cmd(ESP_AT_PORT_DMA_TX_STREAM, DISABLE);  //请求关闭 DMA1 Stream6

    if (prvWaitDMAStreamDisabled() != ESP_AT_PORT_OK)
    {
        return ESP_AT_PORT_ERROR_TIMEOUT;
    }

    DMA_DeInit(ESP_AT_PORT_DMA_TX_STREAM);//把 Stream6 恢复到复位状态

    //清除 Stream6 可能残留的状态 Flag
    prvClearDMAFlags();



    DMA_StructInit(&dma_init);  //获取 DMA 默认配置

    dma_init.DMA_Channel = ESP_AT_PORT_DMA_TX_CHANNEL;  //选择 Channel4

    //设置 Peripheral Address（目标外设地址）
    dma_init.DMA_PeripheralBaseAddr = (uint32_t)&ESP_AT_PORT_USART->DR;

    //设置初始化阶段的 Memory Address
    dma_init.DMA_Memory0BaseAddr = (uint32_t)&s_dma_dummy_byte;

    dma_init.DMA_DIR = DMA_DIR_MemoryToPeripheral;//数据搬运方向

    dma_init.DMA_BufferSize = 1U;  //初始化阶段 Buffer Size = 1

    //Peripheral Address 不递增
    dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;

    //Memory Address 自动递增
    dma_init.DMA_MemoryInc = DMA_MemoryInc_Enable;

    //USART 数据按照一个 Byte 一个 Byte 发送
    dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;

    //Memory Data Size = Byte
    dma_init.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;

    //Normal Mode
    dma_init.DMA_Mode = DMA_Mode_Normal;

    //同时竞争 DMA 总线时的优先级
    dma_init.DMA_Priority = DMA_Priority_High;

    //不使用 DMA FIFO
    dma_init.DMA_FIFOMode = DMA_FIFOMode_Disable;

    /*
     * FIFO Disabled 时这个 Threshold 基本不会参与实际传输，
     * 但结构体字段仍给一个合法值。
     */
    dma_init.DMA_FIFOThreshold = DMA_FIFOThreshold_1QuarterFull;

    //当前不使用 Burst，每次按单个数据单位搬运。
    dma_init.DMA_MemoryBurst = DMA_MemoryBurst_Single;
    dma_init.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;

    //把配置写进 DMA1 Stream6
    DMA_Init(ESP_AT_PORT_DMA_TX_STREAM, &dma_init);

    //允许 DMA1 Stream6 产生中断  DMA_IT_TC：Transfer Complete(DMA 本次传输全部完成)
    //DMA_IT_TE：Transfer Error(DMA 传输发生错误)  DMA_IT_DME：Direct Mode Error
    //DMA在发生TC TE DEM这些事的时候允许产生中断请求
    DMA_ITConfig(ESP_AT_PORT_DMA_TX_STREAM, DMA_IT_TC | DMA_IT_TE | DMA_IT_DME, ENABLE);

    /*
     * ========================================================================
     * 允许 USART2 产生 TX DMA Request
     * ========================================================================
     * 这一句特别重要。 它并不会马上开始 DMA。
     * 它只是告诉 USART2：  当 TX 需要新数据时，  可以向 DMA Controller 发出 DMA Request。
     */
    USART_DMACmd(ESP_AT_PORT_USART, USART_DMAReq_Tx, ENABLE);


    /*
     * 注意：
     * 当前没有执行：   DMA_Cmd(Stream6, ENABLE);
     * 所以 DMA 仍然没有开始传输。
     */
    return ESP_AT_PORT_OK;
}


//prvNVICInit  配置 DMA1 Stream6 在 Cortex-M4 NVIC 中的中断。
static void prvNVICInit(void)
{
    //DMA1 Stream6 IRQ
    NVIC_InitTypeDef nvic_init;

    nvic_init.NVIC_IRQChannel = ESP_AT_PORT_DMA_TX_IRQn;//选择 DMA1 Stream6 IRQ

    nvic_init.NVIC_IRQChannelPreemptionPriority =
        ESP_AT_PORT_DMA_TX_IRQ_PREEMPT_PRIORITY;  //配置抢占优先级

    nvic_init.NVIC_IRQChannelSubPriority =
        ESP_AT_PORT_DMA_TX_IRQ_SUB_PRIORITY;  //配置子优先级

    nvic_init.NVIC_IRQChannelCmd = ENABLE;  //允许 Cortex-M4 响应这个中断

    NVIC_Init(&nvic_init);  //真正把配置写入 NVIC


    //USART2 RX IRQ
    nvic_init.NVIC_IRQChannel = ESP_AT_PORT_USART_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = ESP_AT_PORT_USART_IRQ_PREEMPT_PRIORITY;
    nvic_init.NVIC_IRQChannelSubPriority = ESP_AT_PORT_USART_IRQ_SUB_PRIORITY;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;

    NVIC_Init(&nvic_init);
}


/*
 * ============================================================================
 * prvRXPushByte
 * ============================================================================
 * 把 USART2 刚收到的一个 Byte 写入 RX Ring Buffer。
 * 这个函数会在 USART2 ISR 中调用。
 */
static void prvRXPushByte(uint8_t byte)
{
    uint16_t next_head;
    /*
     * 计算：
     * 当前 head 的下一个位置。
     * % ESP_AT_PORT_RX_BUFFER_SIZE
     * 用来实现 Ring Buffer 到末尾以后自动回到 0。
     */
    //此处是环形缓冲区
    next_head = (uint16_t)((s_rx_head + 1U) % ESP_AT_PORT_RX_BUFFER_SIZE);


    /*
     * 如果 next_head == tail：说明 Buffer 已经满了。
     * 当前策略：   保留旧数据   丢弃当前新 Byte
     */
    if (next_head == s_rx_tail)
    {
        ++s_rx_overflow_count;
        return;
    }

    s_rx_buffer[s_rx_head] = byte;  //把当前 Byte 写入 head 指向的位置

    s_rx_head = next_head;  //head 向前移动
}









/*
 * ============================================================================
 * ESP_AT_Port_Init
 * ============================================================================
 */
ESP_AT_PortStatus_t ESP_AT_Port_Init(void)
{
    ESP_AT_PortStatus_t status;

    if (s_initialized != 0U)  //若已经初始化过，再次调用直接认为成功
    {
        return ESP_AT_PORT_OK;
    }

    prvGPIOInit();  //配置使用的 GPIO

    prvUSARTInit();  //配置 USART

    status = prvDMAInit();  //初始化  DMA。

    if (status != ESP_AT_PORT_OK)  
    {
        return status;
    }

    prvNVICInit();  //配置 DMA1 Stream6 的 NVIC。

    s_initialized = 1U; //标记 Port 初始化完成

    return ESP_AT_PORT_OK;
}


/*
 * ============================================================================
 * ESP_AT_Port_WriteAsync
 * ============================================================================
 * 使用 DMA1 Stream6 将一段数据异步发送到 USART2。
 * 数据流：
 *      data[]
 *        ↓
 *      DMA1 Stream6
 *        ↓
 *      USART2->DR
 *        ↓
 *      USART2 TX
 *        ↓
 *      PA2
 *        ↓
 *      ESP32-C3 RX
 */
ESP_AT_PortStatus_t ESP_AT_Port_WriteAsync(const uint8_t *data, size_t length)
{
    ESP_AT_PortStatus_t status;

    if (s_initialized == 0U)  //检查 Port 是否已经初始化
    {
        return ESP_AT_PORT_ERROR_NOT_INITIALIZED;
    }

    if ((data == NULL) || (length == 0U))  //参数检查
    {
        return ESP_AT_PORT_ERROR_PARAM;
    }

    if (length > ESP_AT_PORT_DMA_MAX_TRANSFER_LENGTH)//检查 DMA 单次传输长度
    {
        return ESP_AT_PORT_ERROR_PARAM;
    }

    if (s_tx_busy != 0U)//检查当前 TX DMA 是否正在使用
    {
        return ESP_AT_PORT_ERROR_BUSY;
    }

    DMA_Cmd(ESP_AT_PORT_DMA_TX_STREAM, DISABLE); //确保 DMA Stream6 处于 Disabled 状态

    status = prvWaitDMAStreamDisabled();

    if (status != ESP_AT_PORT_OK)
    {
        return status;
    }


    prvClearDMAFlags();  //清除上一笔 DMA 遗留的 Flag

    //设置本次 DMA 的 Memory Address
    DMA_MemoryTargetConfig(ESP_AT_PORT_DMA_TX_STREAM, (uint32_t)data, DMA_Memory_0);

    //设置本次需要发送的数据数量
    DMA_SetCurrDataCounter(ESP_AT_PORT_DMA_TX_STREAM, (uint16_t)length);

    //标记 TX DMA 已经 Busy
    s_tx_busy = 1U;

    /* 记录本次 DMA 发送已经开始。 */
    s_tx_state = ESP_AT_PORT_TX_BUSY;

    //启动 DMA1 Stream6
    DMA_Cmd(ESP_AT_PORT_DMA_TX_STREAM, ENABLE);

    /* 重新允许 USART2 发出 TX DMA 请求。 */
    USART_DMACmd(ESP_AT_PORT_USART, USART_DMAReq_Tx, ENABLE);

    //立即返回
    return ESP_AT_PORT_OK;
}


/*
 * ============================================================================
 * ESP_AT_Port_Read
 * ============================================================================
 * 从 USART2 RX Ring Buffer 中读取数据。
 * 本函数不会等待新数据到来。
 */
ESP_AT_PortStatus_t ESP_AT_Port_Read(uint8_t *data, size_t max_length, size_t *read_length)
{
    size_t count;
    uint16_t head_snapshot;


    if (s_initialized == 0U)  //检查Port是否已经初始化
    {
        return ESP_AT_PORT_ERROR_NOT_INITIALIZED;
    }


    if ((data == NULL) || (max_length == 0U) || (read_length == NULL))  //参数检查
    {
        return ESP_AT_PORT_ERROR_PARAM;
    }


    /*
     * 默认本次读取 0 Byte。
     * 如果 Ring Buffer 当前是空的， 后面 while 不会进入，
     * 最终自然返回 read_length = 0。
     */
    *read_length = 0U;


    /*
     * 保存当前 head 的快照。
     * head 是 USART2 ISR 修改的。
     * 本次 Read() 只读取调用这一刻之前已经进入 Ring Buffer 的数据。
     * Read() 执行期间新收到的数据，留给下一次 Read()。
     */
    head_snapshot = s_rx_head;


    /*
     * 当前已经读取的数据数量。
     */
    count = 0U;


    /*
     * 两个条件同时满足时继续读取：
     * 1. tail != head_snapshot 即Buffer 中还有本次需要读取的数据。
     * 2. count < max_length 即上层提供的目标 Buffer 还有空间。
     */
    while ((s_rx_tail != head_snapshot) && (count < max_length))
    {
        data[count] = s_rx_buffer[s_rx_tail];  //取出 tail 当前指向的一个 Byte


        /*
         * tail 向后移动。
         * 到 Buffer 最后一个位置以后， 通过 % 自动回到 0。
         */
        s_rx_tail = (uint16_t)((s_rx_tail + 1U) % ESP_AT_PORT_RX_BUFFER_SIZE);


        ++count;//实际读取数量 +1
    }


    *read_length = count;  //更新这次到底读取了多少Byte

    return ESP_AT_PORT_OK;
}


/*
 * ============================================================================
 * ESP_AT_Port_DMAIRQHandler
 * ============================================================================
 * USART2 TX DMA：  DMA1 Stream6
 * 可能处理：   Transfer Error  Direct Mode Error  Transfer Complete
 * 当前阶段本函数只负责 DMA Port 内部收尾。暂时不通知 AT Core。
 */
void ESP_AT_Port_DMAIRQHandler(void)
{
    uint8_t irq_handled;
    uint8_t dma_error;

    irq_handled = 0U;  // 默认：当前还没有发现属于我们需要处理的 DMA 事件。
    dma_error = 0U;

    //下边连用三个if不用if else是为了如果多个 flag被置位 可以清除多个

    /*
     * ========================================================================
     * Transfer Error
     * ========================================================================
     * 错误事件优先于成功事件处理。
     */
    if (
        DMA_GetITStatus(
            ESP_AT_PORT_DMA_TX_STREAM,
            DMA_IT_TEIF6
        ) != RESET
    )
    {
        /*
         * 清除 Transfer Error 中断 Pending Flag。
         */
        DMA_ClearITPendingBit(
            ESP_AT_PORT_DMA_TX_STREAM,
            DMA_IT_TEIF6
        );


        irq_handled = 1U;
        dma_error = 1U;
    }


    /*
     * ========================================================================
     *  Direct Mode Error
     * ========================================================================
     */
    if (
        DMA_GetITStatus(
            ESP_AT_PORT_DMA_TX_STREAM,
            DMA_IT_DMEIF6
        ) != RESET
    )
    {
        DMA_ClearITPendingBit(
            ESP_AT_PORT_DMA_TX_STREAM,
            DMA_IT_DMEIF6
        );


        irq_handled = 1U;
        dma_error = 1U;
    }


    /*
     * ========================================================================
     *  Transfer Complete
     * ========================================================================
     * TCIF6 = 1： NDTR 已经减到 0。 DMA 已经把本次所有数据交给 USART2。
     */
    if (
        DMA_GetITStatus(
            ESP_AT_PORT_DMA_TX_STREAM,
            DMA_IT_TCIF6
        ) != RESET
    )
    {
        DMA_ClearITPendingBit(
            ESP_AT_PORT_DMA_TX_STREAM,
            DMA_IT_TCIF6
        );


        irq_handled = 1U;
    }


    /*
     * ========================================================================
     *  只要处理到了属于 TX DMA 的事件，就进行统一收尾
     * ========================================================================
     */
    if (irq_handled != 0U)
    {
        /*
         * 请求关闭 DMA1 Stream6。
         * 注意： 当前正在 ISR 中，这里只发送 Disable 请求。
         * 不调用：  prvWaitDMAStreamDisabled()
         * 因为 ISR 中不应该做可能较长时间的轮询等待。
         */
        DMA_Cmd(ESP_AT_PORT_DMA_TX_STREAM, DISABLE);


        /*
        * 根据本次中断事件记录发送结果。
        * 只要出现 TE 或 DME，就认为此次 DMA 失败。
        */
        if (dma_error != 0U)
        {
            s_tx_state = ESP_AT_PORT_TX_ERROR;
        }
        else
        {
            s_tx_state = ESP_AT_PORT_TX_DONE;
        }

        /* DMA 不再处于软件定义的忙状态。 */
        s_tx_busy = 0U;

    }
}


/*
 * ============================================================================
 * ESP_AT_Port_GetTxState
 * ============================================================================
 * 功能：  查询最近一次 USART2 TX DMA 的发送状态。
 * 注意：  这里只返回 DMA 状态，不代表 ESP32-C3 已经回复 OK。
 */
ESP_AT_PortTxState_t ESP_AT_Port_GetTxState(void)
{
    return s_tx_state;
}


/*
 * ============================================================================
 * ESP_AT_Port_AbortTx
 * ============================================================================
 * 功能：
 *     终止 USART2 TX DMA 数据搬运。
 * 使用场景：
 *     AT 命令超时，需要停止未完成的 DMA 发送；
 *     上层主动取消当前发送事务。
 * 注意：
 *     本函数必须由任务上下文调用。
 *     当前 ESP-AT Port 由 NetworkTask 独占使用。
 */
ESP_AT_PortStatus_t ESP_AT_Port_AbortTx(void)
{
    ESP_AT_PortStatus_t status;

    /* 1. 检查 Port 是否已经初始化。 */
    if (s_initialized == 0U)
    {
        return ESP_AT_PORT_ERROR_NOT_INITIALIZED;
    }

    /*
     * 2. 暂时屏蔽 DMA1 Stream6 中断。
     * 防止任务正在取消 DMA 时，
     * DMA 中断又修改 s_tx_busy 和 s_tx_state。
     */
    NVIC_DisableIRQ(ESP_AT_PORT_DMA_TX_IRQn);

    /*
     * 3. 禁止 USART2 继续向 DMA 发出 TX 请求。
     * 这里关闭的是 DMA 请求功能，
     * 不是直接关闭整个 USART2。
     */
    USART_DMACmd(ESP_AT_PORT_USART, USART_DMAReq_Tx, DISABLE);

    /* 4. 请求关闭 DMA1 Stream6。 */
    DMA_Cmd(ESP_AT_PORT_DMA_TX_STREAM, DISABLE);

    /*
     * 5. 等待 DMA Stream 真正停止。
     * 只有 EN 位清零后，才能确认
     * DMA 不会继续读取发送缓冲区。
     */
    status = prvWaitDMAStreamDisabled();

    if (status != ESP_AT_PORT_OK)
    {
        /*
         * 无法确认 DMA 已停止。
         * 保持软件发送锁定，不允许覆盖发送缓冲区。
         * 保持此 DMA IRQ 屏蔽，等待更高层执行故障恢复。
         */
        s_tx_busy = 1U;
        s_tx_state = ESP_AT_PORT_TX_ERROR;

        return status;
    }

    /*
     * 6. 清除 DMA Stream6 的全部历史标志。
     */
    prvClearDMAFlags();

    /*
     * 7. 清除 NVIC 中可能已经挂起的旧 DMA 中断。
     */
    NVIC_ClearPendingIRQ(ESP_AT_PORT_DMA_TX_IRQn);

    /*
     * 8. 更新 Port 软件状态。
     * 到这里已经确认 DMA Stream 停止，
     * 因此可以解除软件 Busy 状态。
     */
    s_tx_busy = 0U;
    s_tx_state = ESP_AT_PORT_TX_ABORTED;

    /* 9. 恢复 DMA1 Stream6 的 NVIC 中断入口。 */
    NVIC_EnableIRQ(ESP_AT_PORT_DMA_TX_IRQn);

    return ESP_AT_PORT_OK;
}


/*
 * ============================================================================
 * ESP_AT_Port_USARTIRQHandler
 * ============================================================================
 * 处理 USART2 RXNE 中断。
 * 当前 ISR 只做：
 *      读一个 Byte
 *          ↓
 *      放入 Ring Buffer
 * 不在 ISR 中解析：   OK  ERROR  ready   WIFI CONNECTED
 * 字符串解析以后放在 Task 上下文进行。
 */
void ESP_AT_Port_USARTIRQHandler(void)
{
    uint8_t received_byte;

    /*
     * USART_IT_RXNE： Receive Data Register Not Empty。
     * 表示 USART2->DR 中已经有一个新 Byte。
     */
    if (USART_GetITStatus(ESP_AT_PORT_USART, USART_IT_RXNE) != RESET)
    {
        /*
         * 从 USART2 DR 读出数据。
         * USART_ReceiveData() 返回 uint16_t，当前使用 8-bit UART，所以只保留低 8 bit。
         */
        received_byte = (uint8_t)(USART_ReceiveData(ESP_AT_PORT_USART) & 0x00FFU);

        /*
         * 将收到的 Byte 放入 RX Ring Buffer。
         */
        prvRXPushByte(received_byte);
    }
}














/*
 * DMA1 Stream6 真正的硬件中断入口。
 * 转交给 ESP-AT Port 层处理。
 */
void DMA1_Stream6_IRQHandler(void)
{
    ESP_AT_Port_DMAIRQHandler();
}

/*
 * USART2 真正的硬件中断入口。
 * 转交给 ESP-AT Port 层处理。
 */
void USART2_IRQHandler(void)
{
    ESP_AT_Port_USARTIRQHandler();
}

