#include "app/interlock.h"

void interlock_init(interlock_t *interlock, uint32_t now_ms)
{
    interlock->state = INTERLOCK_RECOVERING;
    interlock->in_window_since_ms = now_ms;
}

interlock_state_t interlock_update(interlock_t *interlock, uint32_t vbat_mv, uint32_t now_ms)
{
    if (vbat_mv < INTERLOCK_VBAT_MIN_MV || vbat_mv > INTERLOCK_VBAT_MAX_MV) {
        interlock->state = INTERLOCK_CABLE_FAULT;
    } else if (interlock->state == INTERLOCK_CABLE_FAULT) {
        interlock->state = INTERLOCK_RECOVERING;
        interlock->in_window_since_ms = now_ms;
    } else if (interlock->state == INTERLOCK_RECOVERING
               && now_ms - interlock->in_window_since_ms >= INTERLOCK_HOLD_MS) {
        interlock->state = INTERLOCK_ENABLED;
    }
    return interlock->state;
}
