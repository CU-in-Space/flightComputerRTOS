#ifndef DRIVERS_VBAT_ADC_H
#define DRIVERS_VBAT_ADC_H

#include <stdint.h>

// Battery voltage from the power board's VBAT_SENSE divider, and the STM32's own
// 3.3 V rail (VDDA) from its internal reference, on ADC3. Each is read about 1000
// times a second in the background.

// Sets up PF4 and ADC3, calibrates it and starts it converting.
void vbat_adc_init(void);

// The newest battery voltage in mV, at most ~1 ms old. 0 if there's been no new
// reading since the last call (the ADC has stopped). One caller only: the interlock.
uint32_t vbat_adc_read_mv(void);

// The lowest battery voltage and the lowest VDDA, in mV, since the last call.
void vbat_adc_take_lows(uint32_t *vbat_min_mv, uint32_t *vdda_min_mv);

#endif /* DRIVERS_VBAT_ADC_H */
