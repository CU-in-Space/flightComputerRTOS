#ifndef TASKS_SD_CARD_TASK_H
#define TASKS_SD_CARD_TASK_H

#include "FreeRTOS.h"
#include "task.h"

/* Owns the SD card: brings it up, then writes the flight log to it. Other
 * tasks never touch the card directly. */
void vSdCardTask(void *pvParameters);

#endif /* TASKS_SD_CARD_TASK_H */
