#pragma once

#include "esp_err.h"

/* Joins the configured WiFi (station mode) and blocks until an address is assigned or it fails. */
esp_err_t wifi_start(void);
