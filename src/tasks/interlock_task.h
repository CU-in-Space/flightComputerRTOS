#ifndef TASKS_INTERLOCK_TASK_H
#define TASKS_INTERLOCK_TASK_H

// Reads VBAT_SENSE every 10 ms, runs the cable interlock and shows its state on the HAT LED:
// off = RECOVERING, blinking = CABLE_FAULT, on = ENABLED.
void vInterlockTask(void *pvParameters);

#endif /* TASKS_INTERLOCK_TASK_H */
