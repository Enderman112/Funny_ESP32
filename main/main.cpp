#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <esp_sntp.h>
#include <nvs_flash.h>
#include <nvs.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_config.h"
#include "button_bsp.h"
#include "wifi_bsp.h"
#include "web_server.h"

extern "C" const lv_font_t lv_font_MiSansLight_16;

static const char *TAG = "HelloWorld";
static nvs_handle_t my_nvs_handle;

DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

// Clock state
static lv_obj_t *clock_label = NULL;
static bool clock_show_seconds = true;
static bool ntp_synced = false;
static char ntp_server[64] = "ntp.aliyun.com";
static char ntp_timezone[32] = "CST-8";

// NVS存储函数
static void nvs_save_string(const char* key, const char* value)
{
    nvs_set_str(my_nvs_handle, key, value);
    nvs_commit(my_nvs_handle);
}

static void nvs_load_string(const char* key, char* value, size_t max_len)
{
    size_t len = max_len;
    esp_err_t err = nvs_get_str(my_nvs_handle, key, value, &len);
    if (err != ESP_OK) {
        value[0] = '\0';
    }
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP, server: %s, tz: %s", ntp_server, ntp_timezone);
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntp_server);
    esp_sntp_init();
    setenv("TZ", ntp_timezone, 1);
    tzset();
}

static void obtain_time(void)
{
    initialize_sntp();
    
    int retry = 0;
    const int retry_count = 10;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    if (retry < retry_count) {
        ESP_LOGI(TAG, "Time synchronized");
        ntp_synced = true;
    } else {
        ESP_LOGW(TAG, "Failed to sync time");
        ntp_synced = false;
    }
}

void ntp_set_server(const char* server)
{
    strncpy(ntp_server, server, sizeof(ntp_server) - 1);
    ntp_server[sizeof(ntp_server) - 1] = '\0';
    nvs_save_string("ntp_server", ntp_server);
    esp_sntp_stop();
    obtain_time();
}

const char* ntp_get_server(void)
{
    return ntp_server;
}

void ntp_set_timezone(const char* tz)
{
    strncpy(ntp_timezone, tz, sizeof(ntp_timezone) - 1);
    ntp_timezone[sizeof(ntp_timezone) - 1] = '\0';
    nvs_save_string("ntp_tz", ntp_timezone);
    setenv("TZ", ntp_timezone, 1);
    tzset();
}

const char* ntp_get_timezone(void)
{
    return ntp_timezone;
}

void clock_set_show_seconds(bool show)
{
    clock_show_seconds = show;
}

bool clock_get_show_seconds(void)
{
    return clock_show_seconds;
}

static void update_clock(void)
{
    if (!clock_label) return;
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    if (timeinfo.tm_year < (2016 - 1900)) {
        lv_label_set_text(clock_label, "--:--");
    } else {
        char time_buf[16];
        if (clock_show_seconds) {
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", 
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        } else {
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d", 
                     timeinfo.tm_hour, timeinfo.tm_min);
        }
        lv_label_set_text(clock_label, time_buf);
    }
}

static void clock_task(void *arg)
{
    while(1) {
        if (Lvgl_lock(-1)) {
            update_clock();
            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// NVS存储函数
static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_open("storage", NVS_READWRITE, &my_nvs_handle);
}

// Menu state
static bool menu_visible = false;
static int menu_selected = 0;
static const int menu_count = 3;
static const char* menu_items[] = {"Hello World", "信息", "API用量"};
static lv_obj_t *menu_panel = NULL;
static lv_obj_t *menu_labels[3] = {NULL};
static lv_obj_t *hello_label = NULL;

// Info page state
static bool info_page_active = false;
static int info_selected = 0;  // 0: WiFi状态, 1: AP开关, 2: 显示秒
static const int info_count = 3;

// DeepSeek page state
static bool deepseek_page_active = false;
static char deepseek_balance[32] = "未知";
static char deepseek_usage[32] = "未知";
static char deepseek_error[64] = "";
static char deepseek_api_key[65] = "";

// MiMo state
static char mimo_cookie[256] = "";
static char mimo_month_used[32] = "未知";
static char mimo_month_limit[32] = "未知";
static int mimo_month_percent = 0;
static char mimo_error[64] = "";

// UI labels for API page
static lv_obj_t *deepseek_label = NULL;
static lv_obj_t *mimo_label = NULL;

static esp_err_t deepseek_balance_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && evt->data_len < 256) {
                char *balance_start = strstr((char*)evt->data, "\"total_balance\":\"");
                if (balance_start) {
                    balance_start += 17;
                    char *balance_end = strchr(balance_start, '"');
                    if (balance_end) {
                        int len = balance_end - balance_start;
                        if (len > 31) len = 31;
                        strncpy(deepseek_balance, balance_start, len);
                        deepseek_balance[len] = '\0';
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t deepseek_usage_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && evt->data_len < 256) {
                char *usage_start = strstr((char*)evt->data, "\"total_tokens\":");
                if (usage_start) {
                    usage_start += 15;
                    char *usage_end = strchr(usage_start, '}');
                    if (!usage_end) usage_end = strchr(usage_start, ',');
                    if (usage_end) {
                        int len = usage_end - usage_start;
                        if (len > 31) len = 31;
                        strncpy(deepseek_usage, usage_start, len);
                        deepseek_usage[len] = '\0';
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void fetch_deepseek_info(void)
{
    if (strlen(deepseek_api_key) == 0) {
        strcpy(deepseek_error, "请先配置API密钥");
        return;
    }
    
    char auth_header[80];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", deepseek_api_key);
    
    // 查询余额
    esp_http_client_config_t config = {};
    config.url = "https://api.deepseek.com/user/balance";
    config.event_handler = deepseek_balance_handler;
    config.timeout_ms = 5000;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err != ESP_OK) {
        snprintf(deepseek_error, sizeof(deepseek_error), "余额查询失败");
        esp_http_client_cleanup(client);
        return;
    }
    
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    if (status != 200) {
        snprintf(deepseek_error, sizeof(deepseek_error), "HTTP错误: %d", status);
        return;
    }
    
    // 查询用量
    config.url = "https://api.deepseek.com/user/usage";
    config.event_handler = deepseek_usage_handler;
    
    client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Accept", "application/json");
    err = esp_http_client_perform(client);
    
    if (err != ESP_OK) {
        snprintf(deepseek_error, sizeof(deepseek_error), "用量查询失败");
    } else {
        status = esp_http_client_get_status_code(client);
        if (status != 200) {
            snprintf(deepseek_error, sizeof(deepseek_error), "HTTP错误: %d", status);
        } else {
            strcpy(deepseek_error, "");
        }
    }
    
    esp_http_client_cleanup(client);
}

void deepseek_set_api_key(const char* key)
{
    strncpy(deepseek_api_key, key, sizeof(deepseek_api_key) - 1);
    deepseek_api_key[sizeof(deepseek_api_key) - 1] = '\0';
    nvs_save_string("ds_api_key", deepseek_api_key);
    ESP_LOGI(TAG, "DeepSeek API key updated");
}

const char* deepseek_get_api_key(void)
{
    return deepseek_api_key;
}

// MiMo API
static esp_err_t mimo_usage_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && evt->data_len < 1024) {
                // Parse monthUsage percent
                char *percent_start = strstr((char*)evt->data, "\"monthUsage\":{\"percent\":");
                if (percent_start) {
                    percent_start += 25;
                    char *percent_end = strchr(percent_start, ',');
                    if (percent_end) {
                        char buf[8] = {0};
                        int len = percent_end - percent_start;
                        if (len > 7) len = 7;
                        strncpy(buf, percent_start, len);
                        mimo_month_percent = atoi(buf);
                    }
                }
                
                // Parse month_total_token used
                char *used_start = strstr((char*)evt->data, "\"name\":\"month_total_token\"");
                if (used_start) {
                    char *u = strstr(used_start, "\"used\":");
                    if (u) {
                        u += 7;
                        char *u_end = strchr(u, ',');
                        if (u_end) {
                            int len = u_end - u;
                            if (len > 31) len = 31;
                            strncpy(mimo_month_used, u, len);
                            mimo_month_used[len] = '\0';
                        }
                    }
                    char *l = strstr(used_start, "\"limit\":");
                    if (l) {
                        l += 8;
                        char *l_end = strchr(l, ',');
                        if (l_end) {
                            int len = l_end - l;
                            if (len > 31) len = 31;
                            strncpy(mimo_month_limit, l, len);
                            mimo_month_limit[len] = '\0';
                        }
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void fetch_mimo_usage(void)
{
    if (strlen(mimo_cookie) == 0) {
        strcpy(mimo_error, "请先配置Cookie");
        return;
    }
    
    esp_http_client_config_t config = {};
    config.url = "https://platform.xiaomimimo.com/api/v1/tokenPlan/usage";
    config.event_handler = mimo_usage_handler;
    config.timeout_ms = 5000;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Cookie", mimo_cookie);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err != ESP_OK) {
        snprintf(mimo_error, sizeof(mimo_error), "请求失败");
    } else {
        int status = esp_http_client_get_status_code(client);
        if (status == 401) {
            snprintf(mimo_error, sizeof(mimo_error), "Cookie已过期");
        } else if (status != 200) {
            snprintf(mimo_error, sizeof(mimo_error), "HTTP错误: %d", status);
        } else {
            strcpy(mimo_error, "");
        }
    }
    
    esp_http_client_cleanup(client);
}

void mimo_set_cookie(const char* cookie)
{
    strncpy(mimo_cookie, cookie, sizeof(mimo_cookie) - 1);
    mimo_cookie[sizeof(mimo_cookie) - 1] = '\0';
    nvs_save_string("mimo_cookie", mimo_cookie);
    ESP_LOGI(TAG, "MiMo cookie updated");
}

const char* mimo_get_cookie(void)
{
    return mimo_cookie;
}

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

static void update_info_page(void)
{
    if (!info_page_active) return;
    
    char info_buf[256];
    const char* ap_status = wifi_bsp_is_ap_active() ? "开启" : "关闭";
    const char* cursor1 = (info_selected == 0) ? "> " : "  ";
    const char* cursor2 = (info_selected == 1) ? "> " : "  ";
    const char* cursor3 = (info_selected == 2) ? "> " : "  ";
    const char* ntp_status = ntp_synced ? "已同步" : "同步失败";
    const char* sec_status = clock_show_seconds ? "开" : "关";
    
    snprintf(info_buf, sizeof(info_buf),
             "信息\n\n"
             "NTP: %s\n"
             "%sWiFi: %s\n"
             "%sAP热点: %s\n"
             "%s显示秒: %s",
             ntp_status,
             cursor1,
             wifi_bsp_is_connected() ? "已连接" : "未连接",
             cursor2,
             ap_status,
             cursor3,
             sec_status);
    
    lv_obj_set_style_text_font(hello_label, &lv_font_MiSansLight_16, 0);
    lv_label_set_text(hello_label, info_buf);
}

static void update_deepseek_page(void)
{
    if (!deepseek_page_active) return;
    
    // DeepSeek label (left side)
    char ds_buf[128];
    if (strlen(deepseek_error) > 0) {
        snprintf(ds_buf, sizeof(ds_buf), "DeepSeek\n\n错误: %s", deepseek_error);
    } else {
        snprintf(ds_buf, sizeof(ds_buf), "DeepSeek\n\n余额: %s\nToken: %s", deepseek_balance, deepseek_usage);
    }
    if (deepseek_label) lv_label_set_text(deepseek_label, ds_buf);
    
    // MiMo label (right side)
    char mimo_buf[128];
    if (strlen(mimo_error) > 0) {
        snprintf(mimo_buf, sizeof(mimo_buf), "MiMo\n\n错误: %s", mimo_error);
    } else {
        // Progress bar text
        char bar[16] = "";
        int filled = mimo_month_percent / 10;
        for (int i = 0; i < 10; i++) {
            bar[i] = (i < filled) ? '#' : '-';
        }
        bar[10] = '\0';
        snprintf(mimo_buf, sizeof(mimo_buf), "MiMo\n\n本月: %d%%\n[%s]\n%s / %s", 
                 mimo_month_percent,
                 bar, mimo_month_used, mimo_month_limit);
    }
    if (mimo_label) lv_label_set_text(mimo_label, mimo_buf);
}

static void execute_menu_item(void)
{
    if (Lvgl_lock(-1)) {
        switch (menu_selected) {
            case 0: // Hello World
                info_page_active = false;
                deepseek_page_active = false;
                lv_obj_clear_flag(hello_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_font(hello_label, &lv_font_montserrat_28, 0);
                lv_label_set_text(hello_label, "Hello World!");
                break;
            case 1: // Info
                info_page_active = true;
                deepseek_page_active = false;
                lv_obj_clear_flag(hello_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
                info_selected = 0;
                update_info_page();
                break;
            case 2: // API用量
                info_page_active = false;
                deepseek_page_active = true;
                lv_obj_add_flag(hello_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
                fetch_deepseek_info();
                fetch_mimo_usage();
                update_deepseek_page();
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
    // Create clock label (left-top corner)
    clock_label = lv_label_create(lv_scr_act());
    lv_label_set_text(clock_label, "--:--:--");
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_28, 0);
    lv_obj_align(clock_label, LV_ALIGN_TOP_LEFT, 10, 10);
    
    // Create main label (default page - English only)
    hello_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_label, "Hello World!");
    lv_obj_set_style_text_font(hello_label, &lv_font_montserrat_28, 0);
    lv_obj_align(hello_label, LV_ALIGN_CENTER, 0, 0);
    
    // Create DeepSeek label (left side, hidden by default - Chinese)
    deepseek_label = lv_label_create(lv_scr_act());
    lv_label_set_text(deepseek_label, "");
    lv_obj_set_style_text_font(deepseek_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(deepseek_label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_add_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create MiMo label (right side, hidden by default - Chinese)
    mimo_label = lv_label_create(lv_scr_act());
    lv_label_set_text(mimo_label, "");
    lv_obj_set_style_text_font(mimo_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(mimo_label, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create menu panel (right-top corner)
    menu_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(menu_panel, 120, 80);
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
        lv_obj_set_style_text_font(menu_labels[i], &lv_font_MiSansLight_16, 0);
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
        EventBits_t boot_bits = xEventGroupWaitBits(BootButtonGroups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(50));
        EventBits_t key_bits = xEventGroupWaitBits(GP18ButtonGroups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(50));
        
        // KEY键（GPIO 18）
        if (key_bits & set_bit_button(0)) {  // KEY短按
            if (menu_visible) {
                // 菜单页面：切换菜单项
                if (Lvgl_lock(-1)) {
                    menu_selected = (menu_selected + 1) % menu_count;
                    update_menu_ui();
                    Lvgl_unlock();
                }
            }
        }
        
        if (key_bits & set_bit_button(2)) {  // KEY长按
            if (menu_visible) {
                // 菜单已打开：确认选择，隐藏菜单，显示对应页面
                execute_menu_item();
            } else {
                // 任何页面：保持原界面，打开菜单覆盖
                show_menu();
            }
        }
        
        // BOOT键（GPIO 0）
        if (boot_bits & set_bit_button(0)) {  // BOOT短按
            if (info_page_active) {
                if (Lvgl_lock(-1)) {
                    info_selected = (info_selected + 1) % info_count;
                    update_info_page();
                    Lvgl_unlock();
                }
            }
        }
        
        if (boot_bits & set_bit_button(2)) {  // BOOT长按
            if (info_page_active) {
                if (Lvgl_lock(-1)) {
                    switch (info_selected) {
                        case 0: break;
                        case 1:
                            if (wifi_bsp_is_ap_active()) wifi_bsp_stop_ap();
                            else wifi_bsp_start_ap();
                            update_info_page();
                            break;
                        case 2:
                            clock_show_seconds = !clock_show_seconds;
                            update_info_page();
                            break;
                    }
                    Lvgl_unlock();
                }
            } else if (deepseek_page_active) {
                if (Lvgl_lock(-1)) {
                    fetch_deepseek_info();
                    fetch_mimo_usage();
                    update_deepseek_page();
                    Lvgl_unlock();
                }
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

static void ntp_task(void *arg)
{
    obtain_time();
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing NVS...");
    nvs_init();
    
    // 从NVS加载保存的数据
    nvs_load_string("ds_api_key", deepseek_api_key, sizeof(deepseek_api_key));
    nvs_load_string("mimo_cookie", mimo_cookie, sizeof(mimo_cookie));
    nvs_load_string("ntp_server", ntp_server, sizeof(ntp_server));
    nvs_load_string("ntp_tz", ntp_timezone, sizeof(ntp_timezone));
    
    ESP_LOGI(TAG, "Initializing display...");
    RlcdPort.RLCD_Init();
    
    ESP_LOGI(TAG, "Initializing LVGL...");
    Lvgl_PortInit(LCD_WIDTH, LCD_HEIGHT, Lvgl_FlushCallback);
    
    ESP_LOGI(TAG, "Initializing button...");
    Custom_ButtonInit();
    
    if(Lvgl_lock(-1)) {
        ESP_LOGI(TAG, "Creating Menu UI...");
        create_menu_ui();
        Lvgl_unlock();
    }
    
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_bsp_init();
    
    ESP_LOGI(TAG, "Initializing Web Server...");
    web_server_init();
    
    xTaskCreatePinnedToCore(button_task, "button_task", 4 * 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(clock_task, "clock_task", 2 * 1024, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(ntp_task, "ntp_task", 4 * 1024, NULL, 2, NULL, 1);
    
    ESP_LOGI(TAG, "Menu system ready!");
    ESP_LOGI(TAG, "Web admin: http://[IP]");
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
