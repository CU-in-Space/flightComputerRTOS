#ifndef TASKS_LC_TASK_H
#define TASKS_LC_TASK_H

#include <stdbool.h>
#include <stdint.h>

// Owns the two load cells, which share the HAT's one ADS1220. Starts the chip
// each time the HAT gets power and reads every sample.
// LC1 (connector A) is the fast one: nearly every sample, about 978 a second,
// published as a 10 ms average. LC2 (connector B) is for a weight that changes
// slowly: one sample about 10 times a second.

#define LC_COUNT  2

typedef struct {
    bool present;       // ADS1220 started, and this cell read
    int32_t code;       // LC1: average ADC code over the block. LC2: its one sample
    float mv_per_v;     // bridge output per volt of excitation
    bool calibrated;    // newtons only means something once lc_calibrate has run
    float newtons;
} lc_reading_t;

// Zero and scale, set by lc_tare and lc_calibrate.
typedef struct {
    int32_t zero_code;
    float newtons_per_code;   // 0 until calibrated
} lc_calibration_t;

// Starts the ADS1220 when the HAT gets power and reads it on every DRDY.
void vLcTask(void *pvParameters);

// Copies a cell's newest reading. cell is 0 for LC1, 1 for LC2.
void lc_latest(unsigned cell, lc_reading_t *reading);

// Copies a cell's current zero and scale, for the data log.
void lc_calibration(unsigned cell, lc_calibration_t *calibration);

// Makes a cell's current load zero: averages one second of readings, so it
// blocks the caller for a second. False if the cell isn't reading.
bool lc_tare(unsigned cell);

// With a known load on the cell (newtons, after lc_tare at no load), sets the
// scale from one second of readings. False if the cell isn't reading.
bool lc_calibrate(unsigned cell, float newtons);

#endif /* TASKS_LC_TASK_H */
