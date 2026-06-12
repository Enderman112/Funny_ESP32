#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "wifi_bsp.h"
#include <string.h>
#include <stdio.h>

extern void ntp_set_server(const char* server);
extern const char* ntp_get_server(void);
extern void ntp_set_timezone(const char* tz);
extern const char* ntp_get_timezone(void);
extern void clock_set_show_seconds(bool show);
extern bool clock_get_show_seconds(void);
extern void ntp_sync_now(void);
extern const char* wifi_bsp_get_latest_version(void);
extern void weather_set_provider(int provider);
extern int weather_get_provider(void);
extern void weather_set_qweather_key(const char* key);
extern const char* weather_get_qweather_key(void);
extern void weather_set_qweather_apihost(const char* host);
extern const char* weather_get_qweather_apihost(void);
extern void weather_set_openweather_key(const char* key);
extern const char* weather_get_openweather_key(void);
extern void weather_set_location(const char* loc);
extern const char* weather_get_location(void);
extern void weather_set_refresh_min(int min);
extern int weather_get_refresh_min(void);

static const char *TAG = "WebServer";

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// URL解码函数
static void url_decode(char *str) {
    char *p = str;
    char hex[3] = {0};
    while (*str) {
        if (*str == '%') {
            if (str[1] && str[2]) {
                hex[0] = str[1];
                hex[1] = str[2];
                *p = (char)strtol(hex, NULL, 16);
                str += 3;
            } else {
                *p = *str++;
            }
        } else if (*str == '+') {
            *p = ' ';
            str++;
        } else {
            *p = *str++;
        }
        p++;
    }
    *p = '\0';
}

#define WEB_BUF_SIZE 8192
#define OTA_UPLOAD_BUF_SIZE 4096

static bool ota_in_progress = false;
static char ota_status[128] = "";
static char ota_url[256] = "";

// ===== HTML模板 =====
static const char* HTML_HEADER = "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>Funny ESP32</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box;}"
    "body{font-family:'Segoe UI',Arial,sans-serif;background:#f5f5f5;color:#333;font-size:14px;}"
    "#header{background:linear-gradient(135deg,#4a90d9,#357abd);color:white;padding:15px 20px;display:flex;justify-content:space-between;align-items:center;}"
    "#header h1{font-size:20px;font-weight:600;}"
    "#header .version{font-size:12px;opacity:0.8;}"
    "#menubar{background:#fff;border-bottom:1px solid #ddd;padding:0 20px;display:flex;}"
    "#menubar a{display:inline-block;padding:12px 20px;color:#555;text-decoration:none;border-bottom:3px solid transparent;transition:all 0.2s;}"
    "#menubar a:hover,#menubar a.active{color:#4a90d9;border-bottom-color:#4a90d9;background:#f8f9fa;}"
    "#content{max-width:960px;margin:20px auto;padding:0 20px;}"
    ".container{background:white;border-radius:4px;box-shadow:0 1px 3px rgba(0,0,0,0.1);margin-bottom:20px;}"
    ".container .header{background:#f8f9fa;padding:12px 16px;border-bottom:1px solid #e9ecef;font-weight:600;color:#495057;border-radius:4px 4px 0 0;display:flex;align-items:center;}"
    ".container .header .icon{margin-right:8px;}"
    ".container .body{padding:16px;}"
    ".table{width:100%;border-collapse:collapse;}"
    ".table td,.table th{padding:8px 12px;text-align:left;border-bottom:1px solid #eee;}"
    ".table th{background:#f8f9fa;color:#495057;font-weight:600;width:140px;}"
    ".btn{display:inline-block;padding:8px 16px;border:none;border-radius:3px;cursor:pointer;font-size:13px;transition:background 0.2s;}"
    ".btn-primary{background:#4a90d9;color:white;}"
    ".btn-primary:hover{background:#357abd;}"
    ".btn-success{background:#28a745;color:white;}"
    ".btn-success:hover{background:#218838;}"
    "input[type=text],input[type=password],input[type=file]{width:100%;padding:8px 12px;border:1px solid #ced4da;border-radius:3px;font-size:13px;transition:border-color 0.2s;}"
    "input[type=text]:focus,input[type=password]:focus{outline:none;border-color:#4a90d9;box-shadow:0 0 0 2px rgba(74,144,217,0.2);}"
    ".alert{padding:10px 14px;border-radius:3px;margin-bottom:16px;}"
    ".alert-success{background:#d4edda;color:#155724;border:1px solid #c3e6cb;}"
    ".alert-danger{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;}"
    ".badge{display:inline-block;padding:3px 8px;border-radius:12px;font-size:12px;font-weight:600;}"
    ".badge-success{background:#28a745;color:white;}"
    ".badge-danger{background:#dc3545;color:white;}"
    ".form-group{margin-bottom:14px;}"
    ".form-group label{display:block;margin-bottom:4px;color:#495057;font-weight:500;}"
    ".footer{text-align:center;padding:20px;color:#6c757d;font-size:12px;}"
    ".progress{height:20px;background:#e9ecef;border-radius:4px;overflow:hidden;margin:10px 0;}"
    ".progress-bar{height:100%;background:#4a90d9;width:0%;transition:width 0.3s;}"
    "</style></head><body>"
    "<div id='header'><h1>Funny ESP32</h1><span class='version'>%s</span></div>"
    "<div id='menubar'>"
    "<a href='/'>状态</a>"
    "<a href='/network'>网络</a>"
    "<a href='/ota'>固件</a>"
    "<a href='/settings'>设置</a>"
    "</div>"
    "<div id='content'>";

static const char* HTML_FOOTER = "<div class='footer'>Funny ESP32 &copy; 2026 | Powered by ESP-IDF | <a href='https://www.qweather.com' target='_blank'>天气服务由和风天气驱动</a></div>"
    "</div></body></html>";

// ===== 页面：状态 =====
static esp_err_t root_handler(httpd_req_t *req)
{
    char *buf = malloc(WEB_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int len = 0;
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, HTML_HEADER, FIRMWARE_VERSION);
    
    // WiFi状态
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128246;</span>WiFi 状态</div><div class='body'>"
        "<table class='table'>");
    if (wifi_bsp_is_connected()) {
        len += snprintf(buf + len, WEB_BUF_SIZE - len, 
            "<tr><th>状态</th><td><span class='badge badge-success'>已连接</span></td></tr>"
            "<tr><th>名称</th><td>%s</td></tr>"
            "<tr><th>IP 地址</th><td>%s</td></tr>",
            wifi_bsp_get_ssid(), wifi_bsp_get_ip());
    } else {
        len += snprintf(buf + len, WEB_BUF_SIZE - len, 
            "<tr><th>状态</th><td><span class='badge badge-danger'>未连接</span></td></tr>"
            "<tr><th>名称</th><td>%s</td></tr>",
            wifi_bsp_get_ssid());
    }
    len += snprintf(buf + len, WEB_BUF_SIZE - len, "</table></div></div>");
    
    // 系统信息
    const char* latest_ver = wifi_bsp_get_latest_version();
    len += snprintf(buf + len, WEB_BUF_SIZE - len,
        "<div class='container'><div class='header'><span class='icon'>&#128187;</span>系统信息</div><div class='body'>"
        "<table class='table'>"
        "<tr><th>当前版本</th><td>%s</td></tr>"
        "<tr><th>最新版本</th><td>%s</td></tr>"
        "</table></div></div>",
        FIRMWARE_VERSION, latest_ver);
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, "%s", HTML_FOOTER);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// ===== 页面：网络 =====
static esp_err_t network_handler(httpd_req_t *req)
{
    char *buf = malloc(WEB_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int len = 0;
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, HTML_HEADER, FIRMWARE_VERSION);
    
    // WiFi配置
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128268;</span>WiFi 配置</div><div class='body'>"
        "<form action='/wifi' method='post'>"
        "<div class='form-group'><label>SSID</label><input type='text' name='ssid' placeholder='输入WiFi名称' required></div>"
        "<div class='form-group'><label>密码</label><input type='password' name='password' placeholder='输入密码' required></div>"
        "<button type='submit' class='btn btn-primary'>连接</button>"
        "</form></div></div>");
    
    // AP配置
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128225;</span>热点配置</div><div class='body'>"
        "<table class='table'><tr><th>热点名称</th><td>Funny_ESP32</td></tr></table>"
        "<form action='/ap' method='post' style='margin-top:12px;'>"
        "<div class='form-group'><label>新密码</label><input type='password' name='password' placeholder='输入新密码' required></div>"
        "<button type='submit' class='btn btn-primary'>修改密码</button>"
        "</form></div></div>");
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, "%s", HTML_FOOTER);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// ===== 页面：固件更新 =====
static esp_err_t ota_handler_page(httpd_req_t *req)
{
    char *buf = malloc(WEB_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int len = 0;
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, HTML_HEADER, FIRMWARE_VERSION);
    
    // 远程OTA
    const char* latest_ver = wifi_bsp_get_latest_version();
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128230;</span>远程更新</div><div class='body'>"
        "<table class='table'>"
        "<tr><th>当前版本</th><td>%s</td></tr>"
        "<tr><th>最新版本</th><td>%s</td></tr>"
        "</table>"
        "<form action='/ota' method='post' style='margin-top:12px;'>"
        "<button type='submit' class='btn btn-primary'%s>检查并更新</button>"
        "</form>"
        "%s</div></div>",
        FIRMWARE_VERSION, latest_ver,
        ota_in_progress ? " disabled" : "",
        ota_status);
    
    // 本地OTA
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128190;</span>本地更新</div><div class='body'>"
        "<p style='margin-bottom:12px;color:#6c757d;'>选择本地固件文件 (screen.bin) 进行更新</p>"
        "<form action='/upload' method='post' enctype='multipart/form-data'>"
        "<div class='form-group'><input type='file' name='firmware' accept='.bin' required></div>"
        "<button type='submit' class='btn btn-success'>上传并更新</button>"
        "</form>"
        "<div id='upload-status'></div>"
        "</div></div>");
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, "%s", HTML_FOOTER);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// ===== 页面：设置 =====
static esp_err_t settings_handler(httpd_req_t *req)
{
    char *buf = malloc(WEB_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int len = 0;
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, HTML_HEADER, FIRMWARE_VERSION);
    
    // DeepSeek API
    const char* current_key = deepseek_get_api_key();
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#129302;</span>DeepSeek API</div><div class='body'>"
        "<form action='/apikey' method='post'>"
        "<div class='form-group'><label>API 密钥</label><input type='password' name='apikey' value='%s' placeholder='sk-...'></div>"
        "<button type='submit' class='btn btn-primary'>保存</button>"
        "</form></div></div>",
        current_key ? current_key : "");
    
    // MiMo Cookie
    const char* current_cookie = mimo_get_cookie();
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#127850;</span>MiMo Cookie</div><div class='body'>"
        "<form action='/mimo' method='post'>"
        "<div class='form-group'><label>Cookie</label><input type='password' name='cookie' value='%s' placeholder='粘贴Cookie'></div>"
        "<button type='submit' class='btn btn-primary'>保存</button>"
        "</form></div></div>",
        current_cookie ? current_cookie : "");
    
    // NTP配置
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128339;</span>NTP 时间同步</div><div class='body'>"
        "<form action='/ntp' method='post'>"
        "<div class='form-group'><label>NTP 服务器</label><input type='text' name='server' value='%s'></div>"
        "<div class='form-group'><label>时区</label><input type='text' name='timezone' value='%s'>"
        "<small style='color:#6c757d;'>UTC偏移量，如 +8(中国) +9(日本) -5(美国东部)</small></div>"
        "<button type='submit' class='btn btn-primary'>保存</button>"
        "</form>"
        "<form action='/sync' method='post' style='margin-top:10px;'>"
        "<button type='submit' class='btn btn-primary'>立即同步</button>"
        "</form></div></div>",
        ntp_get_server(), ntp_get_timezone());
    
    // 天气配置
    int provider = weather_get_provider();
    const char* qweather_key = weather_get_qweather_key();
    const char* qweather_apihost = weather_get_qweather_apihost();
    const char* openweather_key = weather_get_openweather_key();
    const char* location = weather_get_location();
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#127782;</span>天气配置</div><div class='body'>"
        "<form action='/weather' method='post'>"
        "<div class='form-group'><label>天气提供商</label>"
        "<select name='provider' style='width:100%%;padding:8px;border:1px solid #ced4da;border-radius:3px;'>"
        "<option value='0'%s>关闭</option>"
        "<option value='1'%s>和风天气</option>"
        "<option value='2'%s>OpenWeatherMap</option>"
        "</select></div>"
        "<div class='form-group'><label>和风 API Host</label><input type='text' name='qweather_apihost' value='%s' placeholder='xxx.re.qweatherapi.com'>"
        "<small style='color:#6c757d;'>在和风控制台查看，如 nf63yxx47w.re.qweatherapi.com</small></div>"
        "<div class='form-group'><label>和风天气 API Key</label><input type='password' name='qweather_key' value='%s' placeholder='粘贴Key'></div>"
        "<div class='form-group'><label>地区/城市ID</label><input type='text' name='location' value='%s'>"
        "<small style='color:#6c757d;'>和风: 城市ID如101010100 | OW: 纬度,经度如39.9,116.4</small></div>"
        "<div class='form-group'><label>OpenWeather API Key</label><input type='password' name='openweather_key' value='%s' placeholder='粘贴Key'></div>"
        "<div class='form-group'><label>刷新频率(分钟)</label><input type='number' name='weather_ref' value='%d' min='5' max='360'>"
        "<small style='color:#6c757d;'>天气自动刷新间隔，5-360分钟，默认30</small></div>"
        "<button type='submit' class='btn btn-primary'>保存</button>"
        "</form></div></div>",
        provider == 0 ? " selected" : "",
        provider == 1 ? " selected" : "",
        provider == 2 ? " selected" : "",
        qweather_apihost ? qweather_apihost : "",
        qweather_key ? qweather_key : "",
        location ? location : "",
        openweather_key ? openweather_key : "",
        weather_get_refresh_min());
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, "%s", HTML_FOOTER);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// ===== 处理WiFi配置 =====
static esp_err_t wifi_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char ssid[33] = {0};
    char password[65] = {0};
    
    char *ssid_start = strstr(buf, "ssid=");
    char *pwd_start = strstr(buf, "password=");
    
    if (ssid_start && pwd_start) {
        ssid_start += 5;
        pwd_start += 9;
        
        char *ssid_end = strchr(ssid_start, '&');
        if (ssid_end) {
            int ssid_len = ssid_end - ssid_start;
            if (ssid_len > 32) ssid_len = 32;
            strncpy(ssid, ssid_start, ssid_len);
        }
        
        strncpy(password, pwd_start, 64);
        
        url_decode(ssid);
        url_decode(password);
        
        ESP_LOGI(TAG, "New WiFi config: SSID=%s", ssid);
        wifi_bsp_connect(ssid, password);
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/network");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== 处理AP配置 =====
static esp_err_t ap_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char password[65] = {0};
    char *pwd_start = strstr(buf, "password=");
    if (pwd_start) {
        pwd_start += 9;
        strncpy(password, pwd_start, 64);
        url_decode(password);
        ESP_LOGI(TAG, "New AP password: %s", password);
        wifi_bsp_update_ap_password(password);
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/network");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== 处理DeepSeek API Key =====
static esp_err_t apikey_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char apikey[65] = {0};
    char *apikey_start = strstr(buf, "apikey=");
    if (apikey_start) {
        apikey_start += 7;
        strncpy(apikey, apikey_start, 64);
        url_decode(apikey);
        ESP_LOGI(TAG, "New API key configured");
        deepseek_set_api_key(apikey);
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== 处理MiMo Cookie =====
static esp_err_t mimo_handler(httpd_req_t *req)
{
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char cookie[640] = {0};
    char *cookie_start = strstr(buf, "cookie=");
    if (cookie_start) {
        cookie_start += 7;
        strncpy(cookie, cookie_start, 639);
        url_decode(cookie);
        ESP_LOGI(TAG, "MiMo cookie updated, len=%d", strlen(cookie));
        mimo_set_cookie(cookie);
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== 处理NTP配置 =====
static esp_err_t ntp_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char server[64] = {0};
    char timezone[32] = {0};
    
    char *server_start = strstr(buf, "server=");
    char *tz_start = strstr(buf, "timezone=");
    
    if (server_start && tz_start) {
        server_start += 7;
        tz_start += 9;
        
        char *server_end = strchr(server_start, '&');
        if (server_end) {
            int len = server_end - server_start;
            if (len > 63) len = 63;
            strncpy(server, server_start, len);
        }
        
        strncpy(timezone, tz_start, 31);
        
        url_decode(server);
        // 时区保留+号
        for (int i = 0; timezone[i]; i++) {
            if (timezone[i] == '%' && timezone[i+1] == '2' && timezone[i+2] == 'B') {
                timezone[i] = '+';
                memmove(&timezone[i+1], &timezone[i+3], strlen(&timezone[i+3]) + 1);
            }
        }
        
        ESP_LOGI(TAG, "NTP config: server=%s, tz=%s", server, timezone);
        ntp_set_server(server);
        ntp_set_timezone(timezone);
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== 处理NTP同步 =====
static esp_err_t sync_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Manual NTP sync requested");
    ntp_sync_now();
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t weather_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char provider_str[4] = "0";
    char location[32] = {0};
    char qweather_key[65] = {0};
    char openweather_key[65] = {0};
    
    // Parse provider
    char *p = strstr(buf, "provider=");
    if (p) {
        p += 9;
        char *end = strchr(p, '&');
        if (end) {
            int len = end - p;
            if (len > 3) len = 3;
            strncpy(provider_str, p, len);
        }
    }
    
    // Parse location
    p = strstr(buf, "location=");
    if (p) {
        p += 9;
        char *end = strchr(p, '&');
        if (end) {
            int len = end - p;
            if (len > 31) len = 31;
            strncpy(location, p, len);
            url_decode(location);
        }
    }
    
    // Parse qweather_key
    p = strstr(buf, "qweather_key=");
    if (p) {
        p += 13;
        char *end = strchr(p, '&');
        if (end) {
            int len = end - p;
            if (len > 64) len = 64;
            strncpy(qweather_key, p, len);
            url_decode(qweather_key);
        }
    }
    
    // Parse qweather_apihost
    char qweather_apihost[64] = {0};
    p = strstr(buf, "qweather_apihost=");
    if (p) {
        p += 17;
        char *end = strchr(p, '&');
        if (end) {
            int len = end - p;
            if (len > 63) len = 63;
            strncpy(qweather_apihost, p, len);
            url_decode(qweather_apihost);
        }
    }
    
    // Parse openweather_key
    p = strstr(buf, "openweather_key=");
    if (p) {
        p += 16;
        char *end = strchr(p, '&');
        if (end) {
            int len = end - p;
            if (len > 64) len = 64;
            strncpy(openweather_key, p, len);
            url_decode(openweather_key);
        }
    }
    
    ESP_LOGI(TAG, "Weather config: provider=%s, loc=%s", provider_str, location);
    weather_set_provider(atoi(provider_str));
    if (strlen(location) > 0) weather_set_location(location);
    if (strlen(qweather_key) > 0) weather_set_qweather_key(qweather_key);
    if (strlen(qweather_apihost) > 0) weather_set_qweather_apihost(qweather_apihost);
    if (strlen(openweather_key) > 0) weather_set_openweather_key(openweather_key);
    
    // Parse weather_ref
    p = strstr(buf, "weather_ref=");
    if (p) {
        p += 12;
        char ref_str[8] = {0};
        int i = 0;
        while (p[i] >= '0' && p[i] <= '9' && i < 4) {
            ref_str[i] = p[i];
            i++;
        }
        if (i > 0) weather_set_refresh_min(atoi(ref_str));
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== OTA任务 =====
static void ota_task(void *arg)
{
    char *url = (char*)arg;
    int max_retries = 3;
    int retry = 0;
    
    // 关闭WiFi省电模式，避免长时间下载时断连
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    while (retry < max_retries) {
        ESP_LOGI(TAG, "OTA attempt %d/%d from: %s", retry + 1, max_retries, url);
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-success'>正在更新... (%d/%d)</div>", retry + 1, max_retries);
        
        esp_http_client_config_t config = {};
        config.url = url;
        config.timeout_ms = 120000;
        config.keep_alive_enable = true;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        
        esp_https_ota_config_t ota_config = {};
        ota_config.http_config = &config;
        
        esp_https_ota_handle_t ota_handle = NULL;
        esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
            retry++;
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        
        while (1) {
            err = esp_https_ota_perform(ota_handle);
            if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                continue;
            } else if (err == ESP_OK) {
                break;
            } else {
                ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
                esp_https_ota_abort(ota_handle);
                retry++;
                vTaskDelay(pdMS_TO_TICKS(3000));
                break;
            }
        }
        
        if (err == ESP_OK) {
            if (esp_https_ota_is_complete_data_received(ota_handle)) {
                err = esp_https_ota_finish(ota_handle);
                if (err == ESP_OK) {
                    snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-success'>更新成功，重启中...</div>");
                    ESP_LOGI(TAG, "OTA success, restarting...");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
            } else {
                esp_https_ota_abort(ota_handle);
                retry++;
            }
        }
    }
    
    snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>更新失败，已重试%d次</div>", max_retries);
    ota_in_progress = false;
    vTaskDelete(NULL);
}

// ===== 处理远程OTA =====
static esp_err_t ota_handler(httpd_req_t *req)
{
    if (ota_in_progress) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/ota");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    snprintf(ota_url, sizeof(ota_url), "https://luoyun.eu.org/firmware/screen.bin");
    
    ota_in_progress = true;
    snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-success'>开始更新...</div>");
    
    xTaskCreate(ota_task, "ota_task", 8192, ota_url, 5, NULL);
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/ota");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== 本地OTA任务 =====
static uint8_t *ota_firmware_buf = NULL;
static size_t ota_firmware_size = 0;

static void local_ota_task(void *arg)
{
    if (ota_firmware_buf == NULL || ota_firmware_size == 0) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>固件数据为空</div>");
        ota_in_progress = false;
        free(ota_firmware_buf);
        ota_firmware_buf = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>找不到OTA分区</div>");
        ota_in_progress = false;
        free(ota_firmware_buf);
        ota_firmware_buf = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Local OTA: partition=%s, size=%d", update_partition->label, ota_firmware_size);
    
    esp_ota_handle_t update_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>OTA开始失败</div>");
        ota_in_progress = false;
        free(ota_firmware_buf);
        ota_firmware_buf = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    err = esp_ota_write(update_handle, ota_firmware_buf, ota_firmware_size);
    if (err != ESP_OK) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>写入失败</div>");
        esp_ota_abort(update_handle);
        ota_in_progress = false;
        free(ota_firmware_buf);
        ota_firmware_buf = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>OTA验证失败</div>");
        ota_in_progress = false;
        free(ota_firmware_buf);
        ota_firmware_buf = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>设置启动分区失败</div>");
        ota_in_progress = false;
        free(ota_firmware_buf);
        ota_firmware_buf = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-success'>更新成功，重启中...</div>");
    ESP_LOGI(TAG, "Local OTA success! Size: %d bytes", ota_firmware_size);
    
    free(ota_firmware_buf);
    ota_firmware_buf = NULL;
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// ===== 处理本地OTA上传 =====
static esp_err_t upload_handler(httpd_req_t *req)
{
    if (ota_in_progress) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/ota");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    ota_in_progress = true;
    snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-success'>正在接收固件...</div>");
    
    // 读取整个请求到缓冲区
    size_t buf_size = req->content_len;
    uint8_t *full_buf = malloc(buf_size);
    if (!full_buf) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>内存不足</div>");
        ota_in_progress = false;
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/ota");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    size_t total = 0;
    while (total < buf_size) {
        int ret = httpd_req_recv(req, (char*)(full_buf + total), buf_size - total);
        if (ret <= 0) break;
        total += ret;
    }
    
    ESP_LOGI(TAG, "Received %d bytes (content_len=%d)", total, buf_size);
    
    // 找到固件数据的起始位置（跳过multipart头）
    uint8_t *fw_start = NULL;
    size_t fw_size = 0;
    
    // 搜索0xE9魔数字节（ESP32固件头）
    for (size_t i = 0; i < total - 4; i++) {
        if (full_buf[i] == 0xE9 && full_buf[i+1] == 0x06) {
            fw_start = full_buf + i;
            fw_size = total - i;
            ESP_LOGI(TAG, "Found firmware at offset %d, size=%d", i, fw_size);
            break;
        }
    }
    
    if (!fw_start || fw_size < 1024) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>无效的固件文件</div>");
        free(full_buf);
        ota_in_progress = false;
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/ota");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // 复制固件数据
    ota_firmware_size = fw_size;
    ota_firmware_buf = malloc(ota_firmware_size);
    if (!ota_firmware_buf) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>内存不足</div>");
        free(full_buf);
        ota_in_progress = false;
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/ota");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    memcpy(ota_firmware_buf, fw_start, fw_size);
    free(full_buf);
    
    ESP_LOGI(TAG, "Firmware extracted: %d bytes", ota_firmware_size);
    
    // 启动OTA任务
    xTaskCreate(local_ota_task, "local_ota", 8192, NULL, 5, NULL);
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/ota");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== 初始化Web服务器 =====
void web_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        // 页面路由
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_uri_t network = { .uri = "/network", .method = HTTP_GET, .handler = network_handler };
        httpd_uri_t ota_page = { .uri = "/ota", .method = HTTP_GET, .handler = ota_handler_page };
        httpd_uri_t settings = { .uri = "/settings", .method = HTTP_GET, .handler = settings_handler };
        
        // 表单处理
        httpd_uri_t wifi_post = { .uri = "/wifi", .method = HTTP_POST, .handler = wifi_handler };
        httpd_uri_t ap_post = { .uri = "/ap", .method = HTTP_POST, .handler = ap_handler };
        httpd_uri_t apikey_post = { .uri = "/apikey", .method = HTTP_POST, .handler = apikey_handler };
        httpd_uri_t mimo_post = { .uri = "/mimo", .method = HTTP_POST, .handler = mimo_handler };
        httpd_uri_t ntp_post = { .uri = "/ntp", .method = HTTP_POST, .handler = ntp_handler };
        httpd_uri_t sync_post = { .uri = "/sync", .method = HTTP_POST, .handler = sync_handler };
        httpd_uri_t weather_post = { .uri = "/weather", .method = HTTP_POST, .handler = weather_handler };
        httpd_uri_t ota_post = { .uri = "/ota", .method = HTTP_POST, .handler = ota_handler };
        httpd_uri_t upload_post = { .uri = "/upload", .method = HTTP_POST, .handler = upload_handler };
        
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &network);
        httpd_register_uri_handler(server, &ota_page);
        httpd_register_uri_handler(server, &settings);
        httpd_register_uri_handler(server, &wifi_post);
        httpd_register_uri_handler(server, &ap_post);
        httpd_register_uri_handler(server, &apikey_post);
        httpd_register_uri_handler(server, &mimo_post);
        httpd_register_uri_handler(server, &ntp_post);
        httpd_register_uri_handler(server, &sync_post);
        httpd_register_uri_handler(server, &weather_post);
        httpd_register_uri_handler(server, &ota_post);
        httpd_register_uri_handler(server, &upload_post);
        
        ESP_LOGI(TAG, "Web server started on port 80");
    }
}