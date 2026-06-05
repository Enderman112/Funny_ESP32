#pragma once

#include <esp_adc/adc_oneshot.h>

#ifdef __cplusplus
extern "C" {
#endif

void adc_bsp_init(void);
float adc_bsp_get_battery_voltage(void);
uint8_t adc_bsp_get_battery_level(void);
bool adc_bsp_is_charging(void);

#ifdef __cplusplus
}
#endif