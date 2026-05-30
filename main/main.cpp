#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#include <esp_tls.h>
#include <esp_sntp.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_config.h"
#include "button_bsp.h"
#include "wifi_bsp.h"
#include "web_server.h"
#include "adc_bsp.h"

extern "C" const lv_font_t lv_font_MiSansLight_16;

static const char *TAG = "HelloWorld";
static nvs_handle_t my_nvs_handle;

DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

// Clock state
static lv_obj_t *clock_label = NULL;
static bool clock_show_seconds = true;
static bool ntp_synced = false;
static char ntp_server[64] = "ntp.aliyun.com";
static char ntp_timezone[32] = "UTC-8";

// NVS存储函数
static void nvs_save_string(const char* key, const char* value)
{
    esp_err_t err = nvs_set_str(my_nvs_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set failed: %s key=%s", esp_err_to_name(err), key);
        return;
    }
    err = nvs_commit(my_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NVS saved: %s", key);
    }
}

static void nvs_load_string(const char* key, char* value, size_t max_len)
{
    size_t len = max_len;
    esp_err_t err = nvs_get_str(my_nvs_handle, key, value, &len);
    if (err != ESP_OK || strlen(value) == 0) {
        ESP_LOGW(TAG, "NVS load failed or empty: %s key=%s", esp_err_to_name(err), key);
        // 不覆盖默认值
    } else {
        ESP_LOGI(TAG, "NVS loaded: %s = %s", key, value);
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
    // 支持格式: "+8", "-5", "UTC+8", "UTC-5", "CST-8"
    if (tz[0] == '+' || tz[0] == '-') {
        // 偏移量格式: "+8" -> "UTC-8"
        char posix_tz[32];
        int offset = atoi(tz);
        if (offset >= 0) {
            snprintf(posix_tz, sizeof(posix_tz), "UTC-%d", offset);
        } else {
            snprintf(posix_tz, sizeof(posix_tz), "UTC+%d", -offset);
        }
        strncpy(ntp_timezone, posix_tz, sizeof(ntp_timezone) - 1);
    } else if (strncmp(tz, "UTC+", 4) == 0) {
        // "UTC+8" -> "UTC-8" (用户想表达UTC+8)
        char posix_tz[32];
        int offset = atoi(tz + 4);
        snprintf(posix_tz, sizeof(posix_tz), "UTC-%d", offset);
        strncpy(ntp_timezone, posix_tz, sizeof(ntp_timezone) - 1);
    } else if (strncmp(tz, "UTC-", 4) == 0) {
        // "UTC-8" -> "UTC+8" (用户想表达UTC-8)
        char posix_tz[32];
        int offset = atoi(tz + 4);
        snprintf(posix_tz, sizeof(posix_tz), "UTC+%d", offset);
        strncpy(ntp_timezone, posix_tz, sizeof(ntp_timezone) - 1);
    } else {
        // 其他格式直接使用 (如 "CST-8")
        strncpy(ntp_timezone, tz, sizeof(ntp_timezone) - 1);
    }
    ntp_timezone[sizeof(ntp_timezone) - 1] = '\0';
    nvs_save_string("ntp_tz", ntp_timezone);
    setenv("TZ", ntp_timezone, 1);
    tzset();
}

const char* ntp_get_timezone(void)
{
    return ntp_timezone;
}

extern "C" void ntp_sync_now(void)
{
    ntp_synced = false;
    esp_sntp_stop();
    obtain_time();
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
static int info_selected = 0;  // 0: WiFi状态, 1: AP开关, 2: 显示秒, 3: 同步时间
static const int info_count = 4;

// DeepSeek page state
static bool deepseek_page_active = false;
static char deepseek_balance[32] = "未知";
static char deepseek_usage[32] = "未知";
static char deepseek_error[64] = "";
static char deepseek_api_key[65] = "";

// MiMo state
static char mimo_cookie[640] = "";
static char mimo_month_used[32] = "未知";
static char mimo_month_limit[32] = "未知";
static char mimo_total_used[32] = "未知";
static char mimo_total_limit[32] = "未知";
static int mimo_month_percent = 0;
static float mimo_month_percent_f = 0.0f;
static float mimo_total_percent_f = 0.0f;
static char mimo_error[64] = "";

// UI labels for API page
static lv_obj_t *deepseek_label = NULL;
static lv_obj_t *mimo_label = NULL;
static lv_obj_t *mimo_bar1 = NULL;
static lv_obj_t *mimo_bar1_label = NULL;
static lv_obj_t *mimo_bar2 = NULL;
static lv_obj_t *mimo_bar2_label = NULL;
static lv_obj_t *ota_btn_label = NULL;

// Status bar labels
static lv_obj_t *wifi_icon = NULL;
static lv_obj_t *battery_icon = NULL;
static lv_obj_t *battery_label = NULL;
static lv_obj_t *charge_icon = NULL;

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
    
    // 先测试DNS解析
    struct addrinfo hints = {};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    ESP_LOGI(TAG, "Resolving api.deepseek.com...");
    int dns_err = getaddrinfo("api.deepseek.com", "443", &hints, &result);
    if (dns_err != 0 || result == NULL) {
        snprintf(deepseek_error, sizeof(deepseek_error), "DNS解析失败: %d", dns_err);
        ESP_LOGE(TAG, "DNS resolution failed: %d", dns_err);
        return;
    }
    
    char ip_str[16];
    struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "Resolved IP: %s", ip_str);
    freeaddrinfo(result);
    
    // 查询余额
    esp_http_client_config_t config = {};
    config.url = "https://api.deepseek.com/user/balance";
    config.event_handler = deepseek_balance_handler;
    config.timeout_ms = 15000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.keep_alive_enable = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    ESP_LOGI(TAG, "Fetching DeepSeek balance...");
    ESP_LOGI(TAG, "URL: %s", config.url);
    ESP_LOGI(TAG, "Key: %s...", deepseek_api_key);
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(deepseek_error, sizeof(deepseek_error), "初始化HTTP客户端失败");
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return;
    }
    
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32/1.0");
    
    ESP_LOGI(TAG, "Performing HTTP request...");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err != ESP_OK) {
        snprintf(deepseek_error, sizeof(deepseek_error), "请求失败: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "DeepSeek request failed: %s (0x%x)", esp_err_to_name(err), err);
        esp_http_client_cleanup(client);
        return;
    }
    
    int status = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "DeepSeek response: status=%d, length=%d", status, content_length);
    esp_http_client_cleanup(client);
    
    if (status != 200) {
        snprintf(deepseek_error, sizeof(deepseek_error), "HTTP错误: %d", status);
        return;
    }
    
    // 查询用量（该接口暂不可用）
    // config.url = "https://api.deepseek.com/user/usage";
    // config.event_handler = deepseek_usage_handler;
    
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
                char *data = (char*)evt->data;
                
                // Parse monthUsage percent
                char *month_str = strstr(data, "\"monthUsage\":");
                if (month_str) {
                    char *pct = strstr(month_str, "\"percent\":");
                    if (pct) {
                        pct += 10;
                        float percent = atof(pct);
                        mimo_month_percent_f = percent * 100.0f;
                        mimo_month_percent = (int)mimo_month_percent_f;
                    }
                }
                
                // Parse usage (total) percent
                char *usage_str = strstr(data, "\"usage\":");
                if (usage_str) {
                    char *pct = strstr(usage_str, "\"percent\":");
                    if (pct) {
                        pct += 10;
                        float percent = atof(pct);
                        mimo_total_percent_f = percent * 100.0f;
                    }
                }
                
                // Parse month_total_token used/limit
                char *month_str2 = strstr(data, "\"month_total_token\"");
                if (month_str2) {
                    char *u = strstr(month_str2, "\"used\":");
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
                    char *l = strstr(month_str2, "\"limit\":");
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
    
    // 等待一下，让DeepSeek请求释放资源
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 使用esp_tls直接发送HTTP请求
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        snprintf(mimo_error, sizeof(mimo_error), "初始化失败");
        return;
    }
    
    int ret = esp_tls_conn_http_new_sync("https://platform.xiaomimimo.com/api/v1/tokenPlan/usage", &cfg, tls);
    if (ret < 0 || tls == NULL) {
        snprintf(mimo_error, sizeof(mimo_error), "连接失败");
        ESP_LOGE(TAG, "MiMo TLS connection failed: %d", ret);
        return;
    }
    
    // 构建HTTP请求
    char request[1024];
    int req_len = snprintf(request, sizeof(request),
        "GET /api/v1/tokenPlan/usage HTTP/1.1\r\n"
        "Host: platform.xiaomimimo.com\r\n"
        "Cookie: %s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        mimo_cookie);
    
    int written = esp_tls_conn_write(tls, request, req_len);
    if (written < 0) {
        snprintf(mimo_error, sizeof(mimo_error), "发送失败");
        esp_tls_conn_destroy(tls);
        return;
    }
    
    // 读取响应
    char response[1024];
    int total_read = 0;
    while (total_read < sizeof(response) - 1) {
        int bytes_read = esp_tls_conn_read(tls, response + total_read, sizeof(response) - total_read - 1);
        if (bytes_read <= 0) break;
        total_read += bytes_read;
    }
    response[total_read] = '\0';
    esp_tls_conn_destroy(tls);
    
    // 解析HTTP状态码
    int status = 0;
    if (sscanf(response, "HTTP/%*s %d", &status) == 1) {
        ESP_LOGI(TAG, "MiMo response status: %d", status);
        if (status == 200) {
            char *body = strstr(response, "\r\n\r\n");
            if (body) {
                body += 4;
                // 解析本月百分比 (monthUsage)
                char *month_str = strstr(body, "\"monthUsage\":");
                if (month_str) {
                    char *percent_start = strstr(month_str, "\"percent\":");
                    if (percent_start) {
                        percent_start += 10;
                        float percent = atof(percent_start);
                        mimo_month_percent_f = percent * 100.0f;
                        mimo_month_percent = (int)mimo_month_percent_f;
                    }
                }
                // 解析总套餐百分比 (usage)
                char *usage_str = strstr(body, "\"usage\":");
                if (usage_str) {
                    // 跳过 monthUsage 里的 "usage": 字段
                    char *pct = strstr(usage_str, "\"percent\":");
                    if (pct) {
                        pct += 10;
                        float percent = atof(pct);
                        mimo_total_percent_f = percent * 100.0f;
                    }
                }
                char *used_start = strstr(body, "\"name\":\"month_total_token\"");
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
                strcpy(mimo_error, "");
            }
        } else if (status == 401) {
            snprintf(mimo_error, sizeof(mimo_error), "Cookie已过期");
        } else {
            snprintf(mimo_error, sizeof(mimo_error), "HTTP错误: %d", status);
        }
    } else {
        snprintf(mimo_error, sizeof(mimo_error), "解析失败");
    }
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
    
    char info_buf[512];
    const char* ap_status = wifi_bsp_is_ap_active() ? "开启" : "关闭";
    const char* cursor1 = (info_selected == 0) ? "> " : "  ";
    const char* cursor2 = (info_selected == 1) ? "> " : "  ";
    const char* cursor3 = (info_selected == 2) ? "> " : "  ";
    const char* cursor4 = (info_selected == 3) ? "> " : "  ";
    const char* ntp_status = ntp_synced ? "已同步" : "同步失败";
    const char* sec_status = clock_show_seconds ? "开" : "关";
    
    snprintf(info_buf, sizeof(info_buf),
             "信息\n\n"
             "当前版本: %s\n"
             "最新版本: %s\n\n"
             "NTP: %s\n"
             "%sWiFi: %s\n"
             "%sAP热点: %s\n"
             "%s显示秒: %s\n"
             "%s同步时间",
             FIRMWARE_VERSION,
             wifi_bsp_get_latest_version(),
             ntp_status,
             cursor1,
             wifi_bsp_is_connected() ? "已连接" : "未连接",
             cursor2,
             ap_status,
             cursor3,
             sec_status,
             cursor4);
    
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
        snprintf(ds_buf, sizeof(ds_buf), "DeepSeek\n\n余额: %s", deepseek_balance);
    }
    if (deepseek_label) lv_label_set_text(deepseek_label, ds_buf);
    
    // MiMo label (right side)
    char mimo_buf[128];
    // 把 MiMo 文字拆成两行：本月用量 / 总套餐用量，放在各自进度条下面
    if (strlen(mimo_error) > 0) {
        snprintf(mimo_buf, sizeof(mimo_buf), "MiMo\n错误: %s", mimo_error);
        if (mimo_bar1) lv_obj_add_flag(mimo_bar1, LV_OBJ_FLAG_HIDDEN);
        if (mimo_bar1_label) lv_obj_add_flag(mimo_bar1_label, LV_OBJ_FLAG_HIDDEN);
        if (mimo_bar2) lv_obj_add_flag(mimo_bar2, LV_OBJ_FLAG_HIDDEN);
        if (mimo_bar2_label) lv_obj_add_flag(mimo_bar2_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(mimo_buf, sizeof(mimo_buf), "MiMo");
        
        // Bar 1: monthly
        if (mimo_bar1) {
            lv_bar_set_value(mimo_bar1, (int)mimo_month_percent_f, LV_ANIM_OFF);
            lv_obj_clear_flag(mimo_bar1, LV_OBJ_FLAG_HIDDEN);
        }
        if (mimo_bar1_label) {
            char label[48];
            float month_used_b = atof(mimo_month_used) / 1000000000.0f;
            float month_limit_b = atof(mimo_month_limit) / 1000000000.0f;
            snprintf(label, sizeof(label), "本月: %.1fB / %.1fB (%.1f%%)", month_used_b, month_limit_b, mimo_month_percent_f);
            lv_label_set_text(mimo_bar1_label, label);
            lv_obj_clear_flag(mimo_bar1_label, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Bar 2: total
        if (mimo_bar2) {
            lv_bar_set_value(mimo_bar2, (int)mimo_total_percent_f, LV_ANIM_OFF);
            lv_obj_clear_flag(mimo_bar2, LV_OBJ_FLAG_HIDDEN);
        }
        if (mimo_bar2_label) {
            char label[48];
            float total_used_b = (strlen(mimo_total_used) > 0 && atof(mimo_total_used) > 0) ? atof(mimo_total_used) / 1000000000.0f : atof(mimo_month_used) / 1000000000.0f;
            float total_limit_b = (strlen(mimo_total_limit) > 0 && atof(mimo_total_limit) > 0) ? atof(mimo_total_limit) / 1000000000.0f : atof(mimo_month_limit) / 1000000000.0f;
            snprintf(label, sizeof(label), "总套餐: %.1fB / %.1fB (%.1f%%)", total_used_b, total_limit_b, mimo_total_percent_f);
            lv_label_set_text(mimo_bar2_label, label);
            lv_obj_clear_flag(mimo_bar2_label, LV_OBJ_FLAG_HIDDEN);
        }
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
                lv_obj_add_flag(mimo_bar1, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar1_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar2, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar2_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ota_btn_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_font(hello_label, &lv_font_montserrat_28, 0);
                lv_label_set_text(hello_label, "Hello World!");
                break;
            case 1: // Info
                info_page_active = true;
                deepseek_page_active = false;
                lv_obj_clear_flag(hello_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar1, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar1_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar2, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar2_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ota_btn_label, LV_OBJ_FLAG_HIDDEN);
                info_selected = 0;
                update_info_page();
                break;
            case 2: // API用量
                info_page_active = false;
                deepseek_page_active = true;
                lv_obj_add_flag(hello_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ota_btn_label, LV_OBJ_FLAG_HIDDEN);
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
    
    // Create DeepSeek label (left side, hidden by default)
    deepseek_label = lv_label_create(lv_scr_act());
    lv_label_set_text(deepseek_label, "");
    lv_obj_set_style_text_font(deepseek_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(deepseek_label, LV_ALIGN_TOP_LEFT, 15, 70);
    lv_obj_set_width(deepseek_label, 180);
    lv_obj_add_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create MiMo label (title only)
    mimo_label = lv_label_create(lv_scr_act());
    lv_label_set_text(mimo_label, "");
    lv_obj_set_style_text_font(mimo_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(mimo_label, LV_ALIGN_TOP_LEFT, 190, 40);
    lv_obj_set_width(mimo_label, 200);
    lv_obj_add_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
    
    // Bar 1 label (monthly usage + percent)  above bar
    mimo_bar1_label = lv_label_create(lv_scr_act());
    lv_label_set_text(mimo_bar1_label, "本月: 0.0B / 0.0B (0.0%)");
    lv_obj_set_style_text_font(mimo_bar1_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(mimo_bar1_label, LV_ALIGN_TOP_LEFT, 192, 65);
    lv_obj_add_flag(mimo_bar1_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create MiMo progress bar 1 (monthly)
    mimo_bar1 = lv_bar_create(lv_scr_act());
    lv_obj_set_size(mimo_bar1, 140, 14);
    lv_obj_align(mimo_bar1, LV_ALIGN_TOP_LEFT, 205, 90);
    lv_bar_set_range(mimo_bar1, 0, 100);
    lv_bar_set_value(mimo_bar1, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(mimo_bar1, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_bg_color(mimo_bar1, lv_color_hex(0x4CAF50), LV_PART_INDICATOR);
    lv_obj_set_style_radius(mimo_bar1, 0, 0);
    lv_obj_set_style_radius(mimo_bar1, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(mimo_bar1, 1, 0);
    lv_obj_set_style_border_color(mimo_bar1, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(mimo_bar1, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(mimo_bar1, lv_color_hex(0x666666), LV_PART_INDICATOR);
    lv_obj_add_flag(mimo_bar1, LV_OBJ_FLAG_HIDDEN);
    
    // Bar 1 label (monthly usage + percent)
    mimo_bar1_label = lv_label_create(lv_scr_act());
    lv_label_set_text(mimo_bar1_label, "本月: 0.0B / 0.0B (0.0%)");
    lv_obj_set_style_text_font(mimo_bar1_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(mimo_bar1_label, LV_ALIGN_TOP_LEFT, 190, 65);
    lv_obj_add_flag(mimo_bar1_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create MiMo progress bar 2 (total)
    mimo_bar2 = lv_bar_create(lv_scr_act());
    lv_obj_set_size(mimo_bar2, 140, 14);
    lv_obj_align(mimo_bar2, LV_ALIGN_TOP_LEFT, 205, 140);
    lv_bar_set_range(mimo_bar2, 0, 100);
    lv_bar_set_value(mimo_bar2, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(mimo_bar2, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_bg_color(mimo_bar2, lv_color_hex(0x2196F3), LV_PART_INDICATOR);
    lv_obj_set_style_radius(mimo_bar2, 0, 0);
    lv_obj_set_style_radius(mimo_bar2, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(mimo_bar2, 1, 0);
    lv_obj_set_style_border_color(mimo_bar2, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(mimo_bar2, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(mimo_bar2, lv_color_hex(0x666666), LV_PART_INDICATOR);
    lv_obj_add_flag(mimo_bar2, LV_OBJ_FLAG_HIDDEN);
    
    // Create OTA button label (bottom center, hidden by default)
    ota_btn_label = lv_label_create(lv_scr_act());
    lv_label_set_text(ota_btn_label, "[BOOT] 刷新数据");
    lv_obj_set_style_text_font(ota_btn_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(ota_btn_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(ota_btn_label, LV_OBJ_FLAG_HIDDEN);
    
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
    
    // Create status bar (bottom right)
    // Battery percentage
    battery_label = lv_label_create(lv_scr_act());
    lv_label_set_text(battery_label, "100%");
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_label, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    
    // Battery icon
    battery_icon = lv_label_create(lv_scr_act());
    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(battery_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_icon, LV_ALIGN_BOTTOM_RIGHT, -45, -5);
    
    // Charge icon (lightning)
    charge_icon = lv_label_create(lv_scr_act());
    lv_label_set_text(charge_icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(charge_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(charge_icon, LV_ALIGN_BOTTOM_RIGHT, -60, -5);
    lv_obj_add_flag(charge_icon, LV_OBJ_FLAG_HIDDEN);
    
    // WiFi icon
    wifi_icon = lv_label_create(lv_scr_act());
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_BOTTOM_RIGHT, -80, -5);
}

static void api_refresh_task(void *arg)
{
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (deepseek_page_active && wifi_bsp_is_connected()) {
            if (Lvgl_lock(-1)) {
                fetch_deepseek_info();
                fetch_mimo_usage();
                update_deepseek_page();
                Lvgl_unlock();
            }
        }
    }
}

static void status_bar_task(void *arg)
{
    while(1) {
        if (Lvgl_lock(-1)) {
            // Update WiFi icon
            if (wifi_icon) {
                if (wifi_bsp_is_connected()) {
                    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
                    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x000000), 0);
                } else {
                    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
                    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xCCCCCC), 0);
                }
            }
            
            // Update battery
            if (battery_icon && battery_label) {
                uint8_t level = adc_bsp_get_battery_level();
                float voltage = adc_bsp_get_battery_voltage();
                char level_buf[8];
                snprintf(level_buf, sizeof(level_buf), "%d%%", level);
                lv_label_set_text(battery_label, level_buf);
                
                // Show charge icon when voltage >= 4.15V (charging)
                if (charge_icon) {
                    if (voltage >= 4.15f) {
                        lv_obj_clear_flag(charge_icon, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_add_flag(charge_icon, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                
                if (level > 75) {
                    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_FULL);
                } else if (level > 50) {
                    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_3);
                } else if (level > 25) {
                    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_2);
                } else if (level > 10) {
                    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_1);
                } else {
                    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_EMPTY);
                }
            }
            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
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
                        case 3:  // 同步时间
                            ntp_synced = false;
                            esp_sntp_stop();
                            obtain_time();
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
    
    ESP_LOGI(TAG, "Initializing ADC (Battery)...");
    adc_bsp_init();
    
    ESP_LOGI(TAG, "Initializing Web Server...");
    web_server_init();
    
    xTaskCreatePinnedToCore(button_task, "button_task", 8 * 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(clock_task, "clock_task", 2 * 1024, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(ntp_task, "ntp_task", 4 * 1024, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(api_refresh_task, "api_refresh", 8 * 1024, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(status_bar_task, "status_bar", 4 * 1024, NULL, 2, NULL, 1);
    
    ESP_LOGI(TAG, "Menu system ready!");
    ESP_LOGI(TAG, "Web admin: http://[IP]");
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
