#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "wifi_bsp.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WebServer";

static const char* HTML_HEADER = "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>Funny ESP32 管理后台</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;}"
    ".card{background:white;padding:20px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
    "h1{color:#333;}"
    "input[type=text],input[type=password]{width:100%;padding:10px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;}"
    "input[type=submit]{background:#4CAF50;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;width:100%;}"
    "input[type=submit]:hover{background:#45a049;}"
    ".status{padding:10px;border-radius:4px;margin:10px 0;}"
    ".connected{background:#dff0d8;color:#3c763d;}"
    ".disconnected{background:#f2dede;color:#a94442;}"
    "</style></head><body>";

static const char* HTML_FOOTER = "</body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    char buf[2048];
    int len = 0;
    
    len += snprintf(buf + len, sizeof(buf) - len, "%s", HTML_HEADER);
    len += snprintf(buf + len, sizeof(buf) - len, "<h1>Funny ESP32 管理后台</h1>");
    
    // WiFi status
    len += snprintf(buf + len, sizeof(buf) - len, "<div class='card'><h2>WiFi 状态</h2>");
    if (wifi_bsp_is_connected()) {
        len += snprintf(buf + len, sizeof(buf) - len, 
            "<div class='status connected'>已连接</div>"
            "<p><strong>名称:</strong> %s</p>"
            "<p><strong>IP:</strong> %s</p>",
            wifi_bsp_get_ssid(), wifi_bsp_get_ip());
    } else {
        len += snprintf(buf + len, sizeof(buf) - len, 
            "<div class='status disconnected'>未连接</div>"
            "<p><strong>名称:</strong> %s</p>",
            wifi_bsp_get_ssid());
    }
    len += snprintf(buf + len, sizeof(buf) - len, "</div>");
    
    // WiFi config form
    len += snprintf(buf + len, sizeof(buf) - len, 
        "<div class='card'><h2>更换 WiFi</h2>"
        "<form action='/wifi' method='post'>"
        "<label>名称:</label>"
        "<input type='text' name='ssid' required>"
        "<label>密码:</label>"
        "<input type='password' name='password' required>"
        "<input type='submit' value='连接'>"
        "</form></div>");
    
    len += snprintf(buf + len, sizeof(buf) - len, "%s", HTML_FOOTER);
    
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

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
        
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &wifi);
        ESP_LOGI(TAG, "Web server started on port 80");
    }
}