// 对外接口
#ifndef __LED_H__
#define __LED_H__

#include <stdbool.h>
#include <stdint.h>

//「前向声明」 声明一个名为“led_desc”的结构体类型（是不完整类型） 注：不完整类型不能用来定义变量，但是可以用来定义指针，因为指针的大小是固定的（32 位系统 4 字节），和指向的内容无关。
struct led_desc;
 
// 类型重定义 把「指向`struct led_desc`结构体的指针」重命名为 `led_desc_t`
// 以后 `led_desc_t` 就等价于 `struct led_desc`，是一个指针类型，不是结构体类型。
typedef struct led_desc *led_desc_t;

// struct led_desc;和typedef struct led_desc *led_desc_t;就是「句柄」设计
// 对外只暴露指针类型，调用者只需要知道有这么一个 “LED 句柄”，不需要知道结构体内部长什么样，也不能直接修改里面的成员，只能通过我们提供的函数来操作，实现了封装和信息隐藏。



// 驱动函数的声明
void led_init(led_desc_t led);
void led_set(led_desc_t led, bool onoff);
void led_on(led_desc_t led);
void led_off(led_desc_t led);


#endif /* __LED_H__ */
