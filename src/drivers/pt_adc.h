#ifndef DRIVERS_PT_ADC_H
#define DRIVERS_PT_ADC_H

#include <stdint.h>

// The HAT's two PTs. TIM15 triggers ADC1 and ADC2 together 1000 times a second:
// ADC1 reads each PT's supply and ADC2 its output at the same instant. DMA writes
// the results into a buffer of two halves, 10 ms each, round and round.

#define PT_COUNT          2      // PT1 ox tank, PT2 chamber
#define PT_SAMPLE_HZ      1000U
#define PT_BLOCK_SAMPLES  10     // samples in each half of the buffer

// One PT at one instant, both after the board's 15/25 divider. The order matches
// the dual-mode data register (ADC1 in the low 16 bits, ADC2 in the high 16), so
// the DMA writes these as they are.
typedef struct {
    uint16_t supply_counts;   // ADC1
    uint16_t data_counts;     // ADC2
} pt_adc_reading_t;

// One trigger: both PTs.
typedef struct {
    pt_adc_reading_t pt[PT_COUNT];
} pt_adc_sample_t;

// Sets up the pins, ADC1 + ADC2, the DMA and TIM15, and calibrates both ADCs.
// Nothing samples yet.
void pt_adc_init(void);

// Starts sampling. From then on HAL_ADC_ConvHalfCpltCallback runs when the first
// half of the buffer is full and HAL_ADC_ConvCpltCallback when the second is.
void pt_adc_start(void);

// The PT_BLOCK_SAMPLES samples in one half (0 or 1). The DMA fills the other half
// meanwhile, so a half stays untouched for 10 ms after its callback.
const pt_adc_sample_t *pt_adc_block(unsigned half);

#endif /* DRIVERS_PT_ADC_H */
