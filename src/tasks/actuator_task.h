#ifndef TASKS_ACTUATOR_TASK_H
#define TASKS_ACTUATOR_TASK_H

#include "FreeRTOS.h"
#include "queue.h"

#include "app/log_records.h"

// Owns the solenoids, servos and stepper. Send it command_t on actuator_queue; it
// runs them every 10 ms and refuses to move anything unless the interlock is ENABLED.

#define SOLENOID_COUNT  4   // SOL1-4
#define SERVO_COUNT     4   // SRV1-4

extern QueueHandle_t actuator_queue;

// Creates actuator_queue. Call before the scheduler starts.
void actuator_init(void);
void vActuatorTask(void *pvParameters);

// Copies what every output was doing on the last pass, as the data log records it.
void actuator_latest(record_actuators_t *copy);

#endif /* TASKS_ACTUATOR_TASK_H */
