#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_config.h"

static const char *TAG = "HelloWorld";

DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint16_t *buffer = (uint16_t *)color_map;
    for(int y = area->y1; y <= area->y2; y++) 
    {
        for(int x = area->x1; x <= area->x2; x++) 
        {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            RlcdPort.RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    RlcdPort.RLCD_Display();
    lv_disp_flush_ready(drv);
}

static void create_hello_world_ui(void)
{
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Hello World!");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing display...");
    RlcdPort.RLCD_Init();
    
    ESP_LOGI(TAG, "Initializing LVGL...");
    Lvgl_PortInit(LCD_WIDTH, LCD_HEIGHT, Lvgl_FlushCallback);
    
    if(Lvgl_lock(-1)) {
        ESP_LOGI(TAG, "Creating Hello World UI...");
        create_hello_world_ui();
        Lvgl_unlock();
    }
    
    ESP_LOGI(TAG, "Hello World displayed on screen!");
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
