#include "drivers/pt_adc.h"

#include "main.h"
#include "board/board_pins.h"

#define TIMER_COUNT_HZ  1000000U   // TIM15 counts once a µs

static ADC_HandleTypeDef hadc1;
static ADC_HandleTypeDef hadc2;
static DMA_HandleTypeDef hdma_adc;
static TIM_HandleTypeDef htim15;

// Rank 1 is PT1 and rank 2 is PT2. Each rank converts the supply on ADC1 and the
// output on ADC2 at the same time.
static const uint32_t ranks[PT_COUNT] = { ADC_REGULAR_RANK_1, ADC_REGULAR_RANK_2 };
static const struct {
    uint32_t supply;   // ADC1
    uint32_t data;     // ADC2
} channels[PT_COUNT] = {
    { ADC_CHANNEL_5,  ADC_CHANNEL_13 },   // PT1: PB1, PC3_C
    { ADC_CHANNEL_10, ADC_CHANNEL_15 },   // PT2: PC0, PA3
};

// Two halves of PT_BLOCK_SAMPLES. The DMA moves whole 32-bit words, so the buffer
// has to start on a 4-byte boundary; the struct alone only guarantees 2.
static pt_adc_sample_t buffer[2 * PT_BLOCK_SAMPLES] __attribute__((aligned(4)));

void pt_adc_init(void)
{
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_TIM15_CLK_ENABLE();

    GPIO_InitTypeDef pin_config = {0};
    pin_config.Mode = GPIO_MODE_ANALOG;
    pin_config.Pull = GPIO_NOPULL;
    pin_config.Pin  = PT1_PWR_Pin;
    HAL_GPIO_Init(PT1_PWR_GPIO_Port, &pin_config);
    pin_config.Pin  = PT1_DATA_Pin;
    HAL_GPIO_Init(PT1_DATA_GPIO_Port, &pin_config);
    pin_config.Pin  = PT2_PWR_Pin;
    HAL_GPIO_Init(PT2_PWR_GPIO_Port, &pin_config);
    pin_config.Pin  = PT2_DATA_Pin;
    HAL_GPIO_Init(PT2_DATA_GPIO_Port, &pin_config);

    // PT1's output arrives on PC3_C, a pad that only reaches the ADC through
    // PC3 once this switch is closed.
    HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC3, SYSCFG_SWITCH_PC3_CLOSE);

    // One TIM15 update starts both ranks; the DMA collects the results.
    hadc1.Instance                      = ADC1;
    hadc1.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV4;
    hadc1.Init.Resolution               = ADC_RESOLUTION_16B;
    hadc1.Init.ScanConvMode             = ADC_SCAN_ENABLE;
    hadc1.Init.EOCSelection             = ADC_EOC_SEQ_CONV;
    hadc1.Init.LowPowerAutoWait         = DISABLE;
    hadc1.Init.ContinuousConvMode       = DISABLE;
    hadc1.Init.NbrOfConversion          = PT_COUNT;
    hadc1.Init.DiscontinuousConvMode    = DISABLE;
    hadc1.Init.ExternalTrigConv         = ADC_EXTERNALTRIG_T15_TRGO;
    hadc1.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
    hadc1.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
    // Each rank: 16 conversions averaged in hardware, all from the one trigger.
    hadc1.Init.OversamplingMode                   = ENABLE;
    hadc1.Init.Oversampling.Ratio                 = 16;
    hadc1.Init.Oversampling.RightBitShift         = ADC_RIGHTBITSHIFT_4;
    hadc1.Init.Oversampling.TriggeredMode         = ADC_TRIGGEREDMODE_SINGLE_TRIGGER;
    hadc1.Init.Oversampling.OversamplingStopReset = ADC_REGOVERSAMPLING_CONTINUED_MODE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        Error_Handler();
    }

    // ADC2 copies ADC1 but has no trigger of its own: in dual mode ADC1 starts it.
    hadc2.Instance              = ADC2;
    hadc2.Init                  = hadc1.Init;
    hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    if (HAL_ADC_Init(&hadc2) != HAL_OK) {
        Error_Handler();
    }

    // (64.5 + 8.5) cycles at 8 MHz is 9.1 µs; 16 of them is 146 µs a rank and
    // 0.3 ms a trigger, well inside the 1 ms between triggers. The 10 nF at each
    // pin charges the ADC's sampling cap, so the sample time can be short.
    ADC_ChannelConfTypeDef channel_config = {0};
    channel_config.SamplingTime = ADC_SAMPLETIME_64CYCLES_5;
    channel_config.SingleDiff   = ADC_SINGLE_ENDED;
    channel_config.OffsetNumber = ADC_OFFSET_NONE;
    for (unsigned i = 0; i < PT_COUNT; i++) {
        channel_config.Rank    = ranks[i];
        channel_config.Channel = channels[i].supply;
        if (HAL_ADC_ConfigChannel(&hadc1, &channel_config) != HAL_OK) {
            Error_Handler();
        }
        channel_config.Channel = channels[i].data;
        if (HAL_ADC_ConfigChannel(&hadc2, &channel_config) != HAL_OK) {
            Error_Handler();
        }
    }

    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK
        || HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK) {
        Error_Handler();
    }

    // Regular simultaneous: every conversion on ADC1 starts at the same instant as
    // its partner on ADC2. Both results land in one 32-bit register for the DMA.
    ADC_MultiModeTypeDef dual_config = {0};
    dual_config.Mode             = ADC_DUALMODE_REGSIMULT;
    dual_config.DualModeData     = ADC_DUALMODEDATAFORMAT_32_10_BITS;
    dual_config.TwoSamplingDelay = ADC_TWOSAMPLINGDELAY_1CYCLE;   // only used when interleaved
    if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &dual_config) != HAL_OK) {
        Error_Handler();
    }

    // One word per rank per trigger. Circular: after the last word it starts at the
    // top again, raising an interrupt at the middle and at the end.
    hdma_adc.Instance                 = DMA1_Stream0;
    hdma_adc.Init.Request             = DMA_REQUEST_ADC1;
    hdma_adc.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_adc.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_adc.Init.Mode                = DMA_CIRCULAR;
    hdma_adc.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adc.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adc) != HAL_OK) {
        Error_Handler();
    }
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc);

    // Priority 6, below the FreeRTOS line: the callbacks wake the PT task.
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);

    // APB2 timers run at twice PCLK2 (240 MHz), as for TIM1.
    uint32_t timer_clock = HAL_RCC_GetPCLK2Freq() * 2;

    htim15.Instance               = TIM15;
    htim15.Init.Prescaler         = timer_clock / TIMER_COUNT_HZ - 1;
    htim15.Init.Period            = TIMER_COUNT_HZ / PT_SAMPLE_HZ - 1;
    htim15.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim15.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim15.Init.RepetitionCounter = 0;
    htim15.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim15) != HAL_OK) {
        Error_Handler();
    }

    // TRGO pulses on every update, and that's the ADC's trigger. No pin involved.
    TIM_MasterConfigTypeDef trigger_config = {0};
    trigger_config.MasterOutputTrigger = TIM_TRGO_UPDATE;
    trigger_config.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim15, &trigger_config) != HAL_OK) {
        Error_Handler();
    }
}

void pt_adc_start(void)
{
    // Turns both ADCs on and arms them for the first trigger, then starts the triggers.
    // The length is in words: one per PT per sample.
    if (HAL_ADCEx_MultiModeStart_DMA(&hadc1, (uint32_t *)buffer,
                                     2 * PT_BLOCK_SAMPLES * PT_COUNT) != HAL_OK
        || HAL_TIM_Base_Start(&htim15) != HAL_OK) {
        Error_Handler();
    }
}

const pt_adc_sample_t *pt_adc_block(unsigned half)
{
    return &buffer[half * PT_BLOCK_SAMPLES];
}

// Clears the DMA's flags and calls the half or full callback.
void DMA1_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adc);
}
