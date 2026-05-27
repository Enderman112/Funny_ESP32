idf_component_register(SRCS "main.cpp" "lv_font_hei_16.c"
                    INCLUDE_DIRS "."
                    REQUIRES display_bsp lvgl_bsp button_bsp wifi_bsp web_server nvs_flash)

if(DEFINED ENV{FIRMWARE_VERSION})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FIRMWARE_VERSION="$ENV{FIRMWARE_VERSION}")
endif()
