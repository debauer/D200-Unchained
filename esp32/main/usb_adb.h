#pragma once

#include <stdbool.h>

/* USB host side: finds the D200 (ADB interface), connects with ADB and runs a test command. */
void usb_adb_start(void);

/* True while a device with an ADB interface is attached. */
bool usb_adb_device_present(void);
