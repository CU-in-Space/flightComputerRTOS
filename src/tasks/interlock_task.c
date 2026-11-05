#include "tasks/interlock_task.h"

#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app/interlock.h"
#include "board/board_pins.h"
#include "drivers/vbat_adc.h"

#define INTERLOCK_PERIOD_MS  10U
#define LED_BLINK_MS         200U

static const char *const state_names[] = {
    [INTERLOCK_RECOVERING]  = "RECOVERING",
    [INTERLOCK_CABLE_FAULT] = "CABLE_FAULT",
    [INTERLOCK_ENABLED]     = "ENABLED",
};

void vInterlockTask(void *pvParameters)
{
    (void)pvParameters;

    TickType_t last_wake = xTaskGetTickCount();
    interlock_t interlock;
    interlock_init(&interlock, pdTICKS_TO_MS(last_wake));
    interlock_state_t previous = interlock.state;

    for (;;) {
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        uint32_t vbat_mv = vbat_adc_read_mv();
        interlock_state_t state = interlock_update(&interlock, vbat_mv, now_ms);

        if (state != previous) {
            printf("interlock: %s, VBAT %lu mV\n", state_names[state], (unsigned long)vbat_mv);
            previous = state;
        }

        // HAT LED: off when RECOVERING, blinking when CABLE_FAULT, on when ENABLED.
        bool led_on = state == INTERLOCK_ENABLED
                      || (state == INTERLOCK_CABLE_FAULT && (now_ms / LED_BLINK_MS) % 2);
        HAL_GPIO_WritePin(HAT_LED_GPIO_Port, HAT_LED_Pin, led_on ? GPIO_PIN_SET : GPIO_PIN_RESET);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(INTERLOCK_PERIOD_MS));
    }
}
