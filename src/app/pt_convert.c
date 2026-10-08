#include "app/pt_convert.h"

#define PT_ADC_FULL_SCALE  65535U

/* R2/R4 (and R5/R7) on the board: 10k over 15k, so the ADC sees 15/25. */
#define PT_DIVIDER_TOP_OHMS     10000U
#define PT_DIVIDER_BOTTOM_OHMS  15000U

/* Below ~1 V at the connector the sensor is unpowered or unplugged; the ratio
 * there is noise divided by noise. */
#define PT_MIN_SUPPLY_COUNTS  (PT_ADC_FULL_SCALE / 5U)

bool pt_ratio(uint16_t data_counts, uint16_t supply_counts, float *ratio)
{
    if (supply_counts < PT_MIN_SUPPLY_COUNTS) {
        return false;
    }
    *ratio = (float)data_counts / (float)supply_counts;
    return true;
}

float pt_pressure(const pt_calibration_t *calibration, float ratio)
{
    return (ratio - calibration->ratio_at_zero) / (calibration->ratio_at_full - calibration->ratio_at_zero)
           * calibration->full_scale;
}

uint32_t pt_counts_to_connector_mv(uint16_t counts, uint32_t vref_mv)
{
    /* 64-bit: 65535 * 3300 * 25000 overflows 32 bits. */
    uint64_t millivolts = (uint64_t)counts * vref_mv
                  * (PT_DIVIDER_TOP_OHMS + PT_DIVIDER_BOTTOM_OHMS);
    return (uint32_t)(millivolts / ((uint64_t)PT_ADC_FULL_SCALE * PT_DIVIDER_BOTTOM_OHMS));
}
