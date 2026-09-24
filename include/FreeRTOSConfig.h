/* FreeRTOS V11.3.1 configuration for STM32H743 (Cortex-M7F).
 * Reference: https://www.freertos.org/a00110.html
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* port.c and the assembler both pull this file in, so guard the C bits. */
#ifndef __ASSEMBLER__
  #include <stdint.h>
  extern uint32_t SystemCoreClock;   /* set by SystemInit() / SystemCoreClockUpdate() */
#endif

/* --------------------------------------------------------------- scheduler */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1   /* CLZ-based, needs priorities <= 32 */
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)   /* 1 ms, matches HAL */
#define configMAX_PRIORITIES                    7
#define configMINIMAL_STACK_SIZE                ((uint16_t)128)      /* in words, not bytes */
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    0
#define configUSE_NEWLIB_REENTRANT              1   /* per-task C library state: see src/libc/locks.c */
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUMBER_OF_CORES                   1

/* -------------------------------------------------------------- allocation */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   ((size_t)(64 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ------------------------------------------------------------------- hooks */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     1   /* vApplicationTickHook drives HAL_IncTick */
#define configCHECK_FOR_STACK_OVERFLOW          2   /* pattern check; cheap and worth it */
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* ------------------------------------------------------------------- stats */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configRECORD_STACK_HIGH_ADDRESS         1   /* lets VS Code's RTOS view show stack size and peak use */

/* ---------------------------------------------------------------- coroutine */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         2

/* ------------------------------------------------------------ soft timers */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            (configMINIMAL_STACK_SIZE * 2)

/* -------------------------------------------------------- optional API set */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1

/* -------------------------------------------------- Cortex-M interrupts ---
 * The H7 implements 4 priority bits. Any ISR that calls a FreeRTOS "FromISR"
 * API must have a numerically HIGHER priority value (= lower urgency) than
 * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY.
 */
#define configPRIO_BITS                             4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

#define configKERNEL_INTERRUPT_PRIORITY \
        (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
        (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Catches misconfigured interrupt priorities and API misuse during bring-up. */
#define configASSERT(x) \
    if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;) {} }

/* The kernel's handlers must land on the CMSIS vector table names used by
 * startup_stm32h743xx.s. Do NOT also define these in stm32h7xx_it.c. */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
