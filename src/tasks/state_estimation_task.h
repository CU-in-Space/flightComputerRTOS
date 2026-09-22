#ifndef TASKS_STATE_ESTIMATION_TASK_H
#define TASKS_STATE_ESTIMATION_TASK_H

#include "FreeRTOS.h"
#include "task.h"

/* Reads sensors (IMU/baro, via src/drivers/) and produces flight state
 * estimates (altitude, velocity, orientation, ...) for other tasks to
 * consume, e.g. the airbrake task. */
void vStateEstimationTask(void *pvParameters);

#endif /* TASKS_STATE_ESTIMATION_TASK_H */
