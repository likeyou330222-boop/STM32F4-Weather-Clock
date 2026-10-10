#include "stm32f4xx.h"
#include "misc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ili9341.h"
#include "ili9341_port.h"
#include "ili9341_gfx.h"
#include "ili9341_assets.h"
#include "ili9341_freertos.h"
#include <stdint.h>
#include <stddef.h>
#include "page.h"



void welcome_page_display(void)
{
    /*
     * DMA 状态。
     */
    ILI9341_Status_t status;

    /*
     * 当前颜色。
     */
    uint16_t color;

    /*
     * 本 Task 不使用参数。
     */
    (void)argument;


    /*
     * 初始黑色。
     */
    color = ILI9341_GFX_COLOR_BLACK;


    /*
     * 周期刷新 A 区。
     */
    for (;;)
    {
        /*
         * 先在 A 自己的 Buffer 中准备一整帧。
         */
        prvFillWireColor(
            &s_region_a_buffer[0],
            (uint32_t)REGION_WIDTH * (uint32_t)REGION_HEIGHT,
            color
        );


        /*
         * 然后 DMA 到 A 区。
         */
        status = ILI9341_RTOS_WriteWindowBytesDMA(
            REGION_A_X,
            REGION_A_Y,
            REGION_WIDTH,
            REGION_HEIGHT,
            &s_region_a_buffer[0],
            REGION_BYTES,
            pdMS_TO_TICKS(100U),
            pdMS_TO_TICKS(100U)
        );


        /*
         * 失败时停止。
         */
        if (status != ILI9341_OK)
        {
            prvFatalStop();
        }


        /*
         * 红 / 黄交替。
         */
        if (color == ILI9341_GFX_COLOR_RED)
        {
            color = ILI9341_GFX_COLOR_YELLOW;
        }
        else
        {
            color = ILI9341_GFX_COLOR_RED;
        }


        /*
         * A 每 250 ms 更新一次。
         */
        vTaskDelay(
            pdMS_TO_TICKS(250U)
        );
    }
}
