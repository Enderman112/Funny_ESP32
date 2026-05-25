idf_component_register(
    SRCS 
    "web_server.c"
    REQUIRES 
    esp_http_server
    wifi_bsp
    INCLUDE_DIRS 
    ".")