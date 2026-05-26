idf_component_register(
    SRCS "main.c" "wifi_sta.c" "http_ep.c"
    INCLUDE_DIRS "."
    REQUIRES microlink esp_wifi nvs_flash esp_netif esp_event esp_http_server json lwip
)