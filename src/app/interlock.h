#ifndef APP_INTERLOCK_H
#define APP_INTERLOCK_H

#include <stdint.h>

// Cable-swap interlock. Crossed ACT/AUX cables put VBAT_SENSE on a servo line the
// power board pulls to 0 V, so a believable battery voltage means the cables are right.

#define INTERLOCK_VBAT_MIN_MV  6900U    // 1.25 V at the ADC
#define INTERLOCK_VBAT_MAX_MV  13300U   // 2.4 V at the ADC
#define INTERLOCK_HOLD_MS      300U

typedef enum {
    INTERLOCK_RECOVERING,   // VBAT in the window, waiting out the hold
    INTERLOCK_CABLE_FAULT,  // VBAT outside the window: no battery, or crossed cables
    INTERLOCK_ENABLED,      // actuators allowed
} interlock_state_t;

typedef struct {
    interlock_state_t state;
    uint32_t in_window_since_ms;
} interlock_t;

void interlock_init(interlock_t *interlock, uint32_t now_ms);
interlock_state_t interlock_update(interlock_t *interlock, uint32_t vbat_mv, uint32_t now_ms);

#endif /* APP_INTERLOCK_H */
