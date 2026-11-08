#ifndef TASKS_TC_TASK_H
#define TASKS_TC_TASK_H

#include <stdbool.h>

#include "drivers/max31856.h"

// Owns the thermocouples. Sets the four MAX31856 up each time the HAT gets power
// (the interlock turns ENABLED) and reads them every 100 ms.

#define TC_COUNT  4

typedef struct {
    bool present;              // set up, and this read worked
    max31856_reading_t value;  // only meaningful when present
} tc_reading_t;

// Sets up and reads the four MAX31856.
void vTcTask(void *pvParameters);

// Copies the newest reading of one thermocouple (0 = TC1).
void tc_latest(unsigned tc, tc_reading_t *reading);

#endif /* TASKS_TC_TASK_H */
