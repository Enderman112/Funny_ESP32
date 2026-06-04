#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_bsp_init(void);
bool wifi_bsp_is_connected(void);
const char* wifi_bsp_get_ip(void);
const char* wifi_bsp_get_ssid(void);
void wifi_bsp_connect(const char* ssid, const char* password);
const char* wifi_bsp_get_latest_version(void);
void wifi_bsp_fetch_latest_version(void);

void wifi_bsp_start_ap(void);
void wifi_bsp_stop_ap(void);
bool wifi_bsp_is_ap_active(void);
const char* wifi_bsp_get_ap_ip(void);
void wifi_bsp_update_ap_password(const char* password);

#ifdef __cplusplus
}
#endif