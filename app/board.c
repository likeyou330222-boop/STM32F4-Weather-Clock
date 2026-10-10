//#include <stm32f4xx_rcc.h>
#include "stm32f4xx.h" // 引入 STM32F4 官方标准外设库的头文件
#include <stdio.h>     // 引入标准输入输出库，支持 printf 函数
#include <stdint.h>    // 引入标准整数类型库，支持如 uint8_t, uint32_t 等类型
#include "console_log.h" /* 初始化多任务日志互斥量，启动后统一使用 */
#include "console.h"     // 自定义头文件：用于调试串口 (USART1) 的初始化
#include "led_desc.h"
#include "led.h"

//#include "esp_at.h"    // 自定义头文件：用于 ESP8266/ESP32 的 AT 指令驱动
//#include "lcd.h"       // 自定义头文件：包含显示屏的初始化等
//#include "cpu_tick.h"  // 自定义头文件：用于 CPU 滴答定时器延时功能
//#include "rtc.h" 
//#include "aht20_spl.h"
//#include "app.h" 


void board_lowlevel_init(void)
{
     // --- 第一部分：系统外设时钟使能 ---
    // 开启 GPIOA, GPIOB, GPIOC, GPIOE 的硬件时钟 (挂载在 AHB1 高速总线)
    
    // RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    // RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    // RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);
    

//    // 开启 USART2 的硬件时钟 (挂载在 APB1 低速外设总线)，用于和 WiFi 模块通信
//    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
//	  RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE); //使能SPI1时钟

//    RCC_APB1PeriphClockCmd(AHT20_I2C_CLK, ENABLE);  // 开启 I2C1 时钟
//    RCC_AHB1PeriphClockCmd(AHT20_GPIO_CLK, ENABLE); // 开启 GPIOB 时钟

//    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
//    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
//    PWR_BackupAccessCmd(ENABLE);
//    RCC_LSEConfig(RCC_LSE_ON);
//    while (RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET);
//    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);


    
}


void board_init(void)//模块初始化
{
    
//    LCD_Init();      // 初始化显示屏幕
    console_init();    /* 配置 USART1（PA9/PA10），供 CH340 调试终端使用。 */

    /*
     * FreeRTOS Mutex 可以在调度器启动前创建。
     * 所有任务启动后调用 ConsoleLog_Printf()，就能对整条日志互斥。
     *
     * 注意：这里必须在 console_init() 之后，并且在
     * vTaskStartScheduler() 之前执行。
     */
    if (ConsoleLog_Init() == 0U)
    {
        /* 创建 Mutex 失败是无法安全启动日志系统的致命初始化错误。 */
        console_write("[BOOT][FATAL] ConsoleLog mutex init failed\r\n");
        for (;;)
        {
            /* 启动阶段没有调度器，不使用 vTaskDelay()。 */
        }
    }
//    AHT20_I2C_Init();
//    rtc_init();
//    wireless_init();
    printf("[SYS] Build Date: %s %s\n",__DATE__,__TIME__); // 在调试终端打印系统启动提示

}
// C 标准库底层的字符输出重定向函数
// 当代码中调用 printf 时，最终会逐个字符调用 fputc 投递数据
int fputc(int ch, FILE *f)
{
    // 将待发送的字符强制转换为 uint8_t 并送入 USART1 数据寄存器
    USART_SendData(USART1, (uint8_t)ch);
    // 轮询等待 USART1 的发送数据寄存器为空 (TXE 标志位)，确保当前字符发送完毕
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    return ch; // 按照标准，返回刚刚发送的字符
}

// led: PC13
static struct led_desc _led1 = { GPIOC, GPIO_Pin_13, Bit_RESET, Bit_SET };
led_desc_t led1 = &_led1;

