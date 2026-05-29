idf_component_register(
    SRCS 
    "web_server.c"
    REQUIRES 
    esp_http_server
    esp_https_ota
    bootloader_support
    app_update
    mbedtls
    wifi_bsp
    INCLUDE_DIRS 
    ".")

if(DEFINED ENV{FIRMWARE_VERSION})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FIRMWARE_VERSION="$ENV{FIRMWARE_VERSION}")
endif()