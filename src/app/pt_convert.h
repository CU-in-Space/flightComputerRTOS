#ifndef APP_PT_CONVERT_H
#define APP_PT_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

/* Pressure from the PT board's two ADC readings. Pure logic: no HAL, no
 * FreeRTOS, so it also compiles and tests on a computer.
 *
 * Ratiometric correction: a ratiometric transducer's output is a fixed
 * fraction of its supply, so pressure follows Vout / Vsupply rather than Vout
 * alone. The board scales both by the same 15/25 divider and the ADC reads
 * both against the same VREF+, so both cancel and
 *
 *     ratio = data_counts / supply_counts = Vout / Vsupply
 *
 * Any drift in the 5 V LDO or in VREF+ drops out with them. */

typedef struct {
    float ratio_at_zero;   /* Vout / Vsupply at zero pressure (0.10 for 0.5 V of 5 V) */
    float ratio_at_full;   /* Vout / Vsupply at full scale    (0.90 for 4.5 V of 5 V) */
    float full_scale;      /* pressure at full scale, in whatever unit you want out */
} pt_calibration_t;

/* Vout / Vsupply. Returns false (and leaves *ratio alone) if the supply
 * reading is too low to divide by meaningfully, e.g. sensor unpowered. */
bool pt_ratio(uint16_t data_counts, uint16_t supply_counts, float *ratio);

/* Linear map from ratio to pressure. Not clamped: a slightly negative value
 * at zero pressure is real offset error. */
float pt_pressure(const pt_calibration_t *calibration, float ratio);

/* Voltage at the board's connector (before the divider), in millivolts,
 * for a 16-bit ADC reading (in counts) against vref_mv. Diagnostic only: unlike the
 * ratio, this depends on VREF+ being what you say it is. */
uint32_t pt_counts_to_connector_mv(uint16_t counts, uint32_t vref_mv);

#endif /* APP_PT_CONVERT_H */
