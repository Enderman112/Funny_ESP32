idf_component_register(SRCS "main.cpp" "lv_font_MiSansLight_16.c" "lv_font_MiSansLight_28.c"
                    INCLUDE_DIRS "."
                    REQUIRES display_bsp lvgl_bsp button_bsp wifi_bsp web_server nvs_flash mbedtls esp-tls adc_bsp)

if(DEFINED ENV{FIRMWARE_VERSION})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FIRMWARE_VERSION="$ENV{FIRMWARE_VERSION}")
endif()
