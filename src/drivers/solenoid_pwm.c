#include "drivers/solenoid_pwm.h"

#include "main.h"
#include "board/board_pins.h"

#define PWM_HZ  20000U

static TIM_HandleTypeDef htim1;

//SOL3 is channel 4 and SOL4 is channel 3 because of the routing 
static const uint32_t channels[] = { TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_4, TIM_CHANNEL_3 };

void solenoid_pwm_init(void)
{
    __HAL_RCC_TIM1_CLK_ENABLE();

    // Halting at a breakpoint turns the outputs off. The output goes high-impedannce and
    // The Solenoids 100k pulldown will then deenergize the solenoid
    __HAL_DBGMCU_FREEZE_TIM1();

    // APB2 timers run at twice PCLK2 (240 MHz) because the APB2 prescaler is 2.
    uint32_t timer_clock = HAL_RCC_GetPCLK2Freq() * 2;

    htim1.Instance               = TIM1;
    htim1.Init.Prescaler         = 0;
    htim1.Init.Period            = timer_clock / PWM_HZ - 1;   // 12000 counts per period
    htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
        Error_Handler();
    }

    TIM_OC_InitTypeDef output_config = {0};
    output_config.OCMode      = TIM_OCMODE_PWM1;
    output_config.Pulse       = 0; // start at 0% pwm
    output_config.OCPolarity  = TIM_OCPOLARITY_HIGH;
    output_config.OCIdleState = TIM_OCIDLESTATE_RESET;  
    output_config.OCFastMode  = TIM_OCFAST_DISABLE;
    for (unsigned i = 0; i < sizeof channels / sizeof channels[0]; i++) {
        if (HAL_TIM_PWM_ConfigChannel(&htim1, &output_config, channels[i]) != HAL_OK
            || HAL_TIM_PWM_Start(&htim1, channels[i]) != HAL_OK) {
            Error_Handler();
        }
    }

    // The timer is already driving 0 % when the pins switch over to it.
    GPIO_InitTypeDef pin_config = {0};
    pin_config.Pin       = SOL1_CTL_Pin | SOL2_CTL_Pin | SOL3_CTL_Pin | SOL4_CTL_Pin;
    pin_config.Mode      = GPIO_MODE_AF_PP;
    pin_config.Pull      = GPIO_NOPULL;
    pin_config.Speed     = GPIO_SPEED_FREQ_LOW;
    pin_config.Alternate = SOL_CTL_AF;
    HAL_GPIO_Init(SOL1_CTL_GPIO_Port, &pin_config);   // all four are on port E, so we configure all at once
}

// PWM mode 1: the pin is high while the counter is below the channel's compare
// register (CCR) and low after, so CCR 0 is 0 %, 7200 is 60 % and 12000 is 100 %.
// With preload on, a new CCR waits for the counter to wrap, so no pulse gets cut short.
void solenoid_pwm_set(unsigned solenoid, uint32_t duty_percent)
{
    uint32_t period = __HAL_TIM_GET_AUTORELOAD(&htim1) + 1;
    __HAL_TIM_SET_COMPARE(&htim1, channels[solenoid], period * duty_percent / 100);
}
