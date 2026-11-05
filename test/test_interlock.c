#include <assert.h>

#include "app/interlock.h"

#define GOOD_MV  11100U   // 3S pack at 3.7 V per cell

static void test_enables_after_hold(void)
{
    interlock_t interlock;
    interlock_init(&interlock, 1000);

    interlock_state_t state = interlock_update(&interlock, GOOD_MV, 1000);
    assert(state == INTERLOCK_RECOVERING);
    state = interlock_update(&interlock, GOOD_MV, 1299);
    assert(state == INTERLOCK_RECOVERING);
    state = interlock_update(&interlock, GOOD_MV, 1300);
    assert(state == INTERLOCK_ENABLED);
}

static void test_window_edges(void)
{
    interlock_t interlock;
    interlock_init(&interlock, 0);

    interlock_state_t state = interlock_update(&interlock, INTERLOCK_VBAT_MIN_MV - 1, 0);
    assert(state == INTERLOCK_CABLE_FAULT);
    state = interlock_update(&interlock, INTERLOCK_VBAT_MIN_MV, 0);
    assert(state == INTERLOCK_RECOVERING);
    state = interlock_update(&interlock, INTERLOCK_VBAT_MAX_MV, 0);
    assert(state == INTERLOCK_RECOVERING);
    state = interlock_update(&interlock, INTERLOCK_VBAT_MAX_MV + 1, 0);
    assert(state == INTERLOCK_CABLE_FAULT);
}

static void test_dip_restarts_hold(void)
{
    interlock_t interlock;
    interlock_init(&interlock, 0);

    interlock_update(&interlock, GOOD_MV, 0);
    interlock_state_t state = interlock_update(&interlock, 0, 200);
    assert(state == INTERLOCK_CABLE_FAULT);

    // The hold counts from 250, not from 0.
    state = interlock_update(&interlock, GOOD_MV, 250);
    assert(state == INTERLOCK_RECOVERING);
    state = interlock_update(&interlock, GOOD_MV, 549);
    assert(state == INTERLOCK_RECOVERING);
    state = interlock_update(&interlock, GOOD_MV, 550);
    assert(state == INTERLOCK_ENABLED);
}

static void test_drops_out(void)
{
    interlock_t interlock;
    interlock_init(&interlock, 0);
    interlock_state_t state = interlock_update(&interlock, GOOD_MV, 300);
    assert(state == INTERLOCK_ENABLED);

    // Cables crossed while running: a full hold again before ENABLED.
    state = interlock_update(&interlock, 0, 310);
    assert(state == INTERLOCK_CABLE_FAULT);
    state = interlock_update(&interlock, GOOD_MV, 320);
    assert(state == INTERLOCK_RECOVERING);
    state = interlock_update(&interlock, GOOD_MV, 619);
    assert(state == INTERLOCK_RECOVERING);
    state = interlock_update(&interlock, GOOD_MV, 620);
    assert(state == INTERLOCK_ENABLED);

    // A 4S pack.
    state = interlock_update(&interlock, 16800, 630);
    assert(state == INTERLOCK_CABLE_FAULT);
}

static void test_tick_wrap(void)
{
    interlock_t interlock;
    interlock_init(&interlock, 0xFFFFFF00U);

    interlock_state_t state = interlock_update(&interlock, GOOD_MV, 0xFFFFFF00U + 300U);
    assert(state == INTERLOCK_ENABLED);
}

int main(void)
{
    test_enables_after_hold();
    test_window_edges();
    test_dip_restarts_hold();
    test_drops_out();
    test_tick_wrap();
    return 0;
}
