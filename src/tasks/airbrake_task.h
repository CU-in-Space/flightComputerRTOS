#ifndef TASKS_AIRBRAKE_TASK_H
#define TASKS_AIRBRAKE_TASK_H

#include "FreeRTOS.h"
#include "task.h"

/* Reads flight state and drives the airbrake servo/actuator. Create from
 * main() after the state estimation task exists, since this task will need
 * to consume its output (queue TBD). */
void vAirbrakeTask(void *pvParameters);

#endif /* TASKS_AIRBRAKE_TASK_H */
