#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void web_server_init(void);
void deepseek_set_api_key(const char* key);
const char* deepseek_get_api_key(void);

#ifdef __cplusplus
}
#endif