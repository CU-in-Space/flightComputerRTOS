#ifndef TASKS_INTERLOCK_TASK_H
#define TASKS_INTERLOCK_TASK_H

#include <stdint.h>

// Cable-swap interlock. Crossed ACT/AUX cables put VBAT_SENSE on a servo line the
// power board pulls to 0 V, so a believable battery voltage means the cables are right.

typedef enum {
    INTERLOCK_RECOVERING,   // VBAT in the window, waiting out the hold
    INTERLOCK_CABLE_FAULT,  // VBAT outside the window: no battery, or crossed cables, or no cables
    INTERLOCK_ENABLED,      // actuators allowed
} interlock_state_t;

// Printable name of each state, indexed by interlock_state_t.
extern const char *const interlock_state_names[];

// Reads VBAT_SENSE every 10 ms, runs the interlock and shows its state on the HAT LED:
// off = RECOVERING, blinking = CABLE_FAULT, on = ENABLED.
void vInterlockTask(void *pvParameters);

// The latest state and VBAT reading. Any task may call these.
interlock_state_t interlock_current_state(void);
uint32_t interlock_current_vbat_mv(void);

#endif /* TASKS_INTERLOCK_TASK_H */
