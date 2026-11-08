#include "tasks/interlock_task.h"

#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board/board_pins.h"
#include "drivers/spi4_bus.h"
#include "drivers/vbat_adc.h"

#define INTERLOCK_PERIOD_MS  10U
#define LED_BLINK_MS         200U

#define VBAT_MIN_MV  6900U    // 1.25 V at the ADC
#define VBAT_MAX_MV  13300U   // 2.4 V at the ADC
#define HOLD_MS      300U     // VBAT must stay in the window this long before ENABLED

const char *const interlock_state_names[] = {
    [INTERLOCK_RECOVERING]  = "RECOVERING",
    [INTERLOCK_CABLE_FAULT] = "CABLE_FAULT",
    [INTERLOCK_ENABLED]     = "ENABLED",
};

// Written only by this task; any task may read them.
static volatile interlock_state_t current_state = INTERLOCK_RECOVERING;
static volatile uint32_t current_vbat_mv;

interlock_state_t interlock_current_state(void)
{
    return current_state;
}

uint32_t interlock_current_vbat_mv(void)
{
    return current_vbat_mv;
}

void vInterlockTask(void *pvParameters)
{
    (void)pvParameters;

    TickType_t last_wake = xTaskGetTickCount();
    interlock_state_t state = INTERLOCK_RECOVERING;
    uint32_t in_window_since_ms = pdTICKS_TO_MS(last_wake);

    for (;;) {
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        uint32_t vbat_mv = vbat_adc_read_mv();
        interlock_state_t previous = state;

        if (vbat_mv < VBAT_MIN_MV || vbat_mv > VBAT_MAX_MV) {
            // Outside the window is a fault, whatever the state was.
            state = INTERLOCK_CABLE_FAULT;
        } else if (state == INTERLOCK_CABLE_FAULT) {
            // Back in the window after a fault: start the hold timer.
            state = INTERLOCK_RECOVERING;
            in_window_since_ms = now_ms;
        } else if (state == INTERLOCK_RECOVERING && now_ms - in_window_since_ms >= HOLD_MS) {
            // Stayed in the window for the whole hold: enable.
            state = INTERLOCK_ENABLED;
        }
        // Still in the window and nothing to change: stay RECOVERING or ENABLED.

        // The HAT runs off the battery, so it has power only while ENABLED. Its SPI4
        // pins follow straight away, before any task can see the new state.
        if ((state == INTERLOCK_ENABLED) != (previous == INTERLOCK_ENABLED)) {
            spi4_bus_power(state == INTERLOCK_ENABLED);
        }

        // Publish for the other tasks.
        current_state = state;
        current_vbat_mv = vbat_mv;

        // Log only changes, not every reading.
        if (state != previous) {
            printf("interlock: %s, VBAT %lu mV\r\n", interlock_state_names[state], (unsigned long)vbat_mv);
        }

        // HAT LED: off when RECOVERING, blinking when CABLE_FAULT, on when ENABLED.
        bool led_on = state == INTERLOCK_ENABLED
                      || (state == INTERLOCK_CABLE_FAULT && (now_ms / LED_BLINK_MS) % 2);
        HAL_GPIO_WritePin(HAT_LED_GPIO_Port, HAT_LED_Pin, led_on ? GPIO_PIN_SET : GPIO_PIN_RESET);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(INTERLOCK_PERIOD_MS));
    }
}
