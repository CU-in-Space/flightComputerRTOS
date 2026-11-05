// HAL millisecond tick on TIM6, leaving SysTick to FreeRTOS. TICK_INT_PRIORITY is
// above the FreeRTOS line, so it keeps running while the kernel masks interrupts.
#include "stm32h7xx_hal.h"

static TIM_HandleTypeDef tick_timer;

// Called by HAL_Init() and again by HAL_RCC_ClockConfig() after a clock change.
HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    RCC_ClkInitTypeDef clock_config;
    uint32_t flash_latency;
    HAL_RCC_GetClockConfig(&clock_config, &flash_latency);

    // APB timers run at twice PCLK whenever the APB prescaler isn't 1.
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq();
    if (clock_config.APB1CLKDivider != RCC_APB1_DIV1) {
        timer_clock *= 2;
    }

    __HAL_RCC_TIM6_CLK_ENABLE();

    tick_timer.Instance = TIM6;
    tick_timer.Init.Prescaler = timer_clock / 1000000U - 1U;   // 1 MHz count
    tick_timer.Init.Period = 1000U - 1U;                       // 1 kHz update
    tick_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
    tick_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    if (HAL_TIM_Base_Init(&tick_timer) != HAL_OK) {
        return HAL_ERROR;
    }

    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
    uwTickPrio = TickPriority;

    return HAL_TIM_Base_Start_IT(&tick_timer);
}

void TIM6_DAC_IRQHandler(void)
{
    // The flag clear can land after the handler returns and re-enter it; count real updates only.
    if (__HAL_TIM_GET_FLAG(&tick_timer, TIM_FLAG_UPDATE)) {
        __HAL_TIM_CLEAR_FLAG(&tick_timer, TIM_FLAG_UPDATE);
        HAL_IncTick();
    }
}
