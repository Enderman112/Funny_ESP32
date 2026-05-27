#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void web_server_init(void);
void deepseek_set_api_key(const char* key);
const char* deepseek_get_api_key(void);
void ntp_set_server(const char* server);
const char* ntp_get_server(void);
void ntp_set_timezone(const char* tz);
const char* ntp_get_timezone(void);
void mimo_set_cookie(const char* cookie);
const char* mimo_get_cookie(void);

#ifdef __cplusplus
}
#endif