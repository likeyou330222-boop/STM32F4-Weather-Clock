#include <stdbool.h>
#include <stdint.h>
#include "led_desc.h"
#include "led.h"



// led: PC13


// LED 初始化函数 `led_init`
void led_init(led_desc_t led)
{
    GPIO_InitTypeDef GPIO_InitStructure;// 定义一个`GPIO_InitTypeDef`类型的结构体变量
    
    GPIO_StructInit(&GPIO_InitStructure);// 给 GPIO 初始化结构体赋默认值
    GPIO_InitStructure.GPIO_Pin = led->Pin;// 读取结构体led_desc_t led里边的pin
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(led->Port, &GPIO_InitStructure);
    
    GPIO_WriteBit(led->Port, led->Pin, led->OffBit);
}


// 通用设置函数 `led_set`
void led_set(led_desc_t led, bool onoff)
{
    GPIO_WriteBit(led->Port, led->Pin, onoff ? led->OnBit : led->OffBit);
}

void led_on(led_desc_t led)
{
    GPIO_WriteBit(led->Port, led->Pin, led->OnBit);
}

void led_off(led_desc_t led)
{
    GPIO_WriteBit(led->Port, led->Pin, led->OffBit);
}
