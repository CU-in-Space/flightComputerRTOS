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
} command_type_t;

typedef struct {
    command_type_t type;
    uint8_t channel;    // 0-3 for SOL1-4
    bool on;
} command_t;

#endif /* APP_COMMAND_H */
