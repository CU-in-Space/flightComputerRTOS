#ifndef TASKS_ACTUATOR_TASK_H
#define TASKS_ACTUATOR_TASK_H

#include "FreeRTOS.h"
#include "queue.h"

// Owns the solenoids. Send it command_t on actuator_queue; it runs them every
// 10 ms and refuses "on" unless the interlock is ENABLED.

#define SOLENOID_COUNT  4U   // SOL1-4

extern QueueHandle_t actuator_queue;

// Creates actuator_queue. Call before the scheduler starts.
void actuator_init(void);
void vActuatorTask(void *pvParameters);

#endif /* TASKS_ACTUATOR_TASK_H */
