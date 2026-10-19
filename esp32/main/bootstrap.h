#pragma once

#include "adb.h"
#include "esp_err.h"

/* Port deckd listens on, on the device. */
#define DECKD_PORT 7200

/* Stops the vendor app and makes sure the embedded deckd runs on the device (pushing it if the
 * installed file is missing or differs in size). Port of host/src/d200_deck/service/bootstrap.py. */
esp_err_t bootstrap_deckd(adb_t *a);
