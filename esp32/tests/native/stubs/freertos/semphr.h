#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t sem, unsigned ticks);
int xSemaphoreGive(SemaphoreHandle_t sem);
