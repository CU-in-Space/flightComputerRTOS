#ifndef DRIVERS_PT_ADC_H
#define DRIVERS_PT_ADC_H

#include <stdint.h>

#include "main.h"

/* ADC front end of the PT (pressure transducer) board.
 *
 * The board buffers two signals through identical 10k/15k dividers and
 * OPA2333 followers, so both reach the MCU scaled by 15/25:
 *
 *   SENS_DATA_IN  (transducer output)  -> A0 = PA3 = ADC1_INP15
 *   SENS_PWR_IN   (transducer supply)  -> A1 = PC0 = ADC1_INP10
 *
 * Both are read by ADC1 as a two-channel injected sequence, 16 bits, 32x
 * oversampled. Plain HAL, no FreeRTOS.
 */

typedef struct {
    /* Raw ADC readings, 0-65535 counts, both after the 15/25 divider. */
    uint16_t data_counts;    /* A0: transducer output */
    uint16_t supply_counts;  /* A1: transducer supply */
} pt_adc_sample_t;

/* Sets up the ADC clock, the two analog pins, ADC1 and its injected
 * sequence, and calibrates it. All configuration happens here.
 * Calls Error_Handler() on failure. Call once, from the task that owns the
 * ADC, after MX_GPIO_Init() has turned on the GPIO port clocks.
 *
 * Not from main(): SysTick is already running FreeRTOS's tick handler from
 * HAL_Init() on, and a tick that lands before the scheduler exists faults in
 * xTaskIncrementTick(). Calibration alone outlasts a 1 ms tick. */
void pt_adc_init(void);

/* Triggers the injected sequence (A0 then A1) and waits for it. Only
 * triggers and reads; never reconfigures. Blocks for about 3.2 ms. */
HAL_StatusTypeDef pt_adc_read(pt_adc_sample_t *sample);

#endif /* DRIVERS_PT_ADC_H */
