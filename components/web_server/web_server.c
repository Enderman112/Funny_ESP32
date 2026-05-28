#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "wifi_bsp.h"
#include <string.h>
#include <stdio.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

extern void ntp_set_server(const char* server);
extern const char* ntp_get_server(void);
extern void ntp_set_timezone(const char* tz);
extern const char* ntp_get_timezone(void);
extern void clock_set_show_seconds(bool show);
extern bool clock_get_show_seconds(void);
extern void ntp_sync_now(void);
extern const char* wifi_bsp_get_latest_version(void);

static const char *TAG = "WebServer";

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

// 时区专用解码（保留+号）
static void url_decode_tz(char *str) {
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
        } else {
            *p = *str++;
        }
        p++;
    }
    *p = '\0';
}

static const char* HTML_HEADER = "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>Funny ESP32 - LuCI</title>"
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
    "input[type=text],input[type=password]{width:100%;padding:8px 12px;border:1px solid #ced4da;border-radius:3px;font-size:13px;transition:border-color 0.2s;}"
    "input[type=text]:focus,input[type=password]:focus{outline:none;border-color:#4a90d9;box-shadow:0 0 0 2px rgba(74,144,217,0.2);}"
    ".alert{padding:10px 14px;border-radius:3px;margin-bottom:16px;}"
    ".alert-success{background:#d4edda;color:#155724;border:1px solid #c3e6cb;}"
    ".alert-warning{background:#fff3cd;color:#856404;border:1px solid #ffeeba;}"
    ".alert-danger{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;}"
    ".badge{display:inline-block;padding:3px 8px;border-radius:12px;font-size:12px;font-weight:600;}"
    ".badge-success{background:#28a745;color:white;}"
    ".badge-danger{background:#dc3545;color:white;}"
    ".form-group{margin-bottom:14px;}"
    ".form-group label{display:block;margin-bottom:4px;color:#495057;font-weight:500;}"
    ".footer{text-align:center;padding:20px;color:#6c757d;font-size:12px;}"
    "</style></head><body>"
    "<div id='header'><h1>Funny ESP32</h1><span class='version'>LuCI 风格管理后台</span></div>"
    "<div id='menubar'>"
    "<a href='/' class='active'>状态</a>"
    "<a href='/'>网络</a>"
    "<a href='/'>系统</a>"
    "</div>"
    "<div id='content'>";

static const char* HTML_FOOTER = "<div class='footer'>Funny ESP32 &copy; 2026 | Powered by ESP-IDF</div>"
    "</div></body></html>";

#define WEB_BUF_SIZE 8192

static bool ota_in_progress = false;
static char ota_status[128] = "";
static char ota_url[256] = "";

static esp_err_t root_handler(httpd_req_t *req)
{
    char *buf = malloc(WEB_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int len = 0;
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, "%s", HTML_HEADER);
    
    // WiFi Status Container
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
    
    // WiFi Config Container
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128268;</span>WiFi 配置</div><div class='body'>"
        "<form action='/wifi' method='post'>"
        "<div class='form-group'><label>SSID</label><input type='text' name='ssid' placeholder='输入WiFi名称' required></div>"
        "<div class='form-group'><label>密码</label><input type='password' name='password' placeholder='输入密码' required></div>"
        "<button type='submit' class='btn btn-primary'>连接</button>"
        "</form></div></div>");
    
    // AP Config Container
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128225;</span>热点配置</div><div class='body'>"
        "<table class='table'><tr><th>热点名称</th><td>Funny_ESP32</td></tr></table>"
        "<form action='/ap' method='post' style='margin-top:12px;'>"
        "<div class='form-group'><label>新密码</label><input type='password' name='password' placeholder='输入新密码' required></div>"
        "<button type='submit' class='btn btn-primary'>修改密码</button>"
        "</form></div></div>");
    
    // DeepSeek API Container
    const char* current_key = deepseek_get_api_key();
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#129302;</span>DeepSeek API</div><div class='body'>"
        "<form action='/apikey' method='post'>"
        "<div class='form-group'><label>API 密钥</label><input type='password' name='apikey' value='%s' placeholder='sk-...'></div>"
        "<button type='submit' class='btn btn-primary'>保存</button>"
        "</form></div></div>",
        current_key ? current_key : "");
    
    // MiMo Cookie Container
    const char* current_cookie = mimo_get_cookie();
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#127850;</span>MiMo Cookie</div><div class='body'>"
        "<form action='/mimo' method='post'>"
        "<div class='form-group'><label>Cookie</label><input type='password' name='cookie' value='%s' placeholder='粘贴Cookie'></div>"
        "<button type='submit' class='btn btn-primary'>保存</button>"
        "</form></div></div>",
        current_cookie ? current_cookie : "");
    
    // NTP Config Container
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
    
    // OTA Update Container
    const char* latest_ver = wifi_bsp_get_latest_version();
    len += snprintf(buf + len, WEB_BUF_SIZE - len, 
        "<div class='container'><div class='header'><span class='icon'>&#128230;</span>固件更新</div><div class='body'>"
        "<table class='table'>"
        "<tr><th>当前版本</th><td>%s</td></tr>"
        "<tr><th>最新版本</th><td>%s</td></tr>"
        "</table>"
        "<form action='/ota' method='post' style='margin-top:12px;'>"
        "<button type='submit' class='btn btn-primary'%s>检查并更新</button>"
        "</form>"
        "%s</div></div>",
        FIRMWARE_VERSION,
        latest_ver,
        ota_in_progress ? " disabled" : "",
        ota_status);
    
    len += snprintf(buf + len, WEB_BUF_SIZE - len, "%s", HTML_FOOTER);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

static esp_err_t wifi_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, WEB_BUF_SIZE - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char ssid[33] = {0};
    char password[65] = {0};
    
    // Parse form data
    char *ssid_start = strstr(buf, "ssid=");
    char *pwd_start = strstr(buf, "password=");
    
    if (ssid_start && pwd_start) {
        ssid_start += 5; // skip "ssid="
        pwd_start += 9;  // skip "password="
        
        // Find end of ssid (before &)
        char *ssid_end = strchr(ssid_start, '&');
        if (ssid_end) {
            int ssid_len = ssid_end - ssid_start;
            if (ssid_len > 32) ssid_len = 32;
            strncpy(ssid, ssid_start, ssid_len);
        }
        
        // Copy password
        strncpy(password, pwd_start, 64);
        
        // URL decode (simple version - replace + with space)
        for (int i = 0; ssid[i]; i++) {
            if (ssid[i] == '+') ssid[i] = ' ';
        }
        for (int i = 0; password[i]; i++) {
            if (password[i] == '+') password[i] = ' ';
        }
        
        ESP_LOGI(TAG, "New WiFi config: SSID=%s", ssid);
        wifi_bsp_connect(ssid, password);
    }
    
    // Redirect back to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ap_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, WEB_BUF_SIZE - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char password[65] = {0};
    
    // Parse form data
    char *pwd_start = strstr(buf, "password=");
    if (pwd_start) {
        pwd_start += 9;  // skip "password="
        strncpy(password, pwd_start, 64);
        
        // URL decode (simple version - replace + with space)
        for (int i = 0; password[i]; i++) {
            if (password[i] == '+') password[i] = ' ';
        }
        
        ESP_LOGI(TAG, "New AP password: %s", password);
        wifi_bsp_update_ap_password(password);
    }
    
    // Redirect back to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t apikey_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, WEB_BUF_SIZE - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char apikey[65] = {0};
    
    // Parse form data
    char *apikey_start = strstr(buf, "apikey=");
    if (apikey_start) {
        apikey_start += 7;  // skip "apikey="
        strncpy(apikey, apikey_start, 64);
        
        // URL decode (simple version - replace + with space)
        for (int i = 0; apikey[i]; i++) {
            if (apikey[i] == '+') apikey[i] = ' ';
        }
        
        ESP_LOGI(TAG, "New API key configured");
        deepseek_set_api_key(apikey);
    }
    
    // Redirect back to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ntp_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, WEB_BUF_SIZE - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char server[64] = {0};
    char timezone[32] = {0};
    
    // Parse form data
    char *server_start = strstr(buf, "server=");
    char *tz_start = strstr(buf, "timezone=");
    
    if (server_start && tz_start) {
        server_start += 7;  // skip "server="
        tz_start += 9;      // skip "timezone="
        
        // Find end of server (before &)
        char *server_end = strchr(server_start, '&');
        if (server_end) {
            int len = server_end - server_start;
            if (len > 63) len = 63;
            strncpy(server, server_start, len);
        }
        
        // Copy timezone
        strncpy(timezone, tz_start, 31);
        
        // URL decode
        url_decode(server);
        url_decode_tz(timezone);  // 时区保留+号
        
        ESP_LOGI(TAG, "NTP config: server=%s, tz=%s", server, timezone);
        ntp_set_server(server);
        ntp_set_timezone(timezone);
    }
    
    // Redirect back to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t mimo_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, WEB_BUF_SIZE - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char cookie[256] = {0};
    
    // Parse form data
    char *cookie_start = strstr(buf, "cookie=");
    if (cookie_start) {
        cookie_start += 7;
        strncpy(cookie, cookie_start, 255);
        
        // URL decode (simple version - replace + with space)
        for (int i = 0; cookie[i]; i++) {
            if (cookie[i] == '+') cookie[i] = ' ';
        }
        
        ESP_LOGI(TAG, "MiMo cookie updated");
        mimo_set_cookie(cookie);
    }
    
    // Redirect back to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t sync_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Manual NTP sync requested");
    ntp_sync_now();
    
    // Redirect back to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void ota_task(void *arg)
{
    char *url = (char*)arg;
    
    ESP_LOGI(TAG, "OTA starting from: %s", url);
    snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-success'>正在更新...</div>");
    
    extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
    extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 30000;
    config.keep_alive_enable = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &config;
    
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    
    if (err != ESP_OK) {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>开始更新失败: %s</div>", esp_err_to_name(err));
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            continue;
        } else if (err == ESP_OK) {
            break;
        } else {
            snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>更新失败: %s</div>", esp_err_to_name(err));
            ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
            esp_https_ota_abort(ota_handle);
            ota_in_progress = false;
            vTaskDelete(NULL);
            return;
        }
    }
    
    if (esp_https_ota_is_complete_data_received(ota_handle)) {
        err = esp_https_ota_finish(ota_handle);
        if (err == ESP_OK) {
            snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-success'>更新成功，重启中...</div>");
            ESP_LOGI(TAG, "OTA success, restarting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else {
            snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>完成更新失败: %s</div>", esp_err_to_name(err));
        }
    } else {
        snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-danger'>数据接收不完整</div>");
        esp_https_ota_abort(ota_handle);
    }
    
    ota_in_progress = false;
    vTaskDelete(NULL);
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (ota_in_progress) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // 构建OTA URL (使用HTTP避免证书问题)
    snprintf(ota_url, sizeof(ota_url), 
        "http://luoyun.eu.org/firmware/FunnyEsp32.bin");
    
    ota_in_progress = true;
    snprintf(ota_status, sizeof(ota_status), "<div class='alert alert-success'>开始更新...</div>");
    
    xTaskCreate(ota_task, "ota_task", 8192, ota_url, 5, NULL);
    
    // Redirect back to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void web_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler
        };
        httpd_uri_t wifi = {
            .uri = "/wifi",
            .method = HTTP_POST,
            .handler = wifi_handler
        };
        httpd_uri_t ap = {
            .uri = "/ap",
            .method = HTTP_POST,
            .handler = ap_handler
        };
        httpd_uri_t apikey = {
            .uri = "/apikey",
            .method = HTTP_POST,
            .handler = apikey_handler
        };
        httpd_uri_t ntp = {
            .uri = "/ntp",
            .method = HTTP_POST,
            .handler = ntp_handler
        };
        httpd_uri_t mimo = {
            .uri = "/mimo",
            .method = HTTP_POST,
            .handler = mimo_handler
        };
        httpd_uri_t sync = {
            .uri = "/sync",
            .method = HTTP_POST,
            .handler = sync_handler
        };
        httpd_uri_t ota = {
            .uri = "/ota",
            .method = HTTP_POST,
            .handler = ota_handler
        };
        
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &wifi);
        httpd_register_uri_handler(server, &ap);
        httpd_register_uri_handler(server, &apikey);
        httpd_register_uri_handler(server, &ntp);
        httpd_register_uri_handler(server, &mimo);
        httpd_register_uri_handler(server, &sync);
        httpd_register_uri_handler(server, &ota);
        ESP_LOGI(TAG, "Web server started on port 80");
    }
}