/* TODO: state estimation task -- not yet implemented.
 *
 * Expected shape, per docs/how-it-works.md "How to lay out code as it grows":
 *   - reads sensor drivers (src/drivers/, e.g. imu.c, baro.c -- HAL, no
 *     FreeRTOS) on a fixed period or IMU-interrupt notification
 *   - runs the actual estimation math in app/flight_state.c (pure logic, no
 *     HAL, no FreeRTOS -- so it can be unit tested off-board)
 *   - publishes the result on a queue for the airbrake task and the log task
 *
 * Nothing below is real yet -- it just proves the task starts so you have a
 * known-good point to build from.
 */
#include "state_estimation_task.h"

void vStateEstimationTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        /* TODO: read sensors, run estimation, publish result on a queue. */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
