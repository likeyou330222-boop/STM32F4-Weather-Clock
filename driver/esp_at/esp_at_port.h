#ifndef ESP_AT_PORT_H
#define ESP_AT_PORT_H

#include <stdint.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif


/*
 * ============================================================================
 * ESP-AT Port 层状态码
 * ============================================================================
 * Port 层只描述 STM32 硬件相关错误。
 */
typedef enum
{
    ESP_AT_PORT_OK = 0,  //操作成功
    ESP_AT_PORT_ERROR_PARAM,  //参数错误
    ESP_AT_PORT_ERROR_NOT_INITIALIZED, //Port 尚未初始化
    ESP_AT_PORT_ERROR_BUSY,  //当前硬件正在忙
    ESP_AT_PORT_ERROR_TIMEOUT, //操作超时
    ESP_AT_PORT_ERROR_HW  //STM32 USART / DMA 等硬件错误
} ESP_AT_PortStatus_t;


/*
 * ============================================================================
 * USART2 TX DMA 发送状态
 * ============================================================================
 * IDLE：  尚未启动 DMA 发送。
 * BUSY：  DMA 正在进行数据传输。
 * DONE：  DMA 已经成功完成本次数据搬运。
 * ERROR： DMA 传输发生错误。
 */
typedef enum
{
    ESP_AT_PORT_TX_IDLE = 0,
    ESP_AT_PORT_TX_BUSY,
    ESP_AT_PORT_TX_DONE,
    ESP_AT_PORT_TX_ERROR,
    ESP_AT_PORT_TX_ABORTED    // 本次 DMA 被主动取消
} ESP_AT_PortTxState_t;


/*
 * ============================================================================
 * ESP_AT_Port_Init
 * ============================================================================
 * 初始化 ESP32-C3 使用的 STM32 硬件资源。
 * 后续完整版本会包括：
 *      GPIOA PA2 / PA3
 *      USART2
 *      DMA1 Stream6
 *      USART2 IRQ
 *      DMA IRQ
 *
 * 当前第一步先完成： GPIO + USART2
 */
ESP_AT_PortStatus_t ESP_AT_Port_Init(void);


/*
 * ============================================================================
 * ESP_AT_Port_WriteAsync   （Async是Asynchronous，异步。）
 * ============================================================================
 * 使用 USART2 TX DMA 异步发送一段数据。
 * data：  指向需要发送的数据。
 * length：  要发送的 Byte 数量。
 * 返回 ESP_AT_PORT_OK： 只表示 DMA 已经成功启动。
 * 注意：
 *      返回 ESP_AT_PORT_OK 时，并不表示数据已经全部从 PA2 发出。
 *      DMA 会在函数返回以后继续工作。
 */
ESP_AT_PortStatus_t ESP_AT_Port_WriteAsync(const uint8_t *data, size_t length);


/*
 * 查询 USART2 TX DMA 当前的发送状态。
 * 本函数不等待 DMA，只读取 Port 保存的状态。
 */
ESP_AT_PortTxState_t ESP_AT_Port_GetTxState(void);


/*
 * ============================================================================
 * ESP_AT_Port_AbortTx
 * ============================================================================
 * 功能：
 *     请求终止当前 USART2 TX DMA，并确认 DMA Stream 已经关闭。
 * 返回：
 *     ESP_AT_PORT_OK：DMA 已停止，可以不再担心它读取旧缓冲区。
 *     ESP_AT_PORT_ERROR_NOT_INITIALIZED：Port 尚未初始化。
 *     ESP_AT_PORT_ERROR_TIMEOUT：等待 DMA 停止超时。
 * 注意：
 *     本函数只能在任务上下文使用，不能在中断中调用。
 *     不负责清理 AT 响应，也不负责恢复 Core 命令状态。
 */
ESP_AT_PortStatus_t ESP_AT_Port_AbortTx(void);


/*
 * ============================================================================
 * ESP_AT_Port_Read
 * ============================================================================
 * 从 USART2 RX Ring Buffer 中读取已经接收到的数据。
 * data：   上层提供的目标 Buffer。
 * max_length：   本次最多允许读取多少 Byte。
 * read_length：   返回本次实际读取的 Byte 数量。
 * 本函数为非阻塞读取：
 *      有数据 -> 立即读取；
 *      没数据 -> *read_length = 0，然后立即返回。
 */
ESP_AT_PortStatus_t ESP_AT_Port_Read(uint8_t *data, size_t max_length, size_t *read_length);


/*
 * ============================================================================
 * ESP_AT_Port_DMAIRQHandler
 * ============================================================================
 * USART2 TX 使用 DMA1 Stream6。
 * 当 DMA1 Stream6 产生：
 *      Transfer Complete
 *      Transfer Error
 *      Direct Mode Error
 * 中断时，真正的 Cortex-M IRQ Handler 会调用这里。
 */
void ESP_AT_Port_DMAIRQHandler(void);


/*
 * ============================================================================
 * ESP_AT_Port_USARTIRQHandler
 * ============================================================================
 * USART2 接收中断处理函数。
 * 真正的 USART2_IRQHandler() 会调用这里。
 */
void ESP_AT_Port_USARTIRQHandler(void);




#ifdef __cplusplus
}
#endif


#endif

