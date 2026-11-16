#include <assert.h>
#include <math.h>
#include <stdbool.h>

#include "app/pt_convert.h"

static bool near(float actual, float expected)
{
    return fabsf(actual - expected) < 1e-4f;
}

static void test_ratio(void)
{
    float ratio = 0.0f;

    bool ok = pt_ratio(6554, 65535, &ratio);
    assert(ok);
    assert(near(ratio, 0.1f));

    // Supply drift cancels: same ratio at half the supply.
    ok = pt_ratio(3277, 32768, &ratio);
    assert(ok);
    assert(near(ratio, 0.1f));

    // Supply too low to divide by: sensor unpowered.
    ok = pt_ratio(1000, 100, &ratio);
    assert(!ok);
}

static void test_pressure(void)
{
    const pt_calibration_t calibration = {
        .ratio_at_zero = 0.10f,
        .ratio_at_full = 0.90f,
        .full_scale    = 500.0f,
    };

    assert(near(pt_pressure(&calibration, 0.10f), 0.0f));
    assert(near(pt_pressure(&calibration, 0.50f), 250.0f));
    assert(near(pt_pressure(&calibration, 0.90f), 500.0f));
}

static void test_connector_mv(void)
{
    // Full-scale ADC against 3.3 V, back through the 10k/15k divider: 5.5 V.
    assert(pt_counts_to_connector_mv(65535, 3300) == 5500);
    assert(pt_counts_to_connector_mv(0, 3300) == 0);
}

int main(void)
{
    test_ratio();
    test_pressure();
    test_connector_mv();
    return 0;
}
