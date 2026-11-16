#include "drivers/vbat_adc.h"

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "board/board_pins.h"

#define VREF_MV              3300U
// Power board R1 and R3. Trim against a meter.
#define DIVIDER_TOP_KOHM     100U
#define DIVIDER_BOTTOM_KOHM  22U

static ADC_HandleTypeDef hadc3;

// Written by the ADC interrupt. "Lowest" and "highest" run from one
// vbat_adc_take_lows to the next. A lower VDDA reads as more VREFINT counts.
static volatile uint16_t vbat_counts;
static volatile uint16_t vbat_lowest = UINT16_MAX;
static volatile uint32_t vbat_conversions;
static volatile uint16_t vrefint_counts;
static volatile uint16_t vrefint_highest;

static uint32_t vbat_mv(uint16_t counts)
{
    uint32_t pin_mv = counts * VREF_MV / 65535U;
    return pin_mv * (DIVIDER_TOP_KOHM + DIVIDER_BOTTOM_KOHM) / DIVIDER_BOTTOM_KOHM;
}

void vbat_adc_init(void)
{
    __HAL_RCC_ADC3_CLK_ENABLE();

    GPIO_InitTypeDef pin_config = {0};
    pin_config.Pin  = VBAT_SENSE_Pin;
    pin_config.Mode = GPIO_MODE_ANALOG;
    pin_config.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(VBAT_SENSE_GPIO_Port, &pin_config);

    // Converts VBAT, then VREFINT, over and over by itself, and interrupts after
    // each. Both take about 0.94 ms at the 8 MHz ADC clock, so each is read ~1000
    // times a second without the CPU waiting.
    hadc3.Instance                      = ADC3;
    hadc3.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV4;
    hadc3.Init.Resolution               = ADC_RESOLUTION_16B;
    hadc3.Init.ScanConvMode             = ADC_SCAN_ENABLE;
    hadc3.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    hadc3.Init.LowPowerAutoWait         = DISABLE;
    hadc3.Init.ContinuousConvMode       = ENABLE;
    hadc3.Init.NbrOfConversion          = 2;
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
    // The STM32's own 1.2 V reference, which gives VDDA (the 3.3 V rail). It needs
    // at least 4.3 µs of sampling: 387.5 cycles is 48 µs.
    channel_config.Channel      = ADC_CHANNEL_VREFINT;
    channel_config.Rank         = ADC_REGULAR_RANK_2;
    channel_config.SamplingTime = ADC_SAMPLETIME_387CYCLES_5;
    if (HAL_ADC_ConfigChannel(&hadc3, &channel_config) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_ADCEx_Calibration_Start(&hadc3, ADC_CALIB_OFFSET_LINEARITY,
                                    ADC_SINGLE_ENDED) != HAL_OK) {
        Error_Handler();
    }

    // Priority 6, below the FreeRTOS line, like the other interrupts.
    HAL_NVIC_SetPriority(ADC3_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(ADC3_IRQn);
    __HAL_ADC_ENABLE_IT(&hadc3, ADC_IT_EOC);
    if (HAL_ADC_Start(&hadc3) != HAL_OK) {
        Error_Handler();
    }
}

void ADC3_IRQHandler(void)
{
    uint32_t flags = ADC3->ISR;
    uint16_t counts = (uint16_t)ADC3->DR;   // reading it clears EOC

    // A conversion finished before the last one was read, so which channel DR
    // holds isn't certain. Skip it; the end of the sequence puts it right.
    if (flags & ADC_ISR_OVR) {
        ADC3->ISR = ADC_ISR_OVR | ADC_ISR_EOS;
        return;
    }
    if (flags & ADC_ISR_EOS) {
        // The end of the sequence: VREFINT.
        ADC3->ISR = ADC_ISR_EOS;
        vrefint_counts = counts;
        if (counts > vrefint_highest) {
            vrefint_highest = counts;
        }
    } else {
        vbat_counts = counts;
        if (counts < vbat_lowest) {
            vbat_lowest = counts;
        }
        vbat_conversions++;
    }
}

uint32_t vbat_adc_read_mv(void)
{
    static uint32_t conversions_seen;

    // The first reading comes about 1 ms after the scheduler starts.
    while (vbat_conversions == 0) {
        vTaskDelay(1);
    }
    // Nothing new since the last call: the ADC has stopped.
    if (vbat_conversions == conversions_seen) {
        return 0;
    }
    conversions_seen = vbat_conversions;
    return vbat_mv(vbat_counts);
}

void vbat_adc_take_lows(uint32_t *vbat_min_mv, uint32_t *vdda_min_mv)
{
    taskENTER_CRITICAL();
    uint16_t vbat = vbat_lowest == UINT16_MAX ? vbat_counts : vbat_lowest;
    uint16_t vrefint = vrefint_highest == 0 ? vrefint_counts : vrefint_highest;
    vbat_lowest = UINT16_MAX;
    vrefint_highest = 0;
    taskEXIT_CRITICAL();

    *vbat_min_mv = vbat_mv(vbat);
    *vdda_min_mv = vrefint == 0 ? 0 : __HAL_ADC_CALC_VREFANALOG_VOLTAGE(vrefint, ADC_RESOLUTION_16B);
}
