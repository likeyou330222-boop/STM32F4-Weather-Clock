#ifndef AHT20_H
#define AHT20_H


#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


/*
 * ============================================================================
 * AHT20 Core 层状态码
 * ============================================================================
 * 这里的状态码属于 AHT20 Driver 对上层公开的状态。
 * AppTask 不应该直接依赖：AHT20_PortStatus_t
 * 因为那个属于更底层的 Port。
 */
typedef enum
{
    AHT20_OK = 0,//操作成功

    AHT20_ERROR_PARAM, //参数错误

    AHT20_ERROR_NOT_INITIALIZED,//AHT20 尚未初始化

    AHT20_ERROR_I2C_BUSY,//I2C 总线长时间处于 Busy

    AHT20_ERROR_TIMEOUT,  //I2C 某个过程等待超时

    AHT20_ERROR_NACK,//I2C 从设备没有返回 ACK

    AHT20_ERROR_HW, //其他底层硬件错误

    AHT20_ERROR_SENSOR_BUSY,//AHT20 当前仍然处于测量 Busy 状态

    AHT20_ERROR_CRC  //AHT20 返回数据的 CRC 校验失败

} AHT20_Status_t;


/*
 * ============================================================================
 * AHT20 原始测量数据长度
 * ============================================================================
 * AHT20 一次完整测量读取返回 7 Byte：
 * Byte 0：Status  Byte 1： Byte 2：  Byte 3：湿度和温度数据的一部分
 * Byte 4：  Byte 5：温度数据 Byte 6：CRC
 */
#define AHT20_RAW_DATA_LENGTH    7U


/*
 * ============================================================================
 * AHT20 原始测量结果
 * ============================================================================
 * 先把传感器真正返回的 7 Byte 原样保存下来。
 */
typedef struct
{
    uint8_t bytes[AHT20_RAW_DATA_LENGTH];

} AHT20_RawMeasurement_t;


/*
 * ============================================================================
 * AHT20 最终测量结果
 * ============================================================================
 * 这个结构体和 AHT20_RawMeasurement_t 的职责不同。
 * AHT20_Measurement_t： 保存已经解析、换算完成的实际物理量。
 * humidity_percent：相对湿度，单位 %RH。temperature_celsius：  温度，单位 ℃。
 */
typedef struct
{
    float humidity_percent;
    float temperature_celsius;

} AHT20_Measurement_t;




/*
 * ============================================================================
 * AHT20 Core API
 * ============================================================================
 */
AHT20_Status_t AHT20_Init(void);  //初始化 AHT20 Driver（1.初始化底层 Port 2.检查 AHT20 是否存在 3.初始化成功后记录 Driver 状态）

//触发 AHT20 开始一次新的温湿度测量。  只发送开始测量命令
AHT20_Status_t AHT20_StartMeasurement(void);

/*
 * 读取 AHT20 最近一次测量产生的原始数据。
 * 注意：调用这个函数之前， 应该已经：
 * 1. 调用 AHT20_StartMeasurement()  2. 等待传感器完成测量
 * 当前数据手册要求测量命令后等待约 80 ms。本函数只负责读取 7 Byte，暂时不负责把数据换算成温度和湿度。
 */
AHT20_Status_t AHT20_ReadRawMeasurement(AHT20_RawMeasurement_t *measurement);

/*
 * 读取并解析最近一次 AHT20 测量结果。
 * 调用本函数之前必须已经：  1. AHT20_StartMeasurement()  2. 等待传感器完成测量
 * 本函数负责：  读取 7 Byte  检查 Sensor Busy  解析湿度和温度 CRC 校验。
 */
AHT20_Status_t AHT20_ReadMeasurement(AHT20_Measurement_t *measurement);




#ifdef __cplusplus
}
#endif


#endif /* AHT20_H */

