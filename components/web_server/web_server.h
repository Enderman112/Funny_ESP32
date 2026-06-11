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
void weather_set_provider(int provider);
int weather_get_provider(void);
void weather_set_qweather_key(const char* key);
const char* weather_get_qweather_key(void);
void weather_set_qweather_apihost(const char* host);
const char* weather_get_qweather_apihost(void);
void weather_set_openweather_key(const char* key);
const char* weather_get_openweather_key(void);
void weather_set_location(const char* loc);
const char* weather_get_location(void);

#ifdef __cplusplus
}
#endif