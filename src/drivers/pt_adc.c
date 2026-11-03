/* ADC1 driver for the PT board. See pt_adc.h for the signal path. */
#include "drivers/pt_adc.h"

/* The injected sequence: rank 1 converts first and lands in JDR1, rank 2
 * second into JDR2. */
#define PT_ADC_DATA_CHANNEL    ADC_CHANNEL_15   /* A0 = PA3 */
#define PT_ADC_DATA_RANK       ADC_INJECTED_RANK_1
#define PT_ADC_SUPPLY_CHANNEL  ADC_CHANNEL_10   /* A1 = PC0 */
#define PT_ADC_SUPPLY_RANK     ADC_INJECTED_RANK_2

/* One sequence takes ~3.2 ms; anything past 10 ms means the ADC is stuck. */
#define PT_ADC_TIMEOUT_MS      10U

static ADC_HandleTypeDef hadc1;

/* Adds one channel to the injected sequence. Everything except the channel
 * and rank is the same for both; the HAL collects the ranks and writes the
 * whole sequence (JSQR) on the last call. Must run while the ADC is disabled,
 * because single-ended mode can't be changed once it is on. */
static void add_injected_channel(uint32_t channel, uint32_t rank)
{
    ADC_InjectionConfTypeDef injected_config = {0};
    injected_config.InjectedChannel               = channel;
    injected_config.InjectedRank                  = rank;
    injected_config.InjectedNbrOfConversion       = 2;
    injected_config.InjectedSamplingTime          = ADC_SAMPLETIME_387CYCLES_5;
    injected_config.InjectedSingleDiff            = ADC_SINGLE_ENDED;
    injected_config.InjectedOffsetNumber          = ADC_OFFSET_NONE;
    injected_config.InjectedDiscontinuousConvMode = DISABLE;
    injected_config.AutoInjectedConv              = DISABLE;
    injected_config.QueueInjectedContext          = DISABLE;
    injected_config.ExternalTrigInjecConv         = ADC_INJECTED_SOFTWARE_START;
    injected_config.ExternalTrigInjecConvEdge     = ADC_EXTERNALTRIGINJECCONV_EDGE_NONE;
    injected_config.InjecOversamplingMode         = ENABLE;
    injected_config.InjecOversampling.Ratio         = 32;
    injected_config.InjecOversampling.RightBitShift = ADC_RIGHTBITSHIFT_5;
    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &injected_config) != HAL_OK) {
        Error_Handler();
    }
}

void pt_adc_init(void)
{
    /* ADC kernel clock: CLKP <- HSI (64 MHz). HSI stays on after reset and is
     * independent of the PLL1 setup in system_clock.c, so there is no need to
     * bring up PLL2 just for the ADC. */
    RCC_PeriphCLKInitTypeDef clock_config = {0};
    clock_config.PeriphClockSelection = RCC_PERIPHCLK_ADC | RCC_PERIPHCLK_CKPER;
    clock_config.CkperClockSelection  = RCC_CLKPSOURCE_HSI;
    clock_config.AdcClockSelection    = RCC_ADCCLKSOURCE_CLKP;
    if (HAL_RCCEx_PeriphCLKConfig(&clock_config) != HAL_OK) {
        Error_Handler();
    }
    __HAL_RCC_ADC12_CLK_ENABLE();

    /* GPIOA/GPIOC clocks are already on: MX_GPIO_Init() enables ports A-D and H. */
    GPIO_InitTypeDef pin_config = {0};
    pin_config.Mode = GPIO_MODE_ANALOG;
    pin_config.Pull = GPIO_NOPULL;
    pin_config.Pin  = GPIO_PIN_3;
    HAL_GPIO_Init(GPIOA, &pin_config);
    pin_config.Pin  = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOC, &pin_config);

    /* Settings shared by both groups. We only use the injected group; the
     * regular group is left as an unused one-channel, software-triggered
     * sequence.
     *
     * 64 MHz / 4 = 16 MHz in, which the ADC halves internally (rev V).
     * ScanConvMode must be on for an injected sequence longer than one, and
     * EOC_SEQ_CONV makes the poll wait for the end of the whole sequence
     * (JEOS) rather than just its first conversion (JEOC). */
    hadc1.Instance                      = ADC1;
    hadc1.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV4;
    hadc1.Init.Resolution               = ADC_RESOLUTION_16B;
    hadc1.Init.ScanConvMode             = ADC_SCAN_ENABLE;
    hadc1.Init.EOCSelection             = ADC_EOC_SEQ_CONV;
    hadc1.Init.LowPowerAutoWait         = DISABLE;
    hadc1.Init.ContinuousConvMode       = DISABLE;
    hadc1.Init.NbrOfConversion          = 1;
    hadc1.Init.DiscontinuousConvMode    = DISABLE;
    hadc1.Init.ExternalTrigConv         = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    hadc1.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
    hadc1.Init.OversamplingMode         = DISABLE;   /* regular group only */
    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        Error_Handler();
    }

    /* The injected queue holds up to two pending sequences for hardware
     * triggers. With a software trigger the HAL requires it off. It already
     * is at reset (JQDIS = 1); say so explicitly rather than rely on it.
     * Done before writing the sequence so it can't disturb it. */
    if (HAL_ADCEx_DisableInjectedQueue(&hadc1) != HAL_OK) {
        Error_Handler();
    }

    /* The injected sequence: A0 then A1, 16 bits, 387.5-cycle sampling, 32x
     * oversampled and shifted back down to 16 bits. ~1.6 ms per channel. */
    add_injected_channel(PT_ADC_DATA_CHANNEL, PT_ADC_DATA_RANK);
    add_injected_channel(PT_ADC_SUPPLY_CHANNEL, PT_ADC_SUPPLY_RANK);

    /* Offset + linearity calibration; must run with the ADC disabled. */
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET_LINEARITY,
                                    ADC_SINGLE_ENDED) != HAL_OK) {
        Error_Handler();
    }

    /* The ADC is still disabled here. The first HAL_ADCEx_InjectedStart()
     * turns it on (ADEN, then waits for ADRDY); nothing ever turns it off
     * again, so every later start skips straight to the trigger. */
}

HAL_StatusTypeDef pt_adc_read(pt_adc_sample_t *sample)
{
    /* Sets JADSTART: the ADC converts rank 1, then rank 2, then clears
     * JADSTART itself and raises JEOS. */
    HAL_StatusTypeDef status = HAL_ADCEx_InjectedStart(&hadc1);
    if (status != HAL_OK) {
        return status;
    }

    status = HAL_ADCEx_InjectedPollForConversion(&hadc1, PT_ADC_TIMEOUT_MS);
    if (status != HAL_OK) {
        /* Abort the stuck sequence so the next read starts clean. This also
         * disables the ADC; the next start turns it back on. */
        HAL_ADCEx_InjectedStop(&hadc1);
        return status;
    }

    /* Each rank has its own result register, so nothing can overwrite A0
     * before it is read. 32x oversampling with a shift of 5 keeps the result
     * at 16 bits; keep ratio == 2^shift if you change either. */
    sample->data_counts   = (uint16_t)HAL_ADCEx_InjectedGetValue(&hadc1, PT_ADC_DATA_RANK);
    sample->supply_counts = (uint16_t)HAL_ADCEx_InjectedGetValue(&hadc1, PT_ADC_SUPPLY_RANK);
    return HAL_OK;
}
