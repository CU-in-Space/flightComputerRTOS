/* Task priorities, highest first. Anything that can be busy for a long time
 * sits below everything with a deadline.
 *
 *   6  FreeRTOS timer service (configTIMER_TASK_PRIORITY)
 *   5  safety / interlock
 *   4  actuators
 *   3  sensors
 *   2  radio, storage
 *   1  log, heartbeat, flight stubs
 */
#ifndef TASKS_TASK_PRIORITIES_H
#define TASKS_TASK_PRIORITIES_H

#include "FreeRTOS.h"
#include "task.h"

#define PRIORITY_SENSORS       (tskIDLE_PRIORITY + 3)
#define PRIORITY_LOG           (tskIDLE_PRIORITY + 1)
#define PRIORITY_HEARTBEAT     (tskIDLE_PRIORITY + 1)
#define PRIORITY_FLIGHT_STUBS  (tskIDLE_PRIORITY + 1)

#endif /* TASKS_TASK_PRIORITIES_H */
