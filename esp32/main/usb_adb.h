#pragma once

#include <stdbool.h>

/* USB host: finds the D200, bootstraps deckd over ADB and runs the device client, reconnecting on failure. */
void usb_adb_start(void);

/* True while a device with an ADB interface is attached. */
bool usb_adb_device_present(void);
