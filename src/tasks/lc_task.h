#ifndef TASKS_LC_TASK_H
#define TASKS_LC_TASK_H

#include <stdbool.h>
#include <stdint.h>

// Owns the load cell. Starts the ADS1220 each time the HAT gets power, reads
// every sample (1000 a second) and publishes a 10 ms average.

typedef struct {
    bool present;       // ADS1220 started, and this block read
    int32_t code;       // average ADC code over the block
    float mv_per_v;     // bridge output per volt of excitation
    bool calibrated;    // newtons only means something once lc_calibrate has run
    float newtons;
} lc_reading_t;

// Starts the ADS1220 when the HAT gets power and reads it on every DRDY.
void vLcTask(void *pvParameters);

// Copies the newest reading.
void lc_latest(lc_reading_t *reading);

// Makes the current load zero: averages one second of readings, so it blocks
// the caller for a second. False if the load cell isn't reading.
bool lc_tare(void);

// With a known load on the cell (newtons, after lc_tare at no load), sets the
// scale from one second of readings. False if the load cell isn't reading.
bool lc_calibrate(float newtons);

#endif /* TASKS_LC_TASK_H */
