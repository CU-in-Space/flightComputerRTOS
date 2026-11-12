#include "tasks/actuator_task.h"

#include <stdbool.h>
#include <stdio.h>

#include "task.h"

#include "main.h"
#include "app/command.h"
#include "app/stand.h"
#include "drivers/servo_pwm.h"
#include "drivers/solenoid_pwm.h"
#include "drivers/stepper.h"
#include "tasks/datalog_task.h"
#include "tasks/interlock_task.h"

_Static_assert(SOLENOID_COUNT == OUTPUT_COUNT / 2 && SOLENOID_COUNT + SERVO_COUNT == OUTPUT_COUNT,
               "app/stand.h: outputs are SOL1-4 then SRV1-4");

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

// Full steps per second, with no ramp: keep them low enough that the motor starts
// without stalling. 2000 steps is one turn of the gearbox output.
#define STEPPER_STEPS_PER_S       400U
#define STEPPER_HOME_STEPS_PER_S  200U
// "stp go" stays between 0 (home) and this. A placeholder until the airbrake is built.
#define STEPPER_TRAVEL_STEPS      2000
// Homing looks for the switch this far at most, then backs off this far and calls it 0.
#define STEPPER_HOME_SEEK_STEPS     (STEPPER_TRAVEL_STEPS + 200)
#define STEPPER_HOME_BACKOFF_STEPS  50
// The DRV8825 takes 1.7 ms to wake. vTaskDelay(3) waits at least 2 ms.
#define STEPPER_WAKE_TICKS  3U

// true: hold position between moves (flight). false: sleep between moves, so the
// coils carry no current on the bench. Each wake can then shift it by up to 2 steps.
#define STEPPER_HOLD_WHEN_IDLE  false

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

// What the stepper is doing. Homing is two moves: to the switch, then off it.
typedef enum {
    STEPPER_IDLE,
    STEPPER_MOVING,
    STEPPER_SEEKING_SWITCH,
    STEPPER_BACKING_OFF,
} stepper_job_t;

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

static stepper_job_t stepper_job;
static bool stepper_awake;
static bool stepper_homed;   // position counts from home, or from "stp zero"

static record_actuators_t latest;   // copied in critical sections

void actuator_init(void)
{
    actuator_queue = xQueueCreate(QUEUE_DEPTH, sizeof(command_t));
    if (actuator_queue == NULL) {
        Error_Handler();
    }
}

// "sol 3 vent on": the number, then the name if it has one.
static void print_solenoid(unsigned i, bool on)
{
    char name[NAME_SIZE + 1];
    stand_spaced_name(KIND_SOL, i, name);
    printf("sol %u%s %s\r\n", i + 1, name, on ? "on" : "off");
}

static void print_servo(unsigned i, uint16_t pulse_us)
{
    char name[NAME_SIZE + 1];
    stand_spaced_name(KIND_SRV, i, name);
    if (pulse_us == 0) {
        printf("srv %u%s off\r\n", i + 1, name);
    } else {
        printf("srv %u%s %u us\r\n", i + 1, name, pulse_us);
    }
}

// A repeated "on" keeps the original start, so it doesn't peak again.
static void set_solenoid(unsigned i, bool on, uint32_t now_ms)
{
    if (on && !solenoids[i].on) {
        solenoids[i].on_since_ms = now_ms;
    }
    solenoids[i].on = on;
}

static bool servo_in_limits(unsigned i, uint16_t pulse_us)
{
    return pulse_us == 0 || (pulse_us >= servo_limits[i].min_us && pulse_us <= servo_limits[i].max_us);
}

// Wakes the driver if it's asleep and starts a move.
static void stepper_start(stepper_job_t job, int32_t steps, uint32_t steps_per_s, bool stop_at_limit)
{
    if (!stepper_awake) {
        stepper_set_awake(true);
        stepper_awake = true;
        vTaskDelay(STEPPER_WAKE_TICKS);
    }
    stepper_move(steps, steps_per_s, stop_at_limit);
    stepper_job = job;
}

// Puts the driver to sleep and forgets home: asleep, nothing holds the position.
static void stepper_release(void)
{
    stepper_set_awake(false);
    stepper_awake = false;
    stepper_homed = false;
    stepper_job = STEPPER_IDLE;
}

// A move, or a zero, needs the interlock ENABLED and the last move finished.
static bool stepper_can_move(interlock_state_t interlock)
{
    if (interlock != INTERLOCK_ENABLED) {
        printf("stp refused: interlock %s\r\n", interlock_state_names[interlock]);
        return false;
    }
    if (stepper_job != STEPPER_IDLE || stepper_moving()) {
        printf("stp busy: stp stop first\r\n");
        return false;
    }
    return true;
}

static void run_command(const command_t *command, interlock_state_t interlock, uint32_t now_ms)
{
    switch (command->type) {
    case COMMAND_STATUS:
        printf("interlock %s, VBAT %lu mV\r\n", interlock_state_names[interlock],
               (unsigned long)interlock_current_vbat_mv());
        for (unsigned i = 0; i < SOLENOID_COUNT; i++) {
            print_solenoid(i, solenoids[i].on);
        }
        for (unsigned i = 0; i < SERVO_COUNT; i++) {
            print_servo(i, servos[i].target_us);
        }
        printf("stp %s at %ld, %s\r\n",
               stepper_job != STEPPER_IDLE ? "moving" : stepper_awake ? "holding" : "asleep",
               (long)stepper_position(), stepper_homed ? "homed" : "not homed");
        break;

    case COMMAND_ALL_OFF:
        for (unsigned i = 0; i < SOLENOID_COUNT; i++) {
            solenoids[i].on = false;
        }
        for (unsigned i = 0; i < SERVO_COUNT; i++) {
            servos[i].target_us = 0;
        }
        stepper_release();
        printf("all off commanded\r\n");
        break;

    case COMMAND_SAFE:
        // Each output goes where its valve goes on an abort (the stand's table
        // and RADIO_VALVE_ABORT). One that drives no valve goes off.
        for (unsigned output = 0; output < OUTPUT_COUNT; output++) {
            const output_t *row = stand_output(output);
            uint16_t drive = 0;
            // Without the interlock nothing may be on, so then it's the same as "off".
            if (interlock != INTERLOCK_ENABLED) {
                drive = 0;
            } else if (!stand_safe_drive(output, row, &drive)) {
                continue;   // this valve stays as it is
            }

            if (output < SOLENOID_COUNT) {
                set_solenoid(output, drive != 0, now_ms);
            } else if (servo_in_limits(output - SOLENOID_COUNT, drive)) {
                servos[output - SOLENOID_COUNT].target_us = drive;
            } else {
                servos[output - SOLENOID_COUNT].target_us = 0;
                printf("srv %u safe position is outside its limits: off\r\n", output - SOLENOID_COUNT + 1);
            }
        }
        stepper_release();
        printf("safe state commanded\r\n");
        break;

    case COMMAND_SOLENOID: {
        // Refuse "on" unless the interlock is ENABLED. Off is always allowed.
        if (command->on && interlock != INTERLOCK_ENABLED) {
            printf("sol %u refused: interlock %s\r\n", command->channel + 1,
                   interlock_state_names[interlock]);
            break;
        }
        set_solenoid(command->channel, command->on, now_ms);
        print_solenoid(command->channel, command->on);
        break;
    }

    case COMMAND_SERVO: {
        // Same rule as the solenoids: a position needs ENABLED, off is always allowed.
        if (command->pulse_us != 0 && interlock != INTERLOCK_ENABLED) {
            printf("srv %u refused: interlock %s\r\n", command->channel + 1,
                   interlock_state_names[interlock]);
            break;
        }
        if (!servo_in_limits(command->channel, command->pulse_us)) {
            printf("srv %u refused: limits are %u-%u us\r\n", command->channel + 1,
                   servo_limits[command->channel].min_us, servo_limits[command->channel].max_us);
            break;
        }
        servos[command->channel].target_us = command->pulse_us;
        print_servo(command->channel, command->pulse_us);
        break;
    }

    case COMMAND_STEPPER_HOME:
        if (!stepper_can_move(interlock)) {
            break;
        }
        // Reply first: waking the driver holds this task up for a few ms, and the
        // console prints its prompt as soon as the command has left the queue.
        printf("stp homing\r\n");
        // Already on the switch: skip straight to backing off it.
        if (stepper_at_limit()) {
            stepper_start(STEPPER_BACKING_OFF, STEPPER_HOME_BACKOFF_STEPS, STEPPER_HOME_STEPS_PER_S, false);
        } else {
            stepper_start(STEPPER_SEEKING_SWITCH, -STEPPER_HOME_SEEK_STEPS, STEPPER_HOME_STEPS_PER_S, true);
        }
        break;

    case COMMAND_STEPPER_ZERO:
        // For when there's no switch yet: wherever it is now becomes home. Needs
        // ENABLED like a move, since the interlock forgets home otherwise.
        if (!stepper_can_move(interlock)) {
            break;
        }
        stepper_set_position(0);
        stepper_homed = true;
        printf("stp zeroed here\r\n");
        break;

    case COMMAND_STEPPER_GO:
        if (!stepper_can_move(interlock)) {
            break;
        }
        if (!stepper_homed) {
            printf("stp refused: not homed (stp home or stp zero)\r\n");
            break;
        }
        if (command->steps < 0 || command->steps > STEPPER_TRAVEL_STEPS) {
            printf("stp refused: travel is 0-%d steps\r\n", STEPPER_TRAVEL_STEPS);
            break;
        }
        printf("stp going to %ld\r\n", (long)command->steps);
        stepper_start(STEPPER_MOVING, command->steps - stepper_position(), STEPPER_STEPS_PER_S, false);
        break;

    case COMMAND_STEPPER_JOG:
        // Blind: no home needed and no travel limit, for bring-up. You watch it.
        if (!stepper_can_move(interlock)) {
            break;
        }
        printf("stp jogging %ld\r\n", (long)command->steps);
        stepper_start(STEPPER_MOVING, command->steps, STEPPER_STEPS_PER_S, false);
        break;

    case COMMAND_STEPPER_STOP:
        // A homing cut short is just a move: it reports where it stopped.
        stepper_stop();
        if (stepper_job != STEPPER_IDLE) {
            stepper_job = STEPPER_MOVING;
        }
        printf("stp stopping\r\n");
        break;
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

        // Leaving ENABLED or a driver FAULT: sleep and forget home. The interlock
        // forgets home even when asleep, since a low battery can reset the driver.
        if (interlock != INTERLOCK_ENABLED && (stepper_awake || stepper_homed)) {
            stepper_release();
            printf("stp off: interlock %s\r\n", interlock_state_names[interlock]);
        } else if (stepper_awake && stepper_fault()) {
            stepper_release();
            printf("stp off: driver FAULT (overcurrent or overtemperature)\r\n");
        }

        // A move has finished: report it, or take the next step of homing.
        if (stepper_job != STEPPER_IDLE && !stepper_moving()) {
            stepper_job_t finished = stepper_job;
            stepper_job = STEPPER_IDLE;

            if (finished == STEPPER_MOVING) {
                printf("stp at %ld\r\n", (long)stepper_position());
            } else if (finished == STEPPER_SEEKING_SWITCH && stepper_at_limit()) {
                stepper_start(STEPPER_BACKING_OFF, STEPPER_HOME_BACKOFF_STEPS, STEPPER_HOME_STEPS_PER_S, false);
            } else if (finished == STEPPER_SEEKING_SWITCH) {
                printf("stp home failed: no switch within %d steps\r\n", STEPPER_HOME_SEEK_STEPS);
            } else if (stepper_at_limit()) {
                printf("stp home failed: switch still pressed, stuck or unplugged\r\n");
            } else {
                stepper_set_position(0);
                stepper_homed = true;
                printf("stp homed\r\n");
            }
        }

        // Bench mode: sleep as soon as the move is done. Home is kept.
        if (!STEPPER_HOLD_WHEN_IDLE && stepper_awake && stepper_job == STEPPER_IDLE) {
            stepper_set_awake(false);
            stepper_awake = false;
        }

        // Every pass, so the log shows each output against the sensors.
        record_actuators_t record = {
            .interlock        = (uint8_t)interlock,
            .vbat_mv          = interlock_current_vbat_mv(),
            .stepper_position = stepper_position(),
            .stepper_awake    = stepper_awake,
        };
        for (unsigned i = 0; i < SOLENOID_COUNT; i++) {
            record.solenoids |= (uint8_t)(solenoids[i].on << i);
        }
        for (unsigned i = 0; i < SERVO_COUNT; i++) {
            record.servo_us[i] = servos[i].output_us;
        }
        datalog_write(RECORD_ACTUATORS, &record, sizeof record);

        taskENTER_CRITICAL();
        latest = record;
        taskEXIT_CRITICAL();

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ACTUATOR_PERIOD_MS));
    }
}

void actuator_latest(record_actuators_t *copy)
{
    taskENTER_CRITICAL();
    *copy = latest;
    taskEXIT_CRITICAL();
}
