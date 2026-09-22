/* TODO: airbrake control task -- not yet implemented.
 *
 *   - this task owns no logic itself: it waits on a queue for state estimates
 *     (see state_estimation_task.h) and calls into app/airbrake_control.c
 *     (pure logic, no HAL) to decide a deployment position
 *   - the actual actuation (PWM/servo) belongs in src/drivers/, called from
 *     here, not from app/
 *
 * Nothing below is real yet -- it just proves the task starts and blinks
 * along with the heartbeat so you have a known-good point to build from.
 */
#include "airbrake_task.h"

void vAirbrakeTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        /* TODO: block on the state-estimate queue instead of polling. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
