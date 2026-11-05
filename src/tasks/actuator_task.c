#include "tasks/actuator_task.h"

#include <stdbool.h>
#include <stdio.h>

#include "task.h"

#include "main.h"
#include "app/command.h"
#include "drivers/solenoid_pwm.h"
#include "tasks/interlock_task.h"

#define ACTUATOR_PERIOD_MS  10U
#define QUEUE_DEPTH         8U

// Peak and hold: full current to pull the valve in, then less to keep it there
// and the coil cool.
#define SOLENOID_PEAK_MS       100U
#define SOLENOID_HOLD_PERCENT  60U

typedef struct {
    bool on;
    uint32_t on_since_ms;
} solenoid_t;

QueueHandle_t actuator_queue;

static solenoid_t solenoids[SOLENOID_COUNT];

void actuator_init(void)
{
    actuator_queue = xQueueCreate(QUEUE_DEPTH, sizeof(command_t));
    if (actuator_queue == NULL) {
        Error_Handler();
    }
}

static void run_command(const command_t *command, interlock_state_t interlock, uint32_t now_ms)
{
    switch (command->type) {
    case COMMAND_STATUS:
        printf("interlock %s, VBAT %lu mV\r\n", interlock_state_names[interlock],
               (unsigned long)interlock_current_vbat_mv());
        for (unsigned i = 0; i < SOLENOID_COUNT; i++) {
            printf("sol %u %s\r\n", i + 1, solenoids[i].on ? "on" : "off");
        }
        break;

    case COMMAND_ALL_OFF:
        for (unsigned i = 0; i < SOLENOID_COUNT; i++) {
            solenoids[i].on = false;
        }
        printf("all off commanded\r\n");
        break;

    case COMMAND_SOLENOID: {
        // Refuse "on" unless the interlock is ENABLED. Off is always allowed.
        if (command->on && interlock != INTERLOCK_ENABLED) {
            printf("sol %u refused: interlock %s\r\n", command->channel + 1,
                   interlock_state_names[interlock]);
            break;
        }
        solenoid_t *solenoid = &solenoids[command->channel];
        // A repeated "on" keeps the original start, so it doesn't peak again.
        if (command->on && !solenoid->on) {
            solenoid->on_since_ms = now_ms;
        }
        solenoid->on = command->on;
        printf("sol %u %s\r\n", command->channel + 1, command->on ? "on" : "off");
        break;
    }
    }
}

void vActuatorTask(void *pvParameters)
{
    (void)pvParameters;

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        // One reading of the interlock for the whole pass, so every decision agrees.
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        interlock_state_t interlock = interlock_current_state();

        // Run every command waiting in the queue. If there are none, move on.
        command_t command;
        while (xQueueReceive(actuator_queue, &command, 0) == pdPASS) {
            run_command(&command, interlock, now_ms);
        }

        // Enforce the interlock, then drive each solenoid's gate.
        for (unsigned i = 0; i < SOLENOID_COUNT; i++) {
            // Leaving ENABLED switches everything off for good, so coming back
            // never re-opens a valve by itself.
            if (interlock != INTERLOCK_ENABLED && solenoids[i].on) {
                solenoids[i].on = false;
                printf("sol %u off: interlock %s\r\n", i + 1, interlock_state_names[interlock]);
            }

            // Off: 0 %. On: 100 % for the peak, then the hold duty.
            uint32_t duty_percent = 0;
            if (solenoids[i].on) {
                bool peaking = now_ms - solenoids[i].on_since_ms < SOLENOID_PEAK_MS;
                duty_percent = peaking ? 100 : SOLENOID_HOLD_PERCENT;
            }
            solenoid_pwm_set(i, duty_percent);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ACTUATOR_PERIOD_MS));
    }
}
