/* Entry point -- brings the clocks and kernel up and starts the tasks. */
#include <stdio.h>

#include "main.h"
#include "crash.h"
#include "system_clock.h"
#include "gpio.h"
#include "usart.h"
#include "libc/locks.h"

#include "FreeRTOS.h"
#include "task.h"

#include "tasks/airbrake_task.h"
#include "tasks/log_task.h"
#include "tasks/state_estimation_task.h"
#include "tasks/pt_task.h"
#include "tasks/task_priorities.h"

static void vHeartbeatTask(void *pvParameters);

int main(void)
{
    /* MPU first. The Cortex-M7 speculatively reads ahead, and will fault on
     * the unmapped 0x60000000-0xDFFFFFFF range unless it is marked no-access. */
    MPU_Config();

    /* I-cache is free performance. D-cache stays off until we have
     * cache-maintenance discipline around DMA buffers. */
    SCB_EnableICache();

    /* Order matters: HAL_MspInit() (called from HAL_Init) enables the SYSCFG
     * clock, and SystemClock_Config() needs that to set the overdrive bit
     * required for VOS0 / 480 MHz. */
    HAL_Init();
    crash_init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART3_UART_Init();

    /* Before any task exists, so every task sees working C-library locks. */
    libc_locks_init();

    log_init();
    printf("boot: SYSCLK %lu Hz\n", (unsigned long)SystemCoreClock);
    crash_report();

    if (xTaskCreate(vLogTask,
                    "log",
                    configMINIMAL_STACK_SIZE * 2,
                    NULL,
                    PRIORITY_LOG,
                    NULL) != pdPASS) {
        Error_Handler();
    }

    if (xTaskCreate(vHeartbeatTask,
                    "heartbeat",
                    configMINIMAL_STACK_SIZE * 2,
                    NULL,
                    PRIORITY_HEARTBEAT,
                    NULL) != pdPASS) {
        Error_Handler();
    }

    /* Stub tasks -- see src/tasks/. Priorities/stack sizes are unset
     * placeholders; pick real ones once the tasks do real work. */
    if (xTaskCreate(vStateEstimationTask,
                    "state_est",
                    configMINIMAL_STACK_SIZE * 4,
                    NULL,
                    PRIORITY_FLIGHT_STUBS,
                    NULL) != pdPASS) {
        Error_Handler();
    }

    if (xTaskCreate(vAirbrakeTask,
                    "airbrake",
                    configMINIMAL_STACK_SIZE * 4,
                    NULL,
                    PRIORITY_FLIGHT_STUBS,
                    NULL) != pdPASS) {
        Error_Handler();
    }

    if (xTaskCreate(vPtTask,
                    "pt",
                    configMINIMAL_STACK_SIZE * 4,
                    NULL,
                    PRIORITY_SENSORS,
                    NULL) != pdPASS) {
        Error_Handler();
    }

    vTaskStartScheduler();

    /* Only reached if there was not enough heap to start the scheduler. */
    for (;;) {
    }
}

/* Toggles LD1 (green) every 500 ms. A steady 1 Hz blink means the scheduler is
 * running; a frozen LED means something is hogging the CPU or has faulted. */
static void vHeartbeatTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ------------------------------------------------------------ FreeRTOS hooks */

void vApplicationMallocFailedHook(void)
{
    crash_error(__builtin_return_address(0));
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    crash_stack_overflow(pcTaskName);
}

/* --------------------------------------------------------------------------- */

// Saves an address in the calling function; look it up with addr2line.
void Error_Handler(void)
{
    crash_error(__builtin_return_address(0));
}
