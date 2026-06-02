#include "adc_bsp.h"
#include <esp_log.h>

static const char *TAG = "ADC_BSP";

static adc_cali_handle_t cali_handle;
static adc_oneshot_unit_handle_t adc1_handle;

void adc_bsp_init(void)
{
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.atten = ADC_ATTEN_DB_12;
    cali_config.bitwidth = ADC_BITWIDTH_12;
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));

    adc_oneshot_unit_init_cfg_t init_config1 = {};
    init_config1.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    
    adc_oneshot_chan_cfg_t config = {};
    config.bitwidth = ADC_BITWIDTH_12;
    config.atten = ADC_ATTEN_DB_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config));
    
    ESP_LOGI(TAG, "ADC initialized (Battery on ADC1_CH3)");
}

float adc_bsp_get_battery_voltage(void)
{
    int value;
    int voltage_mv = 0;
    float vol = 0;
    
    esp_err_t err = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &value);
    if (err == ESP_OK) {
        adc_cali_raw_to_voltage(cali_handle, value, &voltage_mv);
        vol = 0.001f * voltage_mv * 3;  // 3x voltage divider
        ESP_LOGI(TAG, "ADC raw=%d, mv=%d, voltage=%.2fV", value, voltage_mv, vol);
    }
    return vol;
}

uint8_t adc_bsp_get_battery_level(void)
{
    float vol = adc_bsp_get_battery_voltage();
    if (vol < 3.0f) {
        return 0;
    }
    if (vol > 4.12f) {
        return 100;
    }
    float level = ((vol - 3.0f) / 1.12f) * 100;
    return (uint8_t)level;
}