#include "drivers/vbat_adc.h"

#include "main.h"
#include "board/board_pins.h"

#define VREF_MV              3300U
// Power board R1 and R3. Trim against a meter.
#define DIVIDER_TOP_KOHM     100U
#define DIVIDER_BOTTOM_KOHM  22U
#define TIMEOUT_MS           5U   // one reading takes ~0.15 ms

static ADC_HandleTypeDef hadc3;

void vbat_adc_init(void)
{
    __HAL_RCC_ADC3_CLK_ENABLE();

    GPIO_InitTypeDef pin_config = {0};
    pin_config.Pin  = VBAT_SENSE_Pin;
    pin_config.Mode = GPIO_MODE_ANALOG;
    pin_config.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(VBAT_SENSE_GPIO_Port, &pin_config);

    hadc3.Instance                      = ADC3;
    hadc3.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV4;
    hadc3.Init.Resolution               = ADC_RESOLUTION_16B;
    hadc3.Init.ScanConvMode             = ADC_SCAN_DISABLE;
    hadc3.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    hadc3.Init.LowPowerAutoWait         = DISABLE;
    hadc3.Init.ContinuousConvMode       = DISABLE;
    hadc3.Init.NbrOfConversion          = 1;
    hadc3.Init.DiscontinuousConvMode    = DISABLE;
    hadc3.Init.ExternalTrigConv         = ADC_SOFTWARE_START;
    hadc3.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc3.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    hadc3.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    hadc3.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
    // 16 conversions summed, then shifted right by 4: their average.
    hadc3.Init.OversamplingMode                       = ENABLE;
    hadc3.Init.Oversampling.Ratio                     = 16;
    hadc3.Init.Oversampling.RightBitShift             = ADC_RIGHTBITSHIFT_4;
    hadc3.Init.Oversampling.TriggeredMode             = ADC_TRIGGEREDMODE_SINGLE_TRIGGER;
    hadc3.Init.Oversampling.OversamplingStopReset     = ADC_REGOVERSAMPLING_CONTINUED_MODE;
    if (HAL_ADC_Init(&hadc3) != HAL_OK) {
        Error_Handler();
    }

    ADC_ChannelConfTypeDef channel_config = {0};
    channel_config.Channel      = ADC_CHANNEL_9;
    channel_config.Rank         = ADC_REGULAR_RANK_1;
    channel_config.SamplingTime = ADC_SAMPLETIME_64CYCLES_5;
    channel_config.SingleDiff   = ADC_SINGLE_ENDED;
    channel_config.OffsetNumber = ADC_OFFSET_NONE;
    if (HAL_ADC_ConfigChannel(&hadc3, &channel_config) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_ADCEx_Calibration_Start(&hadc3, ADC_CALIB_OFFSET_LINEARITY,
                                    ADC_SINGLE_ENDED) != HAL_OK) {
        Error_Handler();
    }
}

uint32_t vbat_adc_read_mv(void)
{
    if (HAL_ADC_Start(&hadc3) != HAL_OK
        || HAL_ADC_PollForConversion(&hadc3, TIMEOUT_MS) != HAL_OK) {
        HAL_ADC_Stop(&hadc3);
        return 0;
    }
    uint32_t pin_mv = HAL_ADC_GetValue(&hadc3) * VREF_MV / 65535U;
    return pin_mv * (DIVIDER_TOP_KOHM + DIVIDER_BOTTOM_KOHM) / DIVIDER_BOTTOM_KOHM;
}
