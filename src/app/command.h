#ifndef APP_COMMAND_H
#define APP_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

// What the actuator task accepts on actuator_queue: from the serial console now,
// and from the radio and the static-fire sequence later.

typedef enum {
    COMMAND_STATUS,
    COMMAND_ALL_OFF,
    COMMAND_SOLENOID,
    COMMAND_SERVO,
    COMMAND_STEPPER_HOME,
    COMMAND_STEPPER_ZERO,
    COMMAND_STEPPER_GO,
    COMMAND_STEPPER_JOG,
    COMMAND_STEPPER_STOP,
} command_type_t;

typedef struct {
    command_type_t type;
    uint8_t channel;    // 0-3 for SOL1-4 or SRV1-4
    bool on;            // solenoid
    uint16_t pulse_us;  // servo; 0 = off
    int32_t steps;      // stepper go (a position) or jog (a distance)
} command_t;

#endif /* APP_COMMAND_H */
