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
#include <zlib.h>
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
extern "C" const lv_font_t lv_font_qweather_icons_24;

static const char *TAG = "HelloWorld";
static nvs_handle_t my_nvs_handle;

// 天气icon code -> Unicode映射表
static const struct { int code; uint32_t unicode; } weather_icon_map[] = {
    {100, 0xF101}, {101, 0xF102}, {102, 0xF103}, {103, 0xF104}, {104, 0xF105},
    {150, 0xF106}, {151, 0xF107}, {152, 0xF108}, {153, 0xF109},
    {300, 0xF10A}, {301, 0xF10B}, {302, 0xF10C}, {303, 0xF10D}, {304, 0xF10E},
    {305, 0xF10F}, {306, 0xF110}, {307, 0xF111}, {308, 0xF112}, {309, 0xF113},
    {310, 0xF114}, {311, 0xF115}, {312, 0xF116}, {313, 0xF117}, {314, 0xF118},
    {315, 0xF119}, {316, 0xF11A}, {317, 0xF11B}, {318, 0xF11C},
    {350, 0xF11D}, {351, 0xF11E}, {399, 0xF11F},
    {400, 0xF120}, {401, 0xF121}, {402, 0xF122}, {403, 0xF123}, {404, 0xF124},
    {405, 0xF125}, {406, 0xF126}, {407, 0xF127}, {408, 0xF128}, {409, 0xF129},
    {410, 0xF12A}, {456, 0xF12B}, {457, 0xF12C}, {499, 0xF12D},
    {500, 0xF12E}, {501, 0xF12F}, {502, 0xF130}, {503, 0xF131}, {504, 0xF132},
    {507, 0xF133}, {508, 0xF134}, {509, 0xF135}, {510, 0xF136}, {511, 0xF137},
    {512, 0xF138}, {513, 0xF139}, {514, 0xF13A}, {515, 0xF13B},
    {900, 0xF144}, {901, 0xF145}, {999, 0xF146},
};

// icon code字符串 -> UTF-8字符
static void icon_code_to_utf8(const char *code, char *buf, size_t buf_size)
{
    int code_int = atoi(code);
    for (int i = 0; i < sizeof(weather_icon_map)/sizeof(weather_icon_map[0]); i++) {
        if (weather_icon_map[i].code == code_int) {
            uint32_t u = weather_icon_map[i].unicode;
            if (u < 0x80) {
                buf[0] = (char)u; buf[1] = '\0';
            } else if (u < 0x800) {
                buf[0] = 0xC0 | (u >> 6); buf[1] = 0x80 | (u & 0x3F); buf[2] = '\0';
            } else if (u < 0x10000) {
                buf[0] = 0xE0 | (u >> 12); buf[1] = 0x80 | ((u >> 6) & 0x3F);
                buf[2] = 0x80 | (u & 0x3F); buf[3] = '\0';
            } else {
                buf[0] = 0xF0 | (u >> 18); buf[1] = 0x80 | ((u >> 12) & 0x3F);
                buf[2] = 0x80 | ((u >> 6) & 0x3F); buf[3] = 0x80 | (u & 0x3F); buf[4] = '\0';
            }
            return;
        }
    }
    buf[0] = '\0';
}

DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

// Clock state
static lv_obj_t *clock_label = NULL;
static lv_obj_t *hello_clock_label = NULL;  // 大时钟
static lv_obj_t *hello_date_label = NULL;   // 日期
static lv_obj_t *hello_week_label = NULL;   // 星期
static lv_obj_t *hello_weather_label = NULL; // 天气
static lv_obj_t *hello_weather_icon = NULL;  // 天气图标
static lv_obj_t *hello_city_label = NULL;    // 城市
static lv_obj_t *hello_saying_label = NULL; // 一言
static char saying_text[128] = "";
static int last_saying_day = 0;
static int last_weather_period = -1;
static bool clock_show_seconds = true;
static bool ntp_synced = false;
static char ntp_server[64] = "ntp.aliyun.com";
static char ntp_timezone[32] = "UTC-8";

// 天气数据
static char weather_text[64] = "天气获取中...";
static char weather_icon_code[8] = "999";
static char weather_temp[16] = "--";
static int weather_provider = 0;  // 0=none, 1=qweather, 2=openweather
static char qweather_api_key[65] = "";
static char qweather_apihost[64] = "";
static char openweather_api_key[65] = "";
static char weather_location[32] = "101010100";

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

// clock_task 移到 update_hello_page 之后定义

// 获取每日一言
// 一言HTTP回调
static char saying_buf[512] = {0};
static int saying_buf_len = 0;

static esp_err_t saying_http_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && saying_buf_len + evt->data_len < 512) {
                memcpy(saying_buf + saying_buf_len, evt->data, evt->data_len);
                saying_buf_len += evt->data_len;
                saying_buf[saying_buf_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static int gzip_decompress(const unsigned char *src, int len, char *dst, int dst_size);

static void fetch_saying(void)
{
    saying_buf[0] = '\0';
    saying_buf_len = 0;
    
    esp_http_client_config_t config = {};
    config.url = "https://uapis.cn/api/v1/saying";
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = saying_http_handler;
    config.user_agent = "Mozilla/5.0";
    config.disable_auto_redirect = true;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        // gzip解压
        char decompressed[512];
        if (saying_buf_len > 0) {
            gzip_decompress((unsigned char *)saying_buf, saying_buf_len, decompressed, sizeof(decompressed));
            memcpy(saying_buf, decompressed, sizeof(decompressed));
        }
        ESP_LOGI(TAG, "Saying API status: %d, response: %s", status, saying_buf);
        if (status == 200 && strlen(saying_buf) > 0) {
            char *text_start = strstr(saying_buf, "\"text\":\"");
                if (text_start) {
                    text_start += 8;
                    char *text_end = strchr(text_start, '"');
                    if (text_end) {
                        int tlen = text_end - text_start;
                        if (tlen > 127) tlen = 127;
                        strncpy(saying_text, text_start, tlen);
                        saying_text[tlen] = '\0';
                        ESP_LOGI(TAG, "Saying: %s", saying_text);
                    }
                }
        }
    }
    
    esp_http_client_cleanup(client);
}

// gzip解压: src/len -> dst/dst_size, 返回解压后长度, 失败返回-1
static int gzip_decompress(const unsigned char *src, int len, char *dst, int dst_size)
{
    if (len < 2 || src[0] != 0x1f || src[1] != 0x8b) {
        // 非gzip，直接拷贝
        int copy = len < dst_size ? len : dst_size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
        return copy;
    }
    z_stream stream = {};
    stream.next_in = (Bytef *)src;
    stream.avail_in = len;
    if (inflateInit2(&stream, 15 + 16) != Z_OK) return -1;
    stream.next_out = (Bytef *)dst;
    stream.avail_out = dst_size - 1;
    int ret = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (ret != Z_STREAM_END && ret != Z_OK) return -1;
    int out_len = stream.total_out;
    dst[out_len] = '\0';
    return out_len;
}

// 天气HTTP回调
static char weather_buf[1024] = {0};
static int weather_buf_len = 0;

static esp_err_t weather_http_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && weather_buf_len + evt->data_len < 1024) {
                memcpy(weather_buf + weather_buf_len, evt->data, evt->data_len);
                weather_buf_len += evt->data_len;
                weather_buf[weather_buf_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// GeoAPI HTTP回调
static char geo_response_buf[1024] = {0};
static int geo_response_len = 0;

static esp_err_t geo_http_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && geo_response_len + evt->data_len < 1024) {
                memcpy(geo_response_buf + geo_response_len, evt->data, evt->data_len);
                geo_response_len += evt->data_len;
                geo_response_buf[geo_response_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void url_encode(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0;
    while (*src && i < dst_size - 4) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[i++] = c;
        } else {
            snprintf(dst + i, dst_size - i, "%%%02X", c);
            i += 3;
        }
        src++;
    }
    dst[i] = '\0';
}

static void fetch_weather(void)
{
    ESP_LOGI(TAG, "fetch_weather: provider=%d, location=%s, apihost=%s", 
             weather_provider, weather_location, qweather_apihost);
    
    if (weather_provider == 0) {
        strcpy(weather_text, "");
        return;
    }
    
    weather_buf[0] = '\0';
    weather_buf_len = 0;
    char url[256];
    char location_id[32] = {0};
    
    if (weather_provider == 1 && strlen(qweather_api_key) > 0) {
        // 和风天气 - 先通过GeoAPI查城市ID
        char geo_url[512];
        char encoded_location[128];
        url_encode(weather_location, encoded_location, sizeof(encoded_location));
        snprintf(geo_url, sizeof(geo_url),
            "https://%s/geo/v2/city/lookup?location=%s&key=%s&number=1",
            qweather_apihost, encoded_location, qweather_api_key);
        
        esp_http_client_config_t geo_config = {};
        geo_config.url = geo_url;
        geo_config.timeout_ms = 5000;
        geo_config.crt_bundle_attach = esp_crt_bundle_attach;
        geo_config.event_handler = geo_http_handler;
        geo_config.user_agent = "Mozilla/5.0";
        geo_config.disable_auto_redirect = true;
        
        geo_response_buf[0] = '\0';
        geo_response_len = 0;
        
        esp_http_client_handle_t geo_client = esp_http_client_init(&geo_config);
        esp_http_client_set_header(geo_client, "Accept", "application/json");
        esp_err_t geo_err = esp_http_client_perform(geo_client);
        
        if (geo_err == ESP_OK) {
            int geo_status = esp_http_client_get_status_code(geo_client);
            // gzip解压
            char decompressed[1024];
            if (geo_response_len > 0) {
                gzip_decompress((unsigned char *)geo_response_buf, geo_response_len, decompressed, sizeof(decompressed));
                memcpy(geo_response_buf, decompressed, sizeof(decompressed));
            }
            ESP_LOGI(TAG, "GeoAPI status: %d, response: %s", geo_status, geo_response_buf);
            if (geo_status == 200 && strlen(geo_response_buf) > 0) {
                // 解析location ID: "id":"101010100"
                char *id_start = strstr(geo_response_buf, "\"id\":\"");
                if (id_start) {
                    id_start += 6;
                    char *id_end = strchr(id_start, '"');
                    if (id_end) {
                        int len = id_end - id_start;
                        if (len > 31) len = 31;
                        strncpy(location_id, id_start, len);
                        ESP_LOGI(TAG, "Location ID: %s", location_id);
                    }
                }
            }
        }
        esp_http_client_cleanup(geo_client);
        
        if (strlen(location_id) == 0) {
            ESP_LOGW(TAG, "GeoAPI: city not found, keeping old weather");
            return;
        }
        
        // 查天气
        snprintf(url, sizeof(url), 
            "https://%s/v7/weather/now?location=%s&key=%s",
            qweather_apihost, location_id, qweather_api_key);
    } else if (weather_provider == 2 && strlen(openweather_api_key) > 0) {
        snprintf(url, sizeof(url),
            "https://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=metric&lang=zh_cn",
            weather_location, openweather_api_key);
    } else {
        strcpy(weather_text, "未配置API");
        return;
    }
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = weather_http_handler;
    config.user_agent = "Mozilla/5.0";
    config.disable_auto_redirect = true;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Weather API status: %d", status);
        
        // gzip解压
        char decompressed[1024];
        if (weather_buf_len > 0) {
            gzip_decompress((unsigned char *)weather_buf, weather_buf_len, decompressed, sizeof(decompressed));
            memcpy(weather_buf, decompressed, sizeof(decompressed));
        }
        
        if (status == 200 && strlen(weather_buf) > 0) {
            if (weather_provider == 1) {
                // 解析和风天气: "temp":"25","text":"晴"
                char *temp_start = strstr(weather_buf, "\"temp\":\"");
                if (temp_start) {
                    temp_start += 8;
                    char *temp_end = strchr(temp_start, '"');
                    if (temp_end) {
                        int len = temp_end - temp_start;
                        if (len > 15) len = 15;
                        strncpy(weather_temp, temp_start, len);
                        weather_temp[len] = '\0';
                    }
                }
                char *text_start = strstr(weather_buf, "\"text\":\"");
                if (text_start) {
                    text_start += 8;
                    char *text_end = strchr(text_start, '"');
                    if (text_end) {
                        int len = text_end - text_start;
                        if (len > 31) len = 31;
                        char temp[32] = {0};
                        strncpy(temp, text_start, len);
                        snprintf(weather_text, sizeof(weather_text), "%s %s°C", temp, weather_temp);
                    }
                }
                // 解析icon: "icon":"101"
                char *icon_start = strstr(weather_buf, "\"icon\":\"");
                if (icon_start) {
                    icon_start += 8;
                    char *icon_end = strchr(icon_start, '"');
                    if (icon_end && icon_end - icon_start < 8) {
                        strncpy(weather_icon_code, icon_start, icon_end - icon_start);
                        weather_icon_code[icon_end - icon_start] = '\0';
                    }
                }
            } else if (weather_provider == 2) {
                // 解析OpenWeatherMap: "temp":25.5,"description":"晴天"
                char *temp_start = strstr(weather_buf, "\"temp\":");
                if (temp_start) {
                    temp_start += 7;
                    char *temp_end = strchr(temp_start, ',');
                    if (temp_end) {
                        int len = temp_end - temp_start;
                        if (len > 15) len = 15;
                        strncpy(weather_temp, temp_start, len);
                        weather_temp[len] = '\0';
                    }
                }
                char *desc_start = strstr(weather_buf, "\"description\":\"");
                if (desc_start) {
                    desc_start += 15;
                    char *desc_end = strchr(desc_start, '"');
                    if (desc_end) {
                        int len = desc_end - desc_start;
                        if (len > 31) len = 31;
                        char temp[32] = {0};
                        strncpy(temp, desc_start, len);
                        snprintf(weather_text, sizeof(weather_text), "%s %s°C", temp, weather_temp);
                    }
                }
            }
            ESP_LOGI(TAG, "Weather: %s", weather_text);
            
            // 和风天气额外查24h获取降雨概率
            if (weather_provider == 1 && strlen(location_id) > 0) {
                char url24h[256];
                snprintf(url24h, sizeof(url24h),
                    "https://%s/v7/weather/24h?location=%s&key=%s",
                    qweather_apihost, location_id, qweather_api_key);
                weather_buf[0] = '\0';
                weather_buf_len = 0;
                esp_http_client_set_url(client, url24h);
                esp_http_client_set_method(client, HTTP_METHOD_GET);
                esp_err_t err24h = esp_http_client_perform(client);
                int status24h = esp_http_client_get_status_code(client);
                ESP_LOGI(TAG, "24h API: err=%d, status=%d, buf_len=%d", err24h, status24h, weather_buf_len);
                if (err24h == ESP_OK && status24h == 200) {
                    char dec24h[2048];
                    if (weather_buf_len > 0) {
                        gzip_decompress((unsigned char *)weather_buf, weather_buf_len, dec24h, sizeof(dec24h));
                        // 解析第一个小时的pop: "pop":"10"
                        char *pop_start = strstr(dec24h, "\"pop\":\"");
                        if (pop_start) {
                            pop_start += 7;
                            char *pop_end = strchr(pop_start, '"');
                            if (pop_end && pop_end - pop_start <= 3) {
                                char pop_val[4] = {0};
                                strncpy(pop_val, pop_start, pop_end - pop_start);
                                char tmp[96];
                                snprintf(tmp, sizeof(tmp), "%s 降雨%s%%", weather_text, pop_val);
                                strncpy(weather_text, tmp, sizeof(weather_text) - 1);
                            }
                        }
                    }
                }
                ESP_LOGI(TAG, "Weather+pop: %s", weather_text);
            }
        } else {
            ESP_LOGW(TAG, "Weather API failed: %d, keeping old weather", status);
        }
    } else {
        ESP_LOGW(TAG, "Weather network error, keeping old weather");
    }
    
    esp_http_client_cleanup(client);
}

static void update_hello_page(void)
{
    if (!hello_clock_label) return;
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    if (timeinfo.tm_year < (2016 - 1900)) {
        lv_label_set_text(hello_clock_label, "--:--:--");
        lv_label_set_text(hello_date_label, "----/--/--");
        lv_label_set_text(hello_week_label, "---");
    } else {
        // 大时钟
        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        lv_label_set_text(hello_clock_label, time_buf);
        
        // 日期
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%04d/%02d/%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        lv_label_set_text(hello_date_label, date_buf);
        
        // 星期
        const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        char week_buf[32];
        snprintf(week_buf, sizeof(week_buf), "%s  DAY %d", weekdays[timeinfo.tm_wday], timeinfo.tm_yday + 1);
        lv_label_set_text(hello_week_label, week_buf);
        
        // 检查是否需要刷新一言（每天一次，NTP同步后）
        int today = timeinfo.tm_mday;
        if (ntp_synced && today > 0 && today != last_saying_day) {
            last_saying_day = today;
            fetch_saying();
        }
        
        // 检查是否需要刷新天气（每30分钟一次）
        if (ntp_synced && (timeinfo.tm_hour * 60 + timeinfo.tm_min) / 30 != last_weather_period) {
            last_weather_period = (timeinfo.tm_hour * 60 + timeinfo.tm_min) / 30;
            fetch_weather();
        }
    }
    
    // 只在Hello World页更新城市/天气/图标
    if (info_page_active || deepseek_page_active) return;
    
    // 更新城市显示
    if (hello_city_label) {
        if (weather_provider > 0 && strlen(weather_location) > 0) {
            lv_label_set_text(hello_city_label, weather_location);
            lv_obj_clear_flag(hello_city_label, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGD(TAG, "City: %s", weather_location);
        } else {
            lv_obj_add_flag(hello_city_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // 更新天气显示
    if (hello_weather_label) {
        lv_label_set_text(hello_weather_label, weather_text);
        ESP_LOGD(TAG, "Weather: %s", weather_text);
    }
    
    // 更新天气图标
    if (hello_weather_icon) {
        if (weather_provider > 0 && strlen(weather_icon_code) > 0) {
            char icon_utf8[8];
            icon_code_to_utf8(weather_icon_code, icon_utf8, sizeof(icon_utf8));
            if (strlen(icon_utf8) > 0) {
                lv_label_set_text(hello_weather_icon, icon_utf8);
                lv_obj_clear_flag(hello_weather_icon, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(hello_weather_icon, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(hello_weather_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // 更新一言显示
    if (hello_saying_label) {
        lv_label_set_text(hello_saying_label, saying_text);
    }
}

static void clock_task(void *arg)
{
    int last_hour = -1;
    while(1) {
        if (Lvgl_lock(-1)) {
            update_clock();
            update_hello_page();
            Lvgl_unlock();
        }
        
        // 每天零点刷新最新版本
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2016 - 1900) && timeinfo.tm_hour == 0 && last_hour != 0) {
            ESP_LOGI(TAG, "Midnight refresh - fetching latest version");
            wifi_bsp_fetch_latest_version();
        }
        last_hour = timeinfo.tm_hour;
        
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
static const int info_count = 5;

// DeepSeek page state
static bool deepseek_page_active = false;
static char deepseek_balance[32] = "未知";
static char deepseek_usage[32] = "未知";
static char deepseek_error[64] = "";
static char deepseek_api_key[65] = "";

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
    esp_http_client_set_header(client, "Accept-Encoding", "none");
    
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
    esp_http_client_set_header(client, "Accept-Encoding", "none");
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
        "Accept-Encoding: none\r\n"
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

void weather_set_provider(int provider)
{
    weather_provider = provider;
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", provider);
    nvs_save_string("weather_prov", buf);
}

int weather_get_provider(void)
{
    return weather_provider;
}

void weather_set_qweather_key(const char* key)
{
    strncpy(qweather_api_key, key, sizeof(qweather_api_key) - 1);
    qweather_api_key[sizeof(qweather_api_key) - 1] = '\0';
    nvs_save_string("qweather_key", qweather_api_key);
}

const char* weather_get_qweather_key(void)
{
    return qweather_api_key;
}

void weather_set_qweather_apihost(const char* host)
{
    strncpy(qweather_apihost, host, sizeof(qweather_apihost) - 1);
    qweather_apihost[sizeof(qweather_apihost) - 1] = '\0';
    nvs_save_string("qweather_host", qweather_apihost);
}

const char* weather_get_qweather_apihost(void)
{
    return qweather_apihost;
}

void weather_set_openweather_key(const char* key)
{
    strncpy(openweather_api_key, key, sizeof(openweather_api_key) - 1);
    openweather_api_key[sizeof(openweather_api_key) - 1] = '\0';
    nvs_save_string("openweather_key", openweather_api_key);
}

const char* weather_get_openweather_key(void)
{
    return openweather_api_key;
}

void weather_set_location(const char* loc)
{
    strncpy(weather_location, loc, sizeof(weather_location) - 1);
    weather_location[sizeof(weather_location) - 1] = '\0';
    nvs_save_string("weather_loc", weather_location);
}

const char* weather_get_location(void)
{
    return weather_location;
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
    const char* cursor5 = (info_selected == 4) ? "> " : "  ";
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
             "%s同步时间\n"
             "%s更新天气",
             FIRMWARE_VERSION,
             wifi_bsp_get_latest_version(),
             ntp_status,
             cursor1,
             wifi_bsp_is_connected() ? "已连接" : "未连接",
             cursor2,
             ap_status,
             cursor3,
             sec_status,
             cursor4,
             cursor5);
    
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
            case 0: // Hello World (大时钟)
                info_page_active = false;
                deepseek_page_active = false;
                lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(hello_clock_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(hello_date_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(hello_week_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(hello_city_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(hello_weather_icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(hello_weather_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(hello_saying_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar1, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar1_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar2, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(mimo_bar2_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ota_btn_label, LV_OBJ_FLAG_HIDDEN);
                update_hello_page();
                break;
            case 1: // Info
                info_page_active = true;
                deepseek_page_active = false;
                lv_obj_clear_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_clock_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_date_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_week_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_city_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_weather_icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_weather_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_saying_label, LV_OBJ_FLAG_HIDDEN);
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
                lv_obj_clear_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_clock_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_date_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_week_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_city_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_weather_icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_weather_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hello_saying_label, LV_OBJ_FLAG_HIDDEN);
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
    // 小时钟（左上角，用于其他页面）
    clock_label = lv_label_create(lv_scr_act());
    lv_label_set_text(clock_label, "--:--:--");
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_28, 0);
    lv_obj_align(clock_label, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
    
    // 大时钟（居中偏上）
    hello_clock_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_clock_label, "--:--:--");
    lv_obj_set_style_text_font(hello_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(hello_clock_label, lv_color_hex(0x000000), 0);
    lv_obj_align(hello_clock_label, LV_ALIGN_CENTER, 0, -45);
    
    // 日期（时钟上方）
    hello_date_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_date_label, "----/--/--");
    lv_obj_set_style_text_font(hello_date_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(hello_date_label, lv_color_hex(0x444444), 0);
    lv_obj_align(hello_date_label, LV_ALIGN_CENTER, 0, -85);
    
    // 星期（时钟下方）
    hello_week_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_week_label, "---");
    lv_obj_set_style_text_font(hello_week_label, &lv_font_MiSansLight_16, 0);
    lv_obj_set_style_text_color(hello_week_label, lv_color_hex(0x444444), 0);
    lv_obj_align(hello_week_label, LV_ALIGN_CENTER, 0, 0);
    
    // 城市（星期下方）
    hello_city_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_city_label, "");
    lv_obj_set_style_text_font(hello_city_label, &lv_font_MiSansLight_16, 0);
    lv_obj_set_style_text_color(hello_city_label, lv_color_hex(0x444444), 0);
    lv_obj_align(hello_city_label, LV_ALIGN_CENTER, 0, 25);
    lv_obj_add_flag(hello_city_label, LV_OBJ_FLAG_HIDDEN);
    
    // 天气图标（天气文字左边）
    hello_weather_icon = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_weather_icon, "");
    lv_obj_set_style_text_font(hello_weather_icon, &lv_font_qweather_icons_24, 0);
    lv_obj_set_style_text_color(hello_weather_icon, lv_color_hex(0x444444), 0);
    lv_obj_align(hello_weather_icon, LV_ALIGN_CENTER, -60, 47);
    lv_obj_add_flag(hello_weather_icon, LV_OBJ_FLAG_HIDDEN);
    
    // 天气（城市下方）
    hello_weather_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_weather_label, "");
    lv_obj_set_style_text_font(hello_weather_label, &lv_font_MiSansLight_16, 0);
    lv_obj_set_style_text_color(hello_weather_label, lv_color_hex(0x444444), 0);
    lv_obj_align(hello_weather_label, LV_ALIGN_CENTER, 10, 47);
    
    // 分隔线（天气和一言之间）
    lv_obj_t *sep_line = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sep_line, 160, 1);
    lv_obj_set_style_bg_color(sep_line, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_width(sep_line, 0, 0);
    lv_obj_set_style_radius(sep_line, 0, 0);
    lv_obj_set_style_pad_all(sep_line, 0, 0);
    lv_obj_align(sep_line, LV_ALIGN_CENTER, 0, 65);
    
    // 一言（分隔线下方）
    hello_saying_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_saying_label, "...");
    lv_obj_set_style_text_font(hello_saying_label, &lv_font_MiSansLight_16, 0);
    lv_obj_set_style_text_color(hello_saying_label, lv_color_hex(0x666666), 0);
    lv_obj_align(hello_saying_label, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_width(hello_saying_label, 360);
    lv_obj_set_style_text_align(hello_saying_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // Create main label (default page - English only)
    hello_label = lv_label_create(lv_scr_act());
    lv_label_set_text(hello_label, "Hello World!");
    lv_obj_set_style_text_font(hello_label, &lv_font_montserrat_28, 0);
    lv_obj_align(hello_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(hello_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create DeepSeek label (left side, hidden by default)
    deepseek_label = lv_label_create(lv_scr_act());
    lv_label_set_text(deepseek_label, "");
    lv_obj_set_style_text_font(deepseek_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(deepseek_label, LV_ALIGN_TOP_LEFT, 15, 55);
    lv_obj_set_width(deepseek_label, 180);
    lv_obj_add_flag(deepseek_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create MiMo label (title only - same Y as DeepSeek)
    mimo_label = lv_label_create(lv_scr_act());
    lv_label_set_text(mimo_label, "");
    lv_obj_set_style_text_font(mimo_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(mimo_label, LV_ALIGN_TOP_LEFT, 190, 55);
    lv_obj_set_width(mimo_label, 200);
    lv_obj_add_flag(mimo_label, LV_OBJ_FLAG_HIDDEN);
    
    // Bar 1 label (monthly usage + percent)
    mimo_bar1_label = lv_label_create(lv_scr_act());
    lv_label_set_text(mimo_bar1_label, "本月: 0.0B / 0.0B (0.0%)");
    lv_obj_set_style_text_font(mimo_bar1_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(mimo_bar1_label, LV_ALIGN_TOP_LEFT, 190, 80);
    lv_obj_add_flag(mimo_bar1_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create MiMo progress bar 1 (monthly)
    mimo_bar1 = lv_bar_create(lv_scr_act());
    lv_obj_set_size(mimo_bar1, 140, 14);
    lv_obj_align(mimo_bar1, LV_ALIGN_TOP_LEFT, 190, 105);
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
    
    // Bar 2 label (total usage + percent)
    mimo_bar2_label = lv_label_create(lv_scr_act());
    lv_label_set_text(mimo_bar2_label, "总套餐: 0.0B / 0.0B (0.0%)");
    lv_obj_set_style_text_font(mimo_bar2_label, &lv_font_MiSansLight_16, 0);
    lv_obj_align(mimo_bar2_label, LV_ALIGN_TOP_LEFT, 190, 130);
    lv_obj_add_flag(mimo_bar2_label, LV_OBJ_FLAG_HIDDEN);
    
    // Create MiMo progress bar 2 (total)
    mimo_bar2 = lv_bar_create(lv_scr_act());
    lv_obj_set_size(mimo_bar2, 140, 14);
    lv_obj_align(mimo_bar2, LV_ALIGN_TOP_LEFT, 190, 155);
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
    
    // Charge icon (lightning) - moved left to avoid overlap
    charge_icon = lv_label_create(lv_scr_act());
    lv_label_set_text(charge_icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(charge_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(charge_icon, LV_ALIGN_BOTTOM_RIGHT, -70, -5);
    lv_obj_add_flag(charge_icon, LV_OBJ_FLAG_HIDDEN);
    
    // WiFi icon
    wifi_icon = lv_label_create(lv_scr_act());
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_BOTTOM_RIGHT, -95, -5);
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
                
                // Show charge icon when charging detected
                if (charge_icon) {
                    if (adc_bsp_is_charging()) {
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
                        case 4:  // 更新天气
                            fetch_weather();
                            update_hello_page();
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

static void Lvgl_FlushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *color_map)
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
    lv_disp_flush_ready(disp);
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
    nvs_load_string("qweather_key", qweather_api_key, sizeof(qweather_api_key));
    nvs_load_string("qweather_host", qweather_apihost, sizeof(qweather_apihost));
    nvs_load_string("openweather_key", openweather_api_key, sizeof(openweather_api_key));
    nvs_load_string("weather_loc", weather_location, sizeof(weather_location));
    
    // 读取天气提供商
    char provider_str[4] = "0";
    nvs_load_string("weather_prov", provider_str, sizeof(provider_str));
    weather_provider = atoi(provider_str);
    
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
    xTaskCreatePinnedToCore(clock_task, "clock_task", 8 * 1024, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(ntp_task, "ntp_task", 4 * 1024, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(status_bar_task, "status_bar", 4 * 1024, NULL, 2, NULL, 1);
    
    ESP_LOGI(TAG, "Menu system ready!");
    ESP_LOGI(TAG, "Web admin: http://[IP]");
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
