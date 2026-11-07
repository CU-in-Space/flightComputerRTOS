#include "tasks/actuator_task.h"

#include <stdbool.h>
#include <stdio.h>

#include "task.h"

#include "main.h"
#include "app/command.h"
#include "drivers/servo_pwm.h"
#include "drivers/solenoid_pwm.h"
#include "tasks/interlock_task.h"

#define ACTUATOR_PERIOD_MS  10U
#define QUEUE_DEPTH         8U

// Peak and hold: full current to pull the valve in, then less to keep it there
// and the coil cool.
#define SOLENOID_PEAK_MS       100U
#define SOLENOID_HOLD_PERCENT  60U

// A servo starting a move draws close to its stall current. Several at once would
// overload the power board's 5 A servo buck, so moves go out one at a time. 60 ms is
// three whole 20 ms periods, because a new pulse only starts at a period boundary.
#define SERVO_STAGGER_MS  60U

typedef struct {
    bool on;
    uint32_t on_since_ms;
} solenoid_t;

typedef struct {
    uint16_t target_us;   // what was commanded; 0 = off
    uint16_t output_us;   // what TIM4 is sending
} servo_t;

typedef struct {
    uint16_t min_us;
    uint16_t max_us;
} servo_limits_t;

// Safe for any hobby servo. Widen a channel once its end stops are measured.
static const servo_limits_t servo_limits[SERVO_COUNT] = {
    { 1000, 2000 },
    { 1000, 2000 },
    { 1000, 2000 },
    { 1000, 2000 },
};

QueueHandle_t actuator_queue;

static solenoid_t solenoids[SOLENOID_COUNT];
static servo_t servos[SERVO_COUNT];
static uint32_t last_servo_move_ms;
static unsigned next_servo;   // where the next search for a move starts

void actuator_init(void)
{
    actuator_queue = xQueueCreate(QUEUE_DEPTH, sizeof(command_t));
    if (actuator_queue == NULL) {
        Error_Handler();
    }
}

static void print_servo(unsigned i, uint16_t pulse_us)
{
    if (pulse_us == 0) {
        printf("srv %u off\r\n", i + 1);
    } else {
        printf("srv %u %u us\r\n", i + 1, pulse_us);
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
        for (unsigned i = 0; i < SERVO_COUNT; i++) {
            print_servo(i, servos[i].target_us);
        }
        break;

    case COMMAND_ALL_OFF:
        for (unsigned i = 0; i < SOLENOID_COUNT; i++) {
            solenoids[i].on = false;
        }
        for (unsigned i = 0; i < SERVO_COUNT; i++) {
            servos[i].target_us = 0;
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

    case COMMAND_SERVO: {
        // Same rule as the solenoids: a position needs ENABLED, off is always allowed.
        if (command->pulse_us != 0 && interlock != INTERLOCK_ENABLED) {
            printf("srv %u refused: interlock %s\r\n", command->channel + 1,
                   interlock_state_names[interlock]);
            break;
        }
        const servo_limits_t *limits = &servo_limits[command->channel];
        if (command->pulse_us != 0
            && (command->pulse_us < limits->min_us || command->pulse_us > limits->max_us)) {
            printf("srv %u refused: limits are %u-%u us\r\n", command->channel + 1,
                   limits->min_us, limits->max_us);
            break;
        }
        servos[command->channel].target_us = command->pulse_us;
        print_servo(command->channel, command->pulse_us);
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

        // Enforce the interlock. Off goes out at once.
        for (unsigned i = 0; i < SERVO_COUNT; i++) {
            // Same as the solenoids: off for good until commanded again.
            if (interlock != INTERLOCK_ENABLED && servos[i].target_us != 0) {
                servos[i].target_us = 0;
                printf("srv %u off: interlock %s\r\n", i + 1, interlock_state_names[interlock]);
            }
            if (servos[i].target_us == 0) {
                servos[i].output_us = 0;
            }
        }

        // One new position per SERVO_STAGGER_MS; a waiting servo keeps its old pulse.
        // Round robin: the search starts after the servo that moved last, so every
        // waiting servo gets a turn.
        if (now_ms - last_servo_move_ms >= SERVO_STAGGER_MS) {
            for (unsigned n = 0; n < SERVO_COUNT; n++) {
                unsigned i = (next_servo + n) % SERVO_COUNT;
                if (servos[i].target_us != servos[i].output_us) {
                    servos[i].output_us = servos[i].target_us;
                    last_servo_move_ms = now_ms;
                    next_servo = (i + 1) % SERVO_COUNT;
                    break;
                }
            }
        }

        for (unsigned i = 0; i < SERVO_COUNT; i++) {
            servo_pwm_set(i, servos[i].output_us);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ACTUATOR_PERIOD_MS));
    }
}
