#include "drivers/stepper.h"

#include "main.h"
#include "board/board_pins.h"

#define COUNT_HZ  1000000U   // one count per µs

// DIR level for positive steps. Swap the two if positive moves turn toward the switch.
#define DIR_POSITIVE  GPIO_PIN_SET
#define DIR_NEGATIVE  GPIO_PIN_RESET

static TIM_HandleTypeDef htim2;

// Shared with the interrupt.
static volatile int32_t position;
static volatile int32_t direction;    // +1 or -1
static volatile uint32_t remaining;   // steps left in this move, counting the one going out
static volatile bool stop_at_limit4

void stepper_init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();

    // Stop stepping at a breakpoint. The interrupt that counts steps and ends the
    // move can't run while halted, so a running timer would step on past the target.
    __HAL_DBGMCU_FREEZE_TIM2();

    // APB1 timers run at twice PCLK1 (240 MHz), as for TIM4.
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq() * 2;

    htim2.Instance               = TIM2;
    htim2.Init.Prescaler         = timer_clock / COUNT_HZ - 1;
    htim2.Init.Period            = COUNT_HZ / 100 - 1;   // each move sets its own
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&htim2) != HAL_OK) {
        Error_Handler();
    }

    // PWM mode 2: low for the first half of each period, high for the second. Each
    // pulse ends at the update, so the interrupt there counts a finished step, and
    // stopping the counter there leaves the pin low.
    TIM_OC_InitTypeDef output_config = {0};
    output_config.OCMode     = TIM_OCMODE_PWM2;
    output_config.Pulse      = COUNT_HZ / 200;   // half the period above
    output_config.OCPolarity = TIM_OCPOLARITY_HIGH;
    output_config.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim2, &output_config, TIM_CHANNEL_1) != HAL_OK) {
        Error_Handler();
    }

    // Only an overflow raises the update interrupt, not the UG that starts a move.
    __HAL_TIM_URS_ENABLE(&htim2);
    htim2.Instance->EGR = TIM_EGR_UG;   // load the compare value, so the pin is low
    TIM_CCxChannelCmd(TIM2, TIM_CHANNEL_1, TIM_CCx_ENABLE);   // output on, counter still stopped
    __HAL_TIM_ENABLE_IT(&htim2, TIM_IT_UPDATE);

    // Above the FreeRTOS line, so kernel critical sections never hold up a step.
    // Allowed because the handler makes no FreeRTOS calls.
    HAL_NVIC_SetPriority(TIM2_IRQn, 4, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);

    GPIO_InitTypeDef pin_config = {0};
    pin_config.Pin       = STP_STEP_Pin;
    pin_config.Mode      = GPIO_MODE_AF_PP;
    pin_config.Pull      = GPIO_NOPULL;
    pin_config.Speed     = GPIO_SPEED_FREQ_LOW;
    pin_config.Alternate = STP_STEP_AF;
    HAL_GPIO_Init(STP_STEP_GPIO_Port, &pin_config);

    // FAULT is open drain. The pull-up holds it high in case the carrier has none.
    pin_config.Pin       = STP_FAULT_Pin;
    pin_config.Mode      = GPIO_MODE_INPUT;
    pin_config.Pull      = GPIO_PULLUP;
    pin_config.Alternate = 0;
    HAL_GPIO_Init(STP_FAULT_GPIO_Port, &pin_config);

    // The power board pulls the switch up. Its cable also carries VBAT_SENSE, so
    // with it unplugged the interlock already blocks every move.
    pin_config.Pin  = AUX_IN1_Pin;
    pin_config.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(AUX_IN1_GPIO_Port, &pin_config);
}

void stepper_set_awake(bool awake)
{
    if (!awake) {
        stepper_stop();
    }
    HAL_GPIO_WritePin(STP_NSLEEP_GPIO_Port, STP_NSLEEP_Pin, awake ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void stepper_move(int32_t steps, uint32_t steps_per_s, bool stop_at_limit_switch)
{
    if (steps == 0 || stepper_moving()) {
        return;
    }

    direction     = steps > 0 ? 1 : -1;
    remaining     = (uint32_t)(steps > 0 ? steps : -steps);
    stop_at_limit = stop_at_limit_switch;
    HAL_GPIO_WritePin(STP_DIR_GPIO_Port, STP_DIR_Pin, steps > 0 ? DIR_POSITIVE : DIR_NEGATIVE);

    uint32_t period = COUNT_HZ / steps_per_s;
    __HAL_TIM_SET_AUTORELOAD(&htim2, period - 1);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, period / 2);

    // UG loads the new period and restarts the count at 0, so the first step rises
    // half a period from now. DIR has long been set by then (the driver needs 650 ns).
    htim2.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_ENABLE(&htim2);
}

void stepper_stop(void)
{
    // The interrupt finishes the step going out and stops there. If the move has
    // already ended, the 1 is left on a stopped timer and the next move replaces it.
    remaining = 1;
}

bool stepper_moving(void)
{
    return (htim2.Instance->CR1 & TIM_CR1_CEN) != 0;
}

int32_t stepper_position(void)
{
    return position;
}

void stepper_set_position(int32_t new_position)
{
    position = new_position;
}

bool stepper_fault(void)
{
    return HAL_GPIO_ReadPin(STP_FAULT_GPIO_Port, STP_FAULT_Pin) == GPIO_PIN_RESET;
}

bool stepper_at_limit(void)
{
    return HAL_GPIO_ReadPin(AUX_IN1_GPIO_Port, AUX_IN1_Pin) == GPIO_PIN_SET;
}

// Runs at the end of every step.
void TIM2_IRQHandler(void)
{
    __HAL_TIM_CLEAR_IT(&htim2, TIM_IT_UPDATE);

    position += direction;
    remaining--;
    if (stop_at_limit && stepper_at_limit()) {
        remaining = 0;
    }
    if (remaining == 0) {
        // __HAL_TIM_DISABLE won't stop a timer with an output enabled, so clear CEN directly.
        htim2.Instance->CR1 &= ~TIM_CR1_CEN;
    }
}
