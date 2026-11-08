#ifndef TASKS_PT_TASK_H
#define TASKS_PT_TASK_H

#include <stdbool.h>
#include <stdint.h>

// Owns the PTs. Every 10 ms it averages the newest block of samples into one
// reading per PT and publishes it.

typedef struct {
    uint32_t supply_mv;   // at the connector, averaged over the block
    uint32_t data_mv;
    bool powered;         // false: supply too low, so ratio and psi mean nothing
    float ratio;          // output / supply
    float psi;
} pt_reading_t;

// Waits for each block from pt_adc and converts it. Starts the sampling itself.
void vPtTask(void *pvParameters);

// Copies the newest reading of one PT (0 = PT1, 1 = PT2).
void pt_latest(unsigned pt, pt_reading_t *reading);

#endif /* TASKS_PT_TASK_H */
