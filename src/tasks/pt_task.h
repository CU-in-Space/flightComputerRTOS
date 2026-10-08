#ifndef TASKS_PT_TASK_H
#define TASKS_PT_TASK_H

#include "FreeRTOS.h"
#include "task.h"

/* Bench test for the PT board: reads the transducer through
 * drivers/pt_adc.c, converts with app/pt_convert.c, and prints over the
 * ST-LINK virtual COM port. Initialises the ADC itself on startup. */
void vPtTask(void *pvParameters);

#endif /* TASKS_PT_TASK_H */
