#ifndef CONSOLE_LOG_H
#define CONSOLE_LOG_H

/*
 * ============================================================================
 * console_log.h - 基于 FreeRTOS Mutex 的串口日志公共接口
 * ============================================================================
 *
 * 分层关系：
 *     AppTask / SensorTask / NetworkTask / ESP-AT Core
 *                      |
 *                      v
 *                ConsoleLog_Printf()
 *                      |
 *                FreeRTOS Mutex
 *                      |
 *                格式化整条日志
 *                      |
 *                 console_write()
 *                      |
 *                 USART1 / CH340
 *
 * 适用场合：正常 FreeRTOS 任务上下文。
 * 禁止场合：ISR 中断、HardFault、栈溢出 Hook、malloc failed Hook。
 *
 * 重要限制：
 *   1. 必须在启动 FreeRTOS 调度器之前初始化。
 *   2. 为控制 RAM，使用固定长度的静态缓冲区；过长日志会被截断。
 *   3. 串口仍采用原工程的同步发送方式；只是消除日志互相穿插，
 *      并没有变成异步 UART DMA 日志。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 返回 1：已初始化，互斥量可用。
 * 返回 0：创建互斥量失败，调用者应当停止系统启动。
 *
 * 应在 board_init() 中 console_init() 成功以后调用。
 */
uint8_t ConsoleLog_Init(void);

/*
 * printf 风格日志接口。
 *
 * 每次函数调用为一个互斥原子输出单元；不同任务的两次调用不会相互穿插。
 * 字符串中的换行建议使用 \r\n，便于串口工具显示。
 *
 * 返回值：
 *   >= 0：实际提交到 console_write() 的字符数，不含 '\0'。
 *   -1  ：参数非法/格式化失败/未初始化/不支持的调用时机。
 *   -2  ：等待日志互斥量超时，本条日志被丢弃（不影响业务数据）。
 *
 * 注意：在本 API 中无法保证底层 USART 故障时的有界发送时间；
 *       当前 console_write() 为轮询实现。
 */
int ConsoleLog_Printf(const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_LOG_H */
