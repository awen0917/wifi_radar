idf_component_register(SRCS "wifi_radar_main.c"
                       EMBED_TXTFILES "dashboard.html" "config.html"
                       PRIV_REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash driver json esp_timer
                       INCLUDE_DIRS "")
