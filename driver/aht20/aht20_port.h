#ifndef AHT20_PORT_H
#define AHT20_PORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 * ============================================================================
 * AHT20 Port 层状态码
 * ============================================================================
 * Port 层只描述： STM32 与 I2C 总线这一层发生了什么。
 * 它不知道：
 *      温度是多少   湿度是多少   0xAC 是什么命令
 * 那些都属于以后 aht20.c 的 Core 层。
 */
typedef enum
{
    AHT20_PORT_OK = 0,  //操作成功

    AHT20_PORT_ERROR_PARAM,  //调用者传入的参数不合法

    AHT20_PORT_ERROR_NOT_INITIALIZED,  //Port 层尚未完成初始化

    AHT20_PORT_ERROR_BUSY,  //I2C 总线当前处于忙状态

    AHT20_PORT_ERROR_TIMEOUT,  //某个 I2C 硬件阶段等待超时

    AHT20_PORT_ERROR_NACK,  //对方没有返回 ACK

    AHT20_PORT_ERROR_HW  //其他硬件异常

} AHT20_PortStatus_t;


/*
 * ============================================================================
 * Port 层公开 API
 * ============================================================================
 */
/*
 * 初始化当前平台上的 I2C 硬件。
 */
AHT20_PortStatus_t AHT20_Port_Init(void);


/*
 * 反初始化 Port。
 */
AHT20_PortStatus_t AHT20_Port_DeInit(void);


/*
 * 检查某个 7-bit I2C 地址的设备是否存在。
 * 注意：
 * address_7bit 传入真正的 7-bit 地址。
 * 对 AHT20 来说： address_7bit = 0x38
 * 上层不需要传： 0x70 也不需要传： 0x71
 * 左移和 R/W 位的处理交给 Port 层。
 */
AHT20_PortStatus_t AHT20_Port_Probe(uint8_t address_7bit);


/*
 * 向某个 I2C 从设备写入若干 Byte。
 * address_7bit： 设备真正的 7-bit 地址。
 * data：   要发送的数据首地址。
 * length： 要发送多少 Byte。
 */
AHT20_PortStatus_t AHT20_Port_Write(
    uint8_t address_7bit,
    const uint8_t *data,
    size_t length
);


/*
 * 从某个 I2C 从设备读取若干 Byte。
 */
AHT20_PortStatus_t AHT20_Port_Read(
    uint8_t address_7bit,
    uint8_t *data,
    size_t length
);


#ifdef __cplusplus
}
#endif


#endif /* AHT20_PORT_H */

