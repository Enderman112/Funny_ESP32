#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_config.h"
#include "button_bsp.h"
#include "wifi_bsp.h"
#include "web_server.h"

static const char *TAG = "HelloWorld";

DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

// Menu state
static bool menu_visible = false;
static int menu_selected = 0;
static const int menu_count = 2;
static const char* menu_items[] = {"Hello World", "Info"};
static lv_obj_t *menu_panel = NULL;
static lv_obj_t *menu_labels[2] = {NULL};
static lv_obj_t *hello_label = NULL;

static void update_menu_ui(void)
{
    if (!menu_panel) return;
    
    for (int i = 0; i < menu_count; i++) {
        if (menu_labels[i]) {
            if (i == menu_selected) {
                lv_obj_set_style_bg_color(menu_labels[i], lv_color_hex(0x0000FF), 0);
                lv_obj_set_style_text_color(menu_labels[i], lv_color_hex(0xFFFFFF), 0);
            } else {
                lv_obj_set_style_bg_color(menu_labels[i], lv_color_hex(0xCCCCCC), 0);
                lv_obj_set_style_text_color(menu_labels[i], lv_color_hex(0x000000), 0);
            }
        }
    }
}

static void execute_menu_item(void)
{
    if (Lvgl_lock(-1)) {
        switch (menu_selected) {
            case 0: // Hello World
                lv_label_set_text(hello_label, "Hello World!");
                break;
            case 1: // Info
                {
                    char info_buf[128];
                    snprintf(info_buf, sizeof(info_buf),
                             "Funny ESP32\n\n"
                             "Current: %s\n"
                             "Latest:  %s\n\n"
                             "ESP32-S3 RLCD 4.2\"\n"
                             "400 x 300 px",
                             FIRMWARE_VERSION,
                             wifi_bsp_get_latest_version());
                    lv_label_set_text(hello_label, info_buf);
                }
                break;
        }
        
        // Hide menu
        if (menu_panel) {
            lv_obj_add_flag(menu_panel, LV_OBJ_FLAG_HIDDEN);
        }
        menu_visible = false;
        Lvgl_unlock();
    }
}

static void show_menu(void)
{
    if (Lvgl_lock(-1)) {
        if (menu_panel) {
            lv_obj_clear_flag(menu_panel, LV_OBJ_FLAG_HIDDEN);
        }
        menu_visible = true;
        update_menu_ui();
        Lvgl_unlock();
    }
}

static void create_menu_ui(void)
{
    // Create main label (default page)
    hello_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_label, "Hello World!");
    lv_obj_set_style_text_font(hello_label, &lv_font_montserrat_14, 0);
    lv_obj_align(hello_label, LV_ALIGN_CENTER, 0, 0);
    
    // Create menu panel (right-top corner)
    menu_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(menu_panel, 120, 60);
    lv_obj_align(menu_panel, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(menu_panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(menu_panel, 2, 0);
    lv_obj_set_style_border_color(menu_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(menu_panel, 5, 0);
    lv_obj_add_flag(menu_panel, LV_OBJ_FLAG_HIDDEN);
    
    // Create menu items
    for (int i = 0; i < menu_count; i++) {
        menu_labels[i] = lv_label_create(menu_panel);
        lv_label_set_text(menu_labels[i], menu_items[i]);
        lv_obj_set_style_text_font(menu_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_width(menu_labels[i], 100);
        lv_obj_set_style_pad_all(menu_labels[i], 3, 0);
        
        if (i == 0) {
            lv_obj_align(menu_labels[i], LV_ALIGN_TOP_MID, 0, 0);
        } else {
            lv_obj_align(menu_labels[i], LV_ALIGN_TOP_MID, 0, i * 22);
        }
        
        lv_obj_set_style_bg_opa(menu_labels[i], LV_OPA_COVER, 0);
    }
    
    update_menu_ui();
}

static void button_task(void *arg)
{
    while(1) {
        EventBits_t bits = xEventGroupWaitBits(BootButtonGroups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
        
        if (bits & set_bit_button(0)) {  // Single click - toggle menu item
            if (menu_visible) {
                if (Lvgl_lock(-1)) {
                    menu_selected = (menu_selected + 1) % menu_count;
                    update_menu_ui();
                    Lvgl_unlock();
                }
            }
        }
        
        if (bits & set_bit_button(2)) {  // Long press - confirm or show menu
            if (!menu_visible) {
                show_menu();
            } else {
                execute_menu_item();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

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

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing display...");
    RlcdPort.RLCD_Init();
    
    ESP_LOGI(TAG, "Initializing LVGL...");
    Lvgl_PortInit(LCD_WIDTH, LCD_HEIGHT, Lvgl_FlushCallback);
    
    ESP_LOGI(TAG, "Initializing button...");
    Custom_ButtonInit();
    
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_bsp_init();
    
    ESP_LOGI(TAG, "Initializing Web Server...");
    web_server_init();
    
    if(Lvgl_lock(-1)) {
        ESP_LOGI(TAG, "Creating Menu UI...");
        create_menu_ui();
        Lvgl_unlock();
    }
    
    xTaskCreatePinnedToCore(button_task, "button_task", 4 * 1024, NULL, 5, NULL, 1);
    
    ESP_LOGI(TAG, "Menu system ready!");
    ESP_LOGI(TAG, "Web admin: http://[IP]");
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
