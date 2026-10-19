idf_component_register(
    SRCS main.c wifi.c web.c usb_adb.c adb.c adb_sync.c bootstrap.c deckd.c deckd_blob.S
    INCLUDE_DIRS .
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_server usb littlefs json
)
