idf_component_register(
    SRCS main.c wifi.c web.c web_guard.c usb_adb.c adb.c adb_sync.c bootstrap.c deckd.c device.c device_json.c events.c config.c config_validate.c config_fields.c actions.c draw.c keyfiles.c logbuf.c blobs.S wallclock.c placeholders.c render.c glyphs.c drawdyn.c mqtt.c mqttjson.c mqttsettings.c rules.c rle.c
    INCLUDE_DIRS .
    REQUIRES lwip nvs_flash esp_wifi esp_event esp_netif esp_http_server usb littlefs json mqtt esp-tls mbedtls
)
