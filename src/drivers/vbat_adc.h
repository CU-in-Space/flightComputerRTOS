#ifndef DRIVERS_VBAT_ADC_H
#define DRIVERS_VBAT_ADC_H

#include <stdint.h>

// Battery voltage from the power board's VBAT_SENSE divider, on ADC3.

// Sets up PF4 and ADC3 and calibrates it.
void vbat_adc_init(void);

// Battery voltage in mV, or 0 if the conversion fails.
uint32_t vbat_adc_read_mv(void);

#endif /* DRIVERS_VBAT_ADC_H */
