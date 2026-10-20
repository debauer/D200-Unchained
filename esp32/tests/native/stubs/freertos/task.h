#pragma once
#include "FreeRTOS.h"
typedef int BaseType_t;
#define pdPASS 1
BaseType_t xTaskCreate(void (*function)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle);
