// Crash record: what went wrong is saved, the board resets, and the next boot prints it.
// The reset puts every pin back to high impedance, so the power board's pull-downs turn
// every actuator off.
#ifndef CRASH_H
#define CRASH_H

#include <stdint.h>

void crash_init(void);     // call right after HAL_Init()
void crash_report(void);   // printf the reset reason and any saved crash, then clear it

// Each saves a record and resets.
__attribute__((noreturn)) void crash_fault(const uint32_t *stack_frame);   // fault handlers
__attribute__((noreturn)) void crash_assert(const char *file, int line);   // configASSERT
__attribute__((noreturn)) void crash_stack_overflow(const char *task_name);
__attribute__((noreturn)) void crash_error(void *address);                 // Error_Handler, malloc failure

#endif /* CRASH_H */
