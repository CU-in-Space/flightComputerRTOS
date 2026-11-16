#include "drivers/servo_pwm.h"

#include "main.h"
#include "board/board_pins.h"

#define COUNT_HZ   1000000U   // one count per µs, so the compare value is the pulse width
#define PERIOD_US  20000U     // 50 Hz

static TIM_HandleTypeDef htim4;

// SRV3 is channel 4 and SRV4 is channel 3 because of the routing.
static const uint32_t channels[] = { TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_4, TIM_CHANNEL_3 };

void servo_pwm_init(void)
{
    __HAL_RCC_TIM4_CLK_ENABLE();

    // Not frozen at a breakpoint, unlike TIM1: a stopped counter could leave a pin
    // high in the middle of a pulse. Running on, the servos just hold position.

    // APB1 timers run at twice PCLK1 (240 MHz) because the APB1 prescaler is 2.
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq() * 2;

    htim4.Instance               = TIM4;
    htim4.Init.Prescaler         = timer_clock / COUNT_HZ - 1;   // 240 MHz (timer clock) / 239 (prescaler value) = 1 MHz
    htim4.Init.Period            = PERIOD_US - 1;
    htim4.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim4.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&htim4) != HAL_OK) {
        Error_Handler();
    }

    TIM_OC_InitTypeDef output_config = {0};
    output_config.OCMode     = TIM_OCMODE_PWM1;
    output_config.Pulse      = 0;   // no pulses
    output_config.OCPolarity = TIM_OCPOLARITY_HIGH;
    output_config.OCFastMode = TIM_OCFAST_DISABLE;
    for (unsigned i = 0; i < sizeof channels / sizeof channels[0]; i++) {
        if (HAL_TIM_PWM_ConfigChannel(&htim4, &output_config, channels[i]) != HAL_OK
            || HAL_TIM_PWM_Start(&htim4, channels[i]) != HAL_OK) {
            Error_Handler();
        }
    }

    // The timer is already holding every pin low when the pins switch over to it.
    GPIO_InitTypeDef pin_config = {0};
    pin_config.Mode      = GPIO_MODE_AF_PP;
    pin_config.Pull      = GPIO_NOPULL;
    pin_config.Speed     = GPIO_SPEED_FREQ_LOW;
    pin_config.Alternate = SRV_PWM_AF;

    pin_config.Pin = SRV1_PWM_Pin | SRV2_PWM_Pin;   // port D
    HAL_GPIO_Init(SRV1_PWM_GPIO_Port, &pin_config);
    pin_config.Pin = SRV3_PWM_Pin | SRV4_PWM_Pin;   // port B
    HAL_GPIO_Init(SRV3_PWM_GPIO_Port, &pin_config);
}

// PWM mode 1 at one count per µs: the pin is high for the first CCR counts of
// each 20 ms period, so CCR is the pulse width, and 0 means no pulse at all.
void servo_pwm_set(unsigned servo, uint32_t pulse_us)
{
    __HAL_TIM_SET_COMPARE(&htim4, channels[servo], pulse_us);
}
