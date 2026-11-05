#ifndef DRIVERS_VBAT_ADC_H
#define DRIVERS_VBAT_ADC_H

#include <stdint.h>

// Battery voltage from the power board's VBAT_SENSE divider, on ADC3.

void vbat_adc_init(void);
uint32_t vbat_adc_read_mv(void);   // 0 if the conversion fails

#endif /* DRIVERS_VBAT_ADC_H */
