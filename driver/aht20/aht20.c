#include "aht20.h"

#include "aht20_port.h"

#include <stddef.h>


/*
 * ============================================================================
 * AHT20 协议常量
 * ============================================================================
 */
/*
 * AHT20 的真正 7-bit I2C 地址。
 * 数据手册在线路上给出的：
 * Write = 0x70
 * Read  = 0x71
 * 去掉最低的 R/W 位以后：
 * 0x70 >> 1 = 0x38
 * 因此 Core 层只保存真正的： 7-bit Address = 0x38
 */
#define AHT20_I2C_ADDRESS_7BIT        0x38U

#define AHT20_CMD_TRIGGER_MEASUREMENT     0xACU  //AHT20 测量命令

#define AHT20_CMD_MEASUREMENT_PARAM_1     0x33U  //AHT20 测量命令

#define AHT20_CMD_MEASUREMENT_PARAM_2     0x00U  //AHT20 测量命令

/*
 * ============================================================================
 * AHT20 Status Byte 位掩码
 * ============================================================================
 * Status Byte 的 Bit7：  1 -> AHT20 仍然处于测量 Busy 状态   0 -> AHT20 当前空闲，测量已经完成
 * 二进制：  1000 0000     十六进制：   0x80
 */
#define AHT20_STATUS_BUSY_MASK             0x80U

/*
 * ============================================================================
 * AHT20 数据换算参数
 * ============================================================================
 * AHT20 的湿度和温度原始数据都是 20 bit。
 * 2^20 = 1048576  因此换算公式中的满量程值就是 1048576。
 */
#define AHT20_RAW_DATA_FULL_SCALE          1048576.0f

/*
 * 相对湿度满量程：  100 %RH
 */
#define AHT20_HUMIDITY_FULL_SCALE          100.0f

/*
 * 温度换算参数下一步详细讲。
 */
#define AHT20_TEMPERATURE_FULL_SCALE       200.0f
#define AHT20_TEMPERATURE_OFFSET           50.0f

/*
 * ============================================================================
 * AHT20 CRC8 参数
 * ============================================================================
 * 数据手册规定： CRC 初始值：  0xFF
 *      CRC8 多项式：  X^8 + X^5 + X^4 + 1
 *      对应十六进制：    0x31
 * AHT20 一次返回 7 Byte：
 *      bytes[0]  Status   bytes[1]  Humidity  bytes[2]  Humidity
 *      bytes[3]  Humidity + Temperature  bytes[4]  Temperature
 *      bytes[5]  Temperature    bytes[6]  CRC
 * 因此 CRC 实际计算：   bytes[0] ~ bytes[5]一共 6 Byte。
 */
#define AHT20_CRC_INITIAL_VALUE           0xFFU  //CRC初始值
#define AHT20_CRC_POLYNOMIAL              0x31U  //CRC8 多项式对应的数值
#define AHT20_CRC_MSB_MASK                0x80U  //检查高位bit7

#define AHT20_CRC_DATA_LENGTH             6U   //参与 CRC 计算的数据长度是 6 Byte
#define AHT20_CRC_BYTE_INDEX              6U   //CRC 这个 Byte 在数组里的下标




/*
 * ============================================================================
 * AHT20 Core 内部状态
 * ============================================================================
 */
/*
 * 记录 AHT20 Core 是否已经成功初始化。
 * 0U： 未初始化
 * 1U： 已初始化
 * static： 只允许当前 aht20.c 使用。
 */
static uint8_t s_initialized = 0U;




/*
 * ============================================================================
 * 私有函数声明
 * ============================================================================
 */
static AHT20_Status_t prvMapPortStatus(AHT20_PortStatus_t port_status);//把 Port 层状态转换成 Core 层状态

//提取 20-bit 相对湿度原始值 SRH[19:0]
static uint32_t prvExtractHumidityRaw(const AHT20_RawMeasurement_t *raw_measurement);  

//从 AHT20 的 7 Byte 原始数据中，提取 20-bit 温度原始值 ST[19:0]。
static uint32_t prvExtractTemperatureRaw(const AHT20_RawMeasurement_t *raw_measurement);


//根据 AHT20 数据手册计算 CRC8。data：指向需要进行 CRC 计算的数据。 length：参与计算的数据长度。
// 返回： 最终计算得到的 CRC8。
static uint8_t prvCalculateCRC8(const uint8_t *data, size_t length);




/*
 * ============================================================================
 * AHT20 Core 初始化
 * ============================================================================
 */
AHT20_Status_t AHT20_Init(void)
{
    AHT20_PortStatus_t port_status;
    if (s_initialized != 0U)  //如果已经初始化成功，不重复初始化
    {
        return AHT20_OK;
    }
    port_status = AHT20_Port_Init();  //初始化底层 Port

    if (port_status != AHT20_PORT_OK)
    {
        return prvMapPortStatus(port_status);
    }

    port_status = AHT20_Port_Probe(AHT20_I2C_ADDRESS_7BIT); //检查 0x38 地址的 AHT20 是否存在

    if (port_status != AHT20_PORT_OK)
    {
        return prvMapPortStatus( port_status);
    }

    s_initialized = 1U; //Port 正常，而且 AHT20 已经 ACK。现在才能认为 AHT20 Core 初始化成功

    return AHT20_OK;
}


AHT20_Status_t AHT20_StartMeasurement(void)
{
    AHT20_PortStatus_t port_status;

    const uint8_t command[] =
    {
        AHT20_CMD_TRIGGER_MEASUREMENT,
        AHT20_CMD_MEASUREMENT_PARAM_1,
        AHT20_CMD_MEASUREMENT_PARAM_2
    };

    if (s_initialized == 0U)   //确认 AHT20 已经初始化
    {
        return AHT20_ERROR_NOT_INITIALIZED;
    }

    port_status = AHT20_Port_Write(AHT20_I2C_ADDRESS_7BIT, command, sizeof(command));  //向 AHT20 发送测量命令

    if (port_status != AHT20_PORT_OK)  //如果 Port 出错，映射成 Core 错误返回给上层
    {
        return prvMapPortStatus(port_status);
    }

    return AHT20_OK;
}


AHT20_Status_t AHT20_ReadRawMeasurement(AHT20_RawMeasurement_t *measurement)
{
    AHT20_PortStatus_t port_status;

    uint8_t calculated_crc;

    if (s_initialized == 0U)  //检查 AHT20 Core 是否已经初始化
    {
        return AHT20_ERROR_NOT_INITIALIZED;
    }

    if (measurement == NULL)  //检查输出参数 measurement 是一个指针不能为空
    {
        return AHT20_ERROR_PARAM;
    }

    

    port_status = AHT20_Port_Read(
        AHT20_I2C_ADDRESS_7BIT,
        measurement->bytes,
        AHT20_RAW_DATA_LENGTH
    );  //从 AHT20 读取完整的 7 Byte 原始数据  把数据从 measurement->bytes[0] 开始依次存进去。

    if (port_status != AHT20_PORT_OK)  //若Port 层发生错误
    {
        return prvMapPortStatus(port_status);
    }



    if ((measurement->bytes[0] & AHT20_STATUS_BUSY_MASK) != 0U)  //检查 Status Byte 的 Busy Bit
    {
        return AHT20_ERROR_SENSOR_BUSY;
    }


    /*
     * =========================================================================
     * 计算本次接收数据的 CRC8
     * =========================================================================
     * AHT20 返回： bytes[0] ~ bytes[5]是参与 CRC 计算的数据。 
     * bytes[6] 是 AHT20 自己计算并返回的 CRC。
     */
    calculated_crc = prvCalculateCRC8(measurement->bytes, AHT20_CRC_DATA_LENGTH);

    /*
     * =========================================================================
     * 比较 CRC
     * =========================================================================
     * STM32 自己算出来：  calculated_crc
     * AHT20 返回：   measurement->bytes[6]
     * 如果两者不一致，说明这一帧数据不能认为是可靠的。
     */
    if (calculated_crc != measurement->bytes[AHT20_CRC_BYTE_INDEX])
    {
        return AHT20_ERROR_CRC;
    }

    /*
     * I2C 通信成功；Sensor Busy = 0；CRC 校验通过。
     * 这份原始测量数据才真正可以交给后面的解析流程。
     */
    return AHT20_OK;

}


AHT20_Status_t AHT20_ReadMeasurement(AHT20_Measurement_t *measurement)
{
    AHT20_RawMeasurement_t raw_measurement;
    AHT20_Status_t status;

    uint32_t raw_humidity;
    uint32_t raw_temperature;

    if (measurement == NULL)  //检查输出参数
    {
        return AHT20_ERROR_PARAM;
    }

    status = AHT20_ReadRawMeasurement(&raw_measurement); //读取 AHT20 的 7 Byte 原始数据

    if (status != AHT20_OK)
    {
        return status;
    }

    raw_humidity = prvExtractHumidityRaw(&raw_measurement);  //提取 20-bit 湿度原始值

    raw_temperature = prvExtractTemperatureRaw(&raw_measurement); //提取 20-bit 温度原始值

    /*
     * =========================================================================
     * 原始湿度 -> %RH
     * =========================================================================
     * RH =
     *      RawHumidity
     *      ----------- × 100
     *        2^20
     */
    measurement->humidity_percent = ((float)raw_humidity / AHT20_RAW_DATA_FULL_SCALE) * AHT20_HUMIDITY_FULL_SCALE;

    /*
     * =========================================================================
     * 原始温度 -> ℃
     * =========================================================================
     * 数据手册：    T = ST / 2^20 × 200 - 50
     */
    measurement->temperature_celsius = ((float)raw_temperature / AHT20_RAW_DATA_FULL_SCALE) * AHT20_TEMPERATURE_FULL_SCALE - AHT20_TEMPERATURE_OFFSET;

    return AHT20_OK;
}




/*
 * ============================================================================
 * Port Status -> Core Status
 * ============================================================================
 */
static AHT20_Status_t prvMapPortStatus(AHT20_PortStatus_t port_status)
{
    switch (port_status)
    {
        case AHT20_PORT_OK:  //Port 成功
        {
            return AHT20_OK;
        }

        case AHT20_PORT_ERROR_PARAM: //参数错误
        {
            return AHT20_ERROR_PARAM;
        }

        case AHT20_PORT_ERROR_NOT_INITIALIZED://Port 尚未初始化
        {
            return AHT20_ERROR_NOT_INITIALIZED;
        }

        case AHT20_PORT_ERROR_BUSY:  //I2C Bus Busy
        {
            return AHT20_ERROR_I2C_BUSY;
        }

        case AHT20_PORT_ERROR_TIMEOUT:  //等待超时
        {
            return AHT20_ERROR_TIMEOUT;
        }

        case AHT20_PORT_ERROR_NACK:   //Slave 没有 ACK
        {
            return AHT20_ERROR_NACK;
        }

        case AHT20_PORT_ERROR_HW:  //其他硬件故障
        {
            return AHT20_ERROR_HW;
        }

        /*
         * 理论上不应该进入这里。
         * 但是为了防止以后 Port 新增状态码后 Core 忘记同步更新， 必须保留 default。
         */
        default:
        {
            return AHT20_ERROR_HW;
        }
    }
}


static uint32_t prvExtractHumidityRaw(const AHT20_RawMeasurement_t *raw_measurement)
{
    uint32_t raw_humidity;


    /*
     * AHT20 的 20-bit 湿度原始数据：
     *      bytes[1]   -> RH[19:12]
     *      bytes[2]   -> RH[11:4]
     *      bytes[3] 的 Bit7:4  -> RH[3:0]
     * 所以：
     *bytes[1] 左移 12 位  bytes[2] 左移 4 位    bytes[3] 右移 4 位
     * 最后通过按位或 '|' 拼成完整的 20 bit。
     */
    raw_humidity = ((uint32_t)raw_measurement->bytes[1] << 12)
                 | ((uint32_t)raw_measurement->bytes[2] << 4)
                 | ((uint32_t)raw_measurement->bytes[3] >> 4);

    return raw_humidity;
}


static uint32_t prvExtractTemperatureRaw(const AHT20_RawMeasurement_t *raw_measurement)
{
    uint32_t raw_temperature;

    /*
     * AHT20 温度的 20-bit 原始数据：
     *      bytes[3] Bit3:0 -> T[19:16]
     *      bytes[4]        -> T[15:8]
     *      bytes[5]        -> T[7:0]
     */
    raw_temperature = ((uint32_t)(raw_measurement->bytes[3] & 0x0FU) << 16)
                    | ((uint32_t)raw_measurement->bytes[4] << 8)
                    | ((uint32_t)raw_measurement->bytes[5]);

    return raw_temperature;
}


static uint8_t prvCalculateCRC8(const uint8_t *data, size_t length)
{
    uint8_t crc;
    uint8_t bit_index;
    size_t byte_index;
    /*
     * =========================================================================
     * 1. 设置 CRC 初始值
     * =========================================================================
     * AHT20 数据手册规定：   CRC Initial Value = 0xFF
     */
    crc = AHT20_CRC_INITIAL_VALUE;

    /*
     * =========================================================================
     * 2. 逐 Byte 处理数据
     * =========================================================================
     * AHT20 一次测量的 CRC 会对前 6 Byte 进行计算。
     * 这里函数本身写成通用形式：  data  length
     * 所以后面并不把 “6 Byte” 写死在函数内部。
     */
    for (byte_index = 0U; byte_index < length; ++byte_index)
    {
        /*
         * ---------------------------------------------------------------------
         * 当前数据 Byte 与 CRC 做 XOR
         * ---------------------------------------------------------------------
         * ^=：  crc = crc ^ data[byte_index];
         * '^' 是按位异或 XOR。
         */
        crc ^= data[byte_index];
        /*
         * ---------------------------------------------------------------------
         * 每一个 Byte 都包含 8 bit
         * ---------------------------------------------------------------------
         * 因此当前 Byte 与 CRC 合并以后， 需要连续执行 8 次 CRC 位运算。
         */
        for (bit_index = 0U; bit_index < 8U; ++bit_index)
        {
            /*
             * 检查当前 CRC 的最高位 Bit7。
             * 0x80：   1000 0000
             */
            if ((crc & AHT20_CRC_MSB_MASK) != 0U)
            {
                /*
                 * 如果 Bit7 = 1：
                 *      先左移 1 bit
                 *      再与多项式 0x31 做 XOR
                 */
                crc = (uint8_t)((crc << 1) ^ AHT20_CRC_POLYNOMIAL);
            }
            else
            {
                /*
                 * 如果 Bit7 = 0：
                 *
                 *      只需要左移 1 bit。
                 */
                crc = (uint8_t)(crc << 1);
            }
        }
    }


    /*
     * 返回最终 CRC8。
     */
    return crc;
}


