idf_component_register(
    SRCS main.c wifi.c web.c web_config.c web_guard.c web_transport.c web_auth.c web_login.c web_oidc.c oidc_client.c oidc_http.c oidc_codec.c oidc_json.c oidc_tx.c oidc_jwt.c oidc_claims.c auth_core.c auth_password.c usb_adb.c adb.c adb_sync.c bootstrap.c deckd.c device.c device_json.c events.c config.c config_validate.c config_fields.c actions.c draw.c keyfiles.c logbuf.c blobs.S wallclock.c placeholders.c render.c glyphs.c drawdyn.c mqtt.c mqttjson.c mqttsettings.c rules.c rle.c
    INCLUDE_DIRS .
    REQUIRES lwip nvs_flash esp_wifi esp_event esp_netif esp_http_server esp_https_server esp_http_client usb littlefs json mqtt esp-tls mbedtls
)
