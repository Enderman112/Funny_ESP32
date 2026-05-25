#include "wifi_bsp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WiFi_BSP";

static bool is_connected = false;
static char current_ip[16] = {0};
static char current_ssid[33] = {0};

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