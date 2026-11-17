#ifndef HOST_TASK_H
#define HOST_TASK_H
#include "FreeRTOS.h"
portTickType xTaskGetTickCount(void);
void vTaskDelay(portTickType ticks);
#endif
