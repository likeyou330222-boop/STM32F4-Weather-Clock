#include "aht20_port.h"

/*
 * ============================================================================
 * STM32F407 Standard Peripheral Library
 * ============================================================================
 * 注意：
 * STM32 专属头文件只放在 Port 实现文件。
 * aht20_port.h 不应该依赖 STM32。
 */
#include "stm32f4xx.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_i2c.h"


/*
 * ============================================================================
 * 当前 STM32F407 硬件配置
 * ============================================================================
 */
/* --------------------------------------------------------------------------
 * I2C 外设
 * --------------------------------------------------------------------------
 */
#define AHT20_PORT_I2C                       I2C2

#define AHT20_PORT_I2C_RCC                   RCC_APB1Periph_I2C2

#define AHT20_PORT_GPIO                      GPIOB

#define AHT20_PORT_GPIO_RCC                  RCC_AHB1Periph_GPIOB

#define AHT20_PORT_SCL_PIN                   GPIO_Pin_10

#define AHT20_PORT_SCL_SOURCE                GPIO_PinSource10

#define AHT20_PORT_SDA_PIN                   GPIO_Pin_11

#define AHT20_PORT_SDA_SOURCE                GPIO_PinSource11

#define AHT20_PORT_GPIO_AF                   GPIO_AF_I2C2

#define AHT20_PORT_I2C_SPEED_HZ              100000UL

#define AHT20_PORT_I2C_MODE              I2C_Mode_I2C   //标准 I2C 工作模式

#define AHT20_PORT_I2C_DUTY_CYCLE        I2C_DutyCycle_2   //Duty Cycle  占空比

#define AHT20_PORT_I2C_OWN_ADDRESS       0x00U   //当前没有这个需求，因此设为 0

#define AHT20_PORT_I2C_ACK               I2C_Ack_Enable   //允许 ACK

#define AHT20_PORT_I2C_ADDRESS_MODE      I2C_AcknowledgedAddress_7bit   //使用 7-bit I2C 地址

#define AHT20_PORT_TIMEOUT_LOOPS             100000UL

#define AHT20_PORT_I2C_ADDRESS_MAX      0x7FU    //7-bit I2C 地址最大只能是： 0b0111 1111 = 0x7F

#define AHT20_PORT_I2C_ADDRESS_SHIFT    1U   //把真正的 7-bit 地址左移 1 位



/*
 * ============================================================================
 *  Port 内部状态
 * ============================================================================
 */
static uint8_t s_initialized = 0U;



/*
 * ============================================================================
 * 私有函数声明
 * ============================================================================
 */
static void prvGPIOInit(void);  //初始化GPIO

static void prvI2CInit(void);  //初始化 I2C2

static AHT20_PortStatus_t prvWaitBusIdle(void);  //等待 I2C 总线恢复为空闲

static AHT20_PortStatus_t prvWaitEvent(uint32_t event);  //等待某个 STM32 I2C Event 出现

static AHT20_PortStatus_t prvWaitAddressAck(uint32_t event);  //等待设备地址被 ACK

static AHT20_PortStatus_t prvStop(void);  //产生 STOP，并等待总线真正恢复空闲

static uint8_t prvMakeAddressByte(uint8_t address_7bit);   //把真正的 7-bit 地址转换成 STM32 SPL 所使用的地址格式

static AHT20_PortStatus_t prvWaitByteTransmitted(void);//等待一个数据Byte完整发送,并检查对方是否产生 NACK

static AHT20_PortStatus_t prvWaitAddressMatched(void); //等待 Slave ACK 地址，并等待 ADDR Flag（找到地址 置1） 出现。

static AHT20_PortStatus_t prvWaitFlagSet(uint32_t flag);//等待指定 I2C Flag 被置位

static void prvClearAddrFlag(void); //清除 ADDR Flag

static void prvRestoreReceiverConfig(void);//把 Receiver 相关配置恢复到默认状态


/*
 * ============================================================================
 * 公开 API
 * ============================================================================
 */

AHT20_PortStatus_t AHT20_Port_Init(void)
{

    if (s_initialized != 0U)   //如果已经初始化过，直接返回成功
    {
        return AHT20_PORT_OK;
    }

    prvGPIOInit();  //初始化 PB10 / PB11

    prvI2CInit();  //初始化 I2C2

    s_initialized = 1U;  //记录初始化完成

    return AHT20_PORT_OK;
}

AHT20_PortStatus_t AHT20_Port_DeInit(void)
{
    s_initialized = 0U;

    return AHT20_PORT_OK;
}

AHT20_PortStatus_t AHT20_Port_Probe(uint8_t address_7bit)
{
    AHT20_PortStatus_t status;

    uint8_t address_byte;

    if (s_initialized == 0U)  //检查 Port 是否已经初始化
    {
        return AHT20_PORT_ERROR_NOT_INITIALIZED;
    }

    if (address_7bit > AHT20_PORT_I2C_ADDRESS_MAX)  //检查 7-bit 地址是否合法 7-bit 地址不能大于 0x7F
    {
        return AHT20_PORT_ERROR_PARAM;
    }

    address_byte = prvMakeAddressByte(address_7bit);//把 7-bit 地址转换成 STM32 SPL 使用的格式

    status = prvWaitBusIdle();//等待总线空闲

    if (status != AHT20_PORT_OK)
    {
        return status;
    }

    I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_AF); //清除可能残留的 AF Flag

    I2C_GenerateSTART(AHT20_PORT_I2C, ENABLE); //产生 START

    status = prvWaitEvent(I2C_EVENT_MASTER_MODE_SELECT); //等待 EV5（START已经成功产生  +STM32已经进入Master模式）

    if (status != AHT20_PORT_OK)
    {
        /*
         * 尝试结束这次异常事务。
         */
        (void)prvStop();
        return status;
    }

    I2C_Send7bitAddress(AHT20_PORT_I2C, address_byte, I2C_Direction_Transmitter);   //发送从设备地址


     //等待 AHT20 ACK  EV6（地址已经发出 + 对方已经ACK + STM32已经进入Master Transmitter状态）
    status = prvWaitAddressAck(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);

    if (status != AHT20_PORT_OK)
    {
        /*
         * 即使失败，
         * 也必须尽量发送 STOP，
         * 把总线释放掉。
         */
        (void)prvStop();

        return status;
    }

    status = prvStop();

    if (status != AHT20_PORT_OK)
    {
        return status;
    }

    return AHT20_PORT_OK;
}

AHT20_PortStatus_t AHT20_Port_Write(uint8_t address_7bit, const uint8_t *data, size_t length)
{
    AHT20_PortStatus_t status;
    uint8_t address_byte;
    size_t index;

    if (s_initialized == 0U)  //检查 Port 是否已经初始化
    {
        return AHT20_PORT_ERROR_NOT_INITIALIZED;
    }

    if ((data == NULL) || (length == 0U))  //检查参数
    {
        return AHT20_PORT_ERROR_PARAM;
    }

    if (address_7bit > AHT20_PORT_I2C_ADDRESS_MAX) ////检查参数
    {
        return AHT20_PORT_ERROR_PARAM;
    }

    address_byte = prvMakeAddressByte(address_7bit); //把真正的 7-bit 地址转换成 STM32 SPL（标准外设库） 使用的格式

    status = prvWaitBusIdle();  //等待 I2C 总线空闲

    if (status != AHT20_PORT_OK)
    {
        return status;
    }

    I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_AF);//清除可能残留的 ACK Failure

    I2C_GenerateSTART(AHT20_PORT_I2C, ENABLE); //产生 START

    status = prvWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);  //等待 STM32 进入 Master Mode

    if (status != AHT20_PORT_OK)
    {
        (void)prvStop();
        return status;
    }

    I2C_Send7bitAddress(AHT20_PORT_I2C, address_byte, I2C_Direction_Transmitter);  //发送 Slave Address + Write

    status = prvWaitAddressAck(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED); //等待 Slave ACK 地址

    if (status != AHT20_PORT_OK)
    {
        (void)prvStop();
        return status;
    }

    for (index = 0U; index < length; ++index)  //连续发送数据
    {
        /*
         * 把当前 Byte 写入 I2C 数据寄存器。
         */
        I2C_SendData(AHT20_PORT_I2C, data[index]);

        /*
         * 等待当前 Byte 真正发送完成。
         */
        status = prvWaitByteTransmitted();

        if (status != AHT20_PORT_OK)
        {
            (void)prvStop();
            return status;
        }
    }


    /*
     * =========================================================================
     * 8. 全部数据发送完成，产生 STOP
     * =========================================================================
     */
    status = prvStop();

    if (status != AHT20_PORT_OK)
    {
        return status;
    }


    return AHT20_PORT_OK;
}

AHT20_PortStatus_t AHT20_Port_Read(uint8_t address_7bit, uint8_t *data, size_t length)
{

    AHT20_PortStatus_t status;

    uint8_t address_byte;

    size_t remaining;  //用来记录“还有多少 Byte 没有读取”

    size_t index = 0U;  //用于记录当前数据应该保存到 data[] 的哪个位置

    if (s_initialized == 0U)  //检查 Port 是否已经初始化
    {
        return AHT20_PORT_ERROR_NOT_INITIALIZED;
    }

    if ((data == NULL) || (length == 0U))  //检查数据缓冲区和长度
    {
        return AHT20_PORT_ERROR_PARAM;
    }

    if (address_7bit > AHT20_PORT_I2C_ADDRESS_MAX)  //检查 I2C 地址是否合法
    {
        return AHT20_PORT_ERROR_PARAM;
    }

    address_byte = prvMakeAddressByte(address_7bit);  //转换地址格式

    status = prvWaitBusIdle();  //等待 I2C 总线空闲

    if (status != AHT20_PORT_OK)
    {
        return status;
    }

    prvRestoreReceiverConfig();  //恢复 Receiver 默认配置

    I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_AF);  //清除可能残留的 AF

    I2C_GenerateSTART(AHT20_PORT_I2C, ENABLE);  //产生 START

    status = prvWaitEvent(I2C_EVENT_MASTER_MODE_SELECT); //等待 Master Mode Select。

    if (status != AHT20_PORT_OK)
    {
        (void)prvStop();

        prvRestoreReceiverConfig();

        return status;
    }

    //发送 Slave Address + Read
    I2C_Send7bitAddress(AHT20_PORT_I2C, address_byte, I2C_Direction_Receiver);


    /*
     * =========================================================================
     * 等待 Slave ACK 地址
     * =========================================================================
     * 注意：
     * prvWaitAddressMatched() 只等待： ADDR = SET 它不会主动把 ADDR 清掉。
     * 因为后面根据读取 Byte 数量不同， 我们还需要决定什么时候配置 ACK / POS，
     * 然后再调用：  prvClearAddrFlag();
     */
    status = prvWaitAddressMatched();
    //此处为什么不用prvWaitAddressAck() 这是因为prvWaitAddressAck()内部调用了I2C_CheckEvent() 
    //会把SR1 SR2读取 进而ADDR就被清掉了 但是在Read模式下 我们不能总是立刻清ADDR  得先关闭ACK再清ADDR 
    //然而 我们读数据的时候 在接收最后一位数据之前需要设置NACK ADDR像是一道门 可以在接收之前暂停 有时间来配置NACK 


    if (status != AHT20_PORT_OK)
    {
        (void)prvStop();
        prvRestoreReceiverConfig();

        return status;
    }

    remaining = length;  //记录当前还有多少 Byte 需要接收

    /*
     * =========================================================================
     * 只读取 1 Byte
     * =========================================================================
     * 当前：   AHT20 已经 ACK 地址； ADDR Flag 已经置位；但是我们还故意没有清除 ADDR。
     * 如果只读取 1 Byte，那么这个 Byte 同时也是最后一个 Byte。
     * 因此必须：
     *      1. 先关闭 ACK
     *      2. 再清除 ADDR
     *      3. 产生 STOP
     *      4. 等待数据到达
     *      5. 读取这个 Byte
     */
    if (remaining == 1U)
    {
        I2C_AcknowledgeConfig(AHT20_PORT_I2C, DISABLE); //关闭 ACK

        prvClearAddrFlag(); //清除 ADDR

        I2C_GenerateSTOP(AHT20_PORT_I2C, ENABLE);//产生 STOP

        status = prvWaitFlagSet(I2C_FLAG_RXNE); // 等待 RXNE（Receive Data Register Not Empty）

        if (status != AHT20_PORT_OK)
        {
            /*
             * 无论成功还是失败，
             * 都尽量恢复 Receiver 的默认配置。
             */
            prvRestoreReceiverConfig();

            return status;
        }

        data[0] = I2C_ReceiveData(AHT20_PORT_I2C);  //从 I2C 数据寄存器读取这个 Byte

        status = prvWaitBusIdle(); //等待 STOP 真正完成即总线真正恢复空闲

        prvRestoreReceiverConfig();  //恢复 ACK / POS

        return status;
    }

    /*
     * =========================================================================
     * 只读取 2 Byte
     * =========================================================================
     * 当前状态：
     *      1. START 已经发送
     *      2. Slave Address + Read 已经发送
     *      3. AHT20 已经 ACK 地址
     *      4. ADDR = 1
     *      5. ADDR 还没有清除
     * STM32F4 对 2 Byte Master Receiver 有特殊要求：
     *      1. POS 设置成 Next
     *      2. 关闭 ACK
     *      3. 清除 ADDR
     *      4. 等待 BTF
     *      5. 产生 STOP
     *      6. 连续读取两个 Byte
     */
    if (remaining == 2U)  //因为还有移位寄存器 所以需要特殊照顾
    {

        // POS 用来决定：关闭 ACK 后， NACK 应该作用在当前 Byte，还是作用在下一个 Byte。

        I2C_NACKPositionConfig(AHT20_PORT_I2C, I2C_NACKPosition_Next);   //设置 NACK Position = Next

        // POS = Next 和 ACK = Disable 配合，是 STM32F4 两 Byte 接收的特殊处理方式。
        I2C_AcknowledgeConfig(AHT20_PORT_I2C, DISABLE);//关闭 ACK

        prvClearAddrFlag(); //清除 ADDR


         // 对这里的 2 Byte 接收来说，我们不急着看到第一个 RXNE 就马上读 DR。
         // 而是等待 BTF（Byte Transfer Finished），让两个 Byte 都进入 STM32 I2C 的接收流水线。
        status = prvWaitFlagSet(I2C_FLAG_BTF);//等待 BTF

        if (status != AHT20_PORT_OK)
        {
            /*
             * 接收过程出现异常时，
             * 尝试产生 STOP 释放 I2C 总线。
             */
            (void)prvStop();

            /*
             * 恢复 ACK / POS 默认配置，
             * 防止本次错误影响下一次通信。
             */
            prvRestoreReceiverConfig();

            return status;
        }

        I2C_GenerateSTOP(AHT20_PORT_I2C, ENABLE);  //产生 STOP

        data[0] = I2C_ReceiveData(AHT20_PORT_I2C); //读取第 1 个 Byte

        data[1] = I2C_ReceiveData(AHT20_PORT_I2C);//读取第 2 个 Byte

        status = prvWaitBusIdle(); //等待总线真正恢复空闲

        prvRestoreReceiverConfig(); //恢复 Receiver 默认配置

        return status;
    }

   
    /*
     * =========================================================================
     *  读取 3 Byte 或更多
     * =========================================================================
     * 如果程序能够运行到这里，
     * 说明： remaining >= 3
     * AHT20 实际测量数据需要读取 7 Byte，
     * 因此真正读取 AHT20 时主要进入这个分支。
     */

    I2C_AcknowledgeConfig(AHT20_PORT_I2C, ENABLE);//确保 ACK 处于 Enable 状态

    prvClearAddrFlag(); //清除 ADDR Flag

    while (remaining > 3U)  //接收前面的普通 Byte
    {
        status = prvWaitFlagSet(I2C_FLAG_RXNE); //等待一个 Byte 到达

        if (status != AHT20_PORT_OK)
        {
            /*
             * 尝试结束当前 I2C 事务，
             * 避免总线一直保持 Busy。
             */
            (void)prvStop();

            /*
             * 恢复 ACK / POS 默认状态，
             * 避免影响下一次通信。
             */
            prvRestoreReceiverConfig();

            return status;
        }

        data[index] = I2C_ReceiveData(AHT20_PORT_I2C); //从 I2C2 的 DR 中读取当前 Byte

        ++index;  //当前数组位置已经使用， 下一 Byte 应该保存到下一个位置。

        --remaining;//已经成功读取一个 Byte， 所以剩余未读取数量减 1。
    }

    /*
     * =========================================================================
     * 处理最后 3 Byte
     * =========================================================================
     * STM32F4 Master Receiver 在最后 3 Byte 时， 不能继续按照前面的普通 RXNE 流程一直读到底。
     * 这里需要开始控制：    BTF    ACK   STOP
     */

    status = prvWaitFlagSet(I2C_FLAG_BTF);//等待第一次 BTF

    if (status != AHT20_PORT_OK)
    {
        /*
         * 出错时尝试释放总线。
         */
        (void)prvStop();

        /*
         * 恢复 ACK / POS 默认状态。
         */
        prvRestoreReceiverConfig();

        return status;
    }

    I2C_AcknowledgeConfig(AHT20_PORT_I2C, DISABLE); //关闭 ACK

    data[index] = I2C_ReceiveData(AHT20_PORT_I2C); //读取倒数第 3 个 Byte

    ++index;

    --remaining;

    status = prvWaitFlagSet(I2C_FLAG_BTF);  //再等待一次 BTF

    if (status != AHT20_PORT_OK)
    {
        (void)prvStop();
        prvRestoreReceiverConfig();

        return status;
    }

    I2C_GenerateSTOP(AHT20_PORT_I2C, ENABLE);//产生 STOP

    data[index] = I2C_ReceiveData(AHT20_PORT_I2C); //读取倒数第 2 个 Byte

    ++index;
    --remaining;

    status = prvWaitFlagSet(I2C_FLAG_RXNE); //读取最后 1 个 Byte

    if (status != AHT20_PORT_OK)
    {
        prvRestoreReceiverConfig();

        return status;
    }

    data[index] = I2C_ReceiveData(AHT20_PORT_I2C);

    status = prvWaitBusIdle(); //等待 STOP 完成 确认 I2C 总线最终回到 Idle。

    prvRestoreReceiverConfig(); //恢复 Receiver 默认配置


    /*
     * 如果 STOP / Bus Idle 正常，
     * status 就是 AHT20_PORT_OK。
     *
     * 如果等待总线空闲失败，
     * 就把对应错误返回给上层。
     */
    return status;
}





/*
 * ============================================================================
 * 6. 私有硬件初始化函数
 * ============================================================================
 */


static void prvGPIOInit(void)
{
    GPIO_InitTypeDef gpio_init;

    RCC_AHB1PeriphClockCmd(AHT20_PORT_GPIO_RCC, ENABLE);

    GPIO_PinAFConfig(AHT20_PORT_GPIO, AHT20_PORT_SCL_SOURCE, AHT20_PORT_GPIO_AF);

    GPIO_PinAFConfig(AHT20_PORT_GPIO, AHT20_PORT_SDA_SOURCE,  AHT20_PORT_GPIO_AF);

    GPIO_StructInit(&gpio_init);

    gpio_init.GPIO_Pin = AHT20_PORT_SCL_PIN | AHT20_PORT_SDA_PIN;

    gpio_init.GPIO_Mode = GPIO_Mode_AF;

    gpio_init.GPIO_OType = GPIO_OType_OD;

    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;

    gpio_init.GPIO_PuPd = GPIO_PuPd_NOPULL;

    GPIO_Init(AHT20_PORT_GPIO, &gpio_init);
}


static void prvI2CInit(void)
{
    I2C_InitTypeDef i2c_init;

    RCC_APB1PeriphClockCmd(AHT20_PORT_I2C_RCC, ENABLE);  //开启 I2C2 外设时钟
    
    I2C_DeInit(AHT20_PORT_I2C);  //将 I2C2 恢复到默认状态

    I2C_StructInit(&i2c_init);  //给 I2C 配置结构体填默认值

    i2c_init.I2C_Mode = AHT20_PORT_I2C_MODE;  //选择标准 I2C 模式

    i2c_init.I2C_DutyCycle = AHT20_PORT_I2C_DUTY_CYCLE;  //设置 SCL Duty Cycle

    i2c_init.I2C_OwnAddress1 = AHT20_PORT_I2C_OWN_ADDRESS;  //设置 STM32 自己的 I2C 地址

    i2c_init.I2C_Ack = AHT20_PORT_I2C_ACK;   //配置 ACK

    i2c_init.I2C_AcknowledgedAddress = AHT20_PORT_I2C_ADDRESS_MODE;  //使用 7-bit 地址模式

    i2c_init.I2C_ClockSpeed = AHT20_PORT_I2C_SPEED_HZ;  //设置真正的 I2C 通信速度

    I2C_Init(AHT20_PORT_I2C, &i2c_init);  //将配置写入 I2C2 硬件

    I2C_Cmd(AHT20_PORT_I2C, ENABLE);  //正式使能 I2C2
}


static uint8_t prvMakeAddressByte(uint8_t address_7bit)
{
    return (uint8_t)(address_7bit << AHT20_PORT_I2C_ADDRESS_SHIFT);
}


static AHT20_PortStatus_t prvWaitBusIdle(void)
{
    uint32_t timeout;
    /*
     * 初始化软件超时计数器。
     */
    timeout = AHT20_PORT_TIMEOUT_LOOPS;
    /*
     * 只要 BUSY Flag 还是 SET， 就说明 I2C 总线仍然忙。
     */
    while (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_BUSY) == SET)
    {
        /*
         * 防止永远卡死。
         */
        if (timeout == 0UL)
        {
            return AHT20_PORT_ERROR_BUSY;
        }

        --timeout;
    }


    /*
     * BUSY 已经清零。 总线空闲。
     */
    return AHT20_PORT_OK;
}


static AHT20_PortStatus_t prvWaitEvent(uint32_t event)
{
    uint32_t timeout;

    timeout = AHT20_PORT_TIMEOUT_LOOPS;

    /*
     * 等待指定 I2C Event 出现。
     *STM32 的 I2C 硬件内部会产生很多状态 比如START已经生成、地址已经ACK、一个Byte已经发送完、收到一个Byte
     *STM32 标准库把一些常见状态组合定义成：Event
     *比如 I2C_EVENT_MASTER_MODE_SELECT 可以理解成START 已经成功产生，并且 STM32 已经进入 Master 状态
     */
    while (I2C_CheckEvent(AHT20_PORT_I2C, event) == ERROR)
    {
        if (timeout == 0UL)
        {
            return AHT20_PORT_ERROR_TIMEOUT;
        }

        --timeout;
    }


    return AHT20_PORT_OK;
}


static AHT20_PortStatus_t prvWaitAddressAck(uint32_t event)   //当没有设备时 返回NACK
{
    uint32_t timeout;

    timeout = AHT20_PORT_TIMEOUT_LOOPS;

    while (I2C_CheckEvent(AHT20_PORT_I2C, event) == ERROR)
    {
        /*
         * --------------------------------------------------------
         * 检查 ACK Failure   ACK是一个从机应答的标志 当没有ACK即NACK时 STM32的硬件 AF会置1
         * --------------------------------------------------------
         * 如果 AF 被置位，说明对方没有 ACK。
         */
        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_AF) == SET)
        {
            /*
             * AF 是一个错误状态， 使用完后必须清除。
             */
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_AF);

            return AHT20_PORT_ERROR_NACK;
        }


        /*
         * --------------------------------------------------------
         * 防止无限等待
         * --------------------------------------------------------
         */
        if (timeout == 0UL)
        {
            return AHT20_PORT_ERROR_TIMEOUT;
        }

        --timeout;
    }


    return AHT20_PORT_OK;
}


static AHT20_PortStatus_t prvStop(void)   //让I2C stop
{
    uint32_t timeout;

    /*
     * 产生 STOP 条件。
     */
    I2C_GenerateSTOP(AHT20_PORT_I2C,ENABLE);

    /*
     * 等待 BUSY Flag 清除。
     * BUSY 清零以后，才说明本次事务真正结束。
     */
    timeout = AHT20_PORT_TIMEOUT_LOOPS;

    while (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_BUSY) == SET)
    {
        if (timeout == 0UL)
        {
            return AHT20_PORT_ERROR_TIMEOUT;
        }

        --timeout;
    }

    return AHT20_PORT_OK;
}


static AHT20_PortStatus_t prvWaitByteTransmitted(void)
{
    uint32_t timeout = AHT20_PORT_TIMEOUT_LOOPS;

    while (I2C_CheckEvent(AHT20_PORT_I2C, I2C_EVENT_MASTER_BYTE_TRANSMITTED) == ERROR)//BYTE_TRANSMITTED是硬件已经发送完了
    {
        /*
         * AF = Acknowledge Failure。
         * 如果 Slave 没有 ACK 当前发送的数据， STM32 会把 AF 置位。
         */
        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_AF) == SET)
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_AF);
            return AHT20_PORT_ERROR_NACK;
        }

        /*
         * Bus Error。
         */
        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_BERR) == SET)
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_BERR);
            return AHT20_PORT_ERROR_HW;
        }

        /*
         * Arbitration Lost。
         *
         * 当前只有一个 Master，正常情况下基本不会发生，但驱动层仍然处理。
         */
        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_ARLO) == SET)
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_ARLO);
            return AHT20_PORT_ERROR_HW;
        }

        if (timeout == 0UL)
        {
            return AHT20_PORT_ERROR_TIMEOUT;
        }

        --timeout;
    }

    return AHT20_PORT_OK;
}


static AHT20_PortStatus_t prvWaitAddressMatched(void)
{
    uint32_t timeout = AHT20_PORT_TIMEOUT_LOOPS;

    while (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_ADDR) == RESET)//ADDR置1 说明设备已经找到
    {
        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_AF) == SET)  //AF置1 说明对方无应答
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_AF);
            return AHT20_PORT_ERROR_NACK;
        }

        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_BERR) == SET)//BEER置1 说明I2C 总线协议时序出现异常
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_BERR);
            return AHT20_PORT_ERROR_HW;
        }

        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_ARLO) == SET)//ARLO置1 说明两个 Master 同时抢 I2C 总线
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_ARLO);
            return AHT20_PORT_ERROR_HW;
        }

        if (timeout == 0UL)
        {
            return AHT20_PORT_ERROR_TIMEOUT;
        }

        --timeout;
    }

    return AHT20_PORT_OK;
}


static AHT20_PortStatus_t prvWaitFlagSet(uint32_t flag)
{
    uint32_t timeout = AHT20_PORT_TIMEOUT_LOOPS;

    while (I2C_GetFlagStatus(AHT20_PORT_I2C, flag) == RESET)
    {
        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_BERR) == SET) //检查 Bus Error
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_BERR);

            return AHT20_PORT_ERROR_HW;
        }


        /*
         * --------------------------------------------------------------------
         * 检查 Arbitration Lost
         * --------------------------------------------------------------------
         *
         * ARLO：
         *
         *      Arbitration Lost
         *      仲裁丢失。
         *
         * 主要用于多 Master 的 I2C 总线。
         *
         * 当前项目只有 STM32F407 一个 Master，
         * 正常情况下基本不会出现，
         * 但 Port 层仍然进行保护。
         */
        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_ARLO) == SET)
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_ARLO);
            return AHT20_PORT_ERROR_HW;
        }


        /*
         * --------------------------------------------------------------------
         * 检查 Overrun
         * --------------------------------------------------------------------
         * OVR：
         *      Overrun / Underrun
         * 对我们当前的接收场景来说，可以先理解为：
         *      “新的数据已经来了，但是软件还没有及时处理前面的数据。”
         * 也就是说软件读取速度没有跟上 I2C 硬件。
         */
        if (I2C_GetFlagStatus(AHT20_PORT_I2C, I2C_FLAG_OVR) == SET)
        {
            I2C_ClearFlag(AHT20_PORT_I2C, I2C_FLAG_OVR);

            return AHT20_PORT_ERROR_HW;
        }


        /*
         * --------------------------------------------------------------------
         * 检查软件超时
         * --------------------------------------------------------------------
         */
        if (timeout == 0UL)
        {
            return AHT20_PORT_ERROR_TIMEOUT;
        }


        /*
         * 每循环一次，
         * 超时计数器减 1。
         */
        --timeout;
    }


    /*
     * 能够离开 while()，
     * 说明目标 Flag 已经变成 SET。
     */
    return AHT20_PORT_OK;
}


static void prvClearAddrFlag(void)
{
    /*
     * dummy：
     * 这里只是临时接收寄存器读出来的值。
     * 我们并不关心 SR1 / SR2 的具体数值，真正需要的是“读取 SR1，再读取 SR2”这个动作本身。
     * volatile：告诉编译器这些读取操作不能被随意优化掉。
     */
    volatile uint32_t dummy;


    /*
     * ========================================================================
     * 读取 SR1
     * ========================================================================
     * SR1：  Status Register 1   状态寄存器 1
     *  ADDR Flag 就位于 I2C 的状态寄存器中。
     * STM32F4 硬件规定：    清除 ADDR 的第一步必须先读取 SR1。
     */
    dummy = AHT20_PORT_I2C->SR1;


    /*
     * ========================================================================
     * 读取 SR2
     * ========================================================================
     * SR2： Status Register 2  状态寄存器 2
     * 在已经读取 SR1 的前提下，再读取 SR2，
     * 硬件就会自动清除 ADDR Flag。
     */
    dummy = AHT20_PORT_I2C->SR2;


    /*
     * dummy 最终的数值本身没有实际用途。
     *
     * 写这一句只是明确告诉编译器：
     *
     *      “这个变量虽然没有继续参与计算，
     *       但这是有意为之。”
     *
     * 可以避免部分编译器产生
     * “变量赋值后未使用”之类的警告。
     */
    (void)dummy;
}


static void prvRestoreReceiverConfig(void)  //把 I2C 接收相关的 ACK/POS 设置恢复成下一次通信可以正常开始的默认状态。
{
    /*
     * ========================================================================
     * 恢复 NACK Position
     * ========================================================================
     * POS：  ACK/NACK 应该作用在当前 Byte，还是下一个 Byte。
     * 正常情况下恢复为 I2C_NACKPosition_Current也就是 ACK/NACK 针对当前接收 Byte。
     */
    I2C_NACKPositionConfig(AHT20_PORT_I2C, I2C_NACKPosition_Current);

    /*
     * ========================================================================
     *  恢复 ACK
     * ========================================================================
     * ACK Enable：   STM32 收到一个 Byte 后，正常向 Slave 返回 ACK，
     *    表示： “这个 Byte 我已经收到，可以继续发。” 最后一个 Byte 时我们会临时关闭 ACK。
     * 所以一次接收完成后，
     * 要重新把 ACK 打开。
     */
    I2C_AcknowledgeConfig(AHT20_PORT_I2C, ENABLE);
}



