#include <stdint.h>
#include <string.h>
#include "stm32f4xx.h"
#include "console.h"

static console_received_func_t received_func;  //保存一个回调函数，console_received_func_t是一个指针类型

void console_init(void)  //串口驱动的初始化
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);//开启GPIOA的时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);// 开启 USART1 的硬件时钟


    USART_InitTypeDef USART_InitStructure;
    USART_StructInit(&USART_InitStructure);

    USART_InitStructure.USART_BaudRate = 115200u;  //设置波特率
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;  //硬件流控 此处先不用
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStructure.USART_Parity = USART_Parity_No;  //校验位
    USART_InitStructure.USART_StopBits = USART_StopBits_1;  //停止位
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;  //数据位
    
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);  //两个引脚 选择AF映射
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);
    
    GPIO_InitTypeDef GPIO_InitStructure;  //配置GPIO
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;  //复用模式
    GPIO_InitStructure.GPIO_Speed = GPIO_High_Speed;  //高速
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_Init(USART1, &USART_InitStructure);  //USART应用配置
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);  //开启RXNE中断
    USART_Cmd(USART1, ENABLE);  //开启USART
    
    NVIC_InitTypeDef NVIC_InitStructure;  //配置NVIC
    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;  //选择中断通道
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;  //设置抢占优先级（数字越小优先级越高）
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;  //设置子优先级
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;  //使能NVIC通道
    NVIC_Init(&NVIC_InitStructure);  //NVIC应用配置
}

void console_write(const char str[])  //发送函数
{
    size_t len = strlen(str);  //计算要发送的字符串的长度
    for (size_t i = 0; i < len; i++)  //循环发送字符串的每个字符
    {
        USART_ClearFlag(USART1, USART_FLAG_TC);  //清除SUART1发送完毕的标志位
        USART_SendData(USART1, str[i]);  //发送一个字符
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);  //死等发送结束（也证明现在的console是阻塞式的）
    }
}

void console_received_register(console_received_func_t func)  //注册回调函数
{
    received_func = func;
}

void USART1_IRQHandler(void)  //USART1的中断函数，也就是说中断之后先进这个函数
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)  //检查RX是否真的置位 若没有说明接收到了新数据则进if
    {
        if (received_func != NULL)  //检查有没有人注册回调函数
        {
            uint8_t data = USART_ReceiveData(USART1);  //把接收到的函数读出来 读到data里边
            received_func(data); 
        }
        
        USART_ClearITPendingBit(USART1, USART_IT_RXNE);  //清除这次 RXNE 中断 pending 状态。
    }
}
