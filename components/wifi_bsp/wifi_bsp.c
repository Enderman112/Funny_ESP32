#include "wifi_bsp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WiFi_BSP";

static bool is_connected = false;
static char current_ip[16] = {0};
static char current_ssid[33] = {0};
static char latest_version[32] = "unknown";

static bool ap_active = false;
static char ap_ip[16] = "192.168.4.1";

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && evt->data_len < 512) {
                char *tag_start = strstr((char*)evt->data, "\"tag_name\":\"");
                if (tag_start) {
                    tag_start += 12;
                    char *tag_end = strchr(tag_start, '"');
                    if (tag_end) {
                        int len = tag_end - tag_start;
                        if (len > 31) len = 31;
                        strncpy(latest_version, tag_start, len);
                        latest_version[len] = '\0';
                        ESP_LOGI(TAG, "Latest version: %s", latest_version);
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void fetch_latest_version(void)
{
    esp_http_client_config_t config = {
        .url = "https://api.github.com/repos/Enderman112/Funny_ESP32/releases/latest",
        .event_handler = http_event_handler,
        .timeout_ms = 5000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "User-Agent", "ESP32");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        uint32_t pxip = event->ip_info.ip.addr;
        sprintf(current_ip, "%d.%d.%d.%d", 
                (uint8_t)(pxip), (uint8_t)(pxip >> 8), 
                (uint8_t)(pxip >> 16), (uint8_t)(pxip >> 24));
        ESP_LOGI(TAG, "Connected, IP: %s", current_ip);
        is_connected = true;
        
        // Fetch latest version from GitHub
        fetch_latest_version();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected");
        is_connected = false;
        memset(current_ip, 0, sizeof(current_ip));
        esp_wifi_connect();
    }
}

void wifi_bsp_init(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip);
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Xiaomi_417AAA",
            .password = "Zjhu417417",
        },
    };
    
    strncpy(current_ssid, (char*)wifi_config.sta.ssid, sizeof(current_ssid) - 1);
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    
    ESP_LOGI(TAG, "WiFi initialized, connecting to %s", current_ssid);
}

bool wifi_bsp_is_connected(void)
{
    return is_connected;
}

const char* wifi_bsp_get_ip(void)
{
    return current_ip;
}

const char* wifi_bsp_get_ssid(void)
{
    return current_ssid;
}

void wifi_bsp_connect(const char* ssid, const char* password)
{
    esp_wifi_disconnect();
    
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    
    strncpy(current_ssid, ssid, sizeof(current_ssid) - 1);
    
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
    
    ESP_LOGI(TAG, "Connecting to new WiFi: %s", ssid);
}

const char* wifi_bsp_get_latest_version(void)
{
    return latest_version;
}

void wifi_bsp_start_ap(void)
{
    esp_netif_create_default_wifi_ap();
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "Funny_ESP32",
            .ssid_len = 11,
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    
    ap_active = true;
    ESP_LOGI(TAG, "AP started, SSID: Funny_ESP32, Password: 12345678");
}

void wifi_bsp_stop_ap(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    ap_active = false;
    ESP_LOGI(TAG, "AP stopped");
}

bool wifi_bsp_is_ap_active(void)
{
    return ap_active;
}

const char* wifi_bsp_get_ap_ip(void)
{
    return ap_ip;
}

void wifi_bsp_update_ap_password(const char* password)
{
    wifi_config_t ap_config;
    esp_wifi_get_config(WIFI_IF_AP, &ap_config);
    
    strncpy((char*)ap_config.ap.password, password, sizeof(ap_config.ap.password) - 1);
    
    if (strlen(password) < 8) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }
    
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    ESP_LOGI(TAG, "AP password updated");
}