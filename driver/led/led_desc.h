// 最底层的头文件 ，专门用来定义LED的硬件属性结构体
#ifndef __LED_DESC_H__
#define __LED_DESC_H__

#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx.h"

// 定义LED的 硬件描述符结构体 （是整个驱动的核心数据结构，把一个 LED 的所有硬件属性打包在一起）
// Port：指向 GPIO 外设的指针，STM32 里每个 GPIO 端口都对应一个GPIO_TypeDef类型的结构体基地址，比如GPIOC就是GPIOC端口的基地址。

struct led_desc
{
    GPIO_TypeDef* Port;
    uint32_t Pin;
    BitAction OnBit;
    BitAction OffBit;
};

#endif /* __LED_DESC_H__ */
