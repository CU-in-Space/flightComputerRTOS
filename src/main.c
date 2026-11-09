/* Entry point -- brings the clocks and kernel up and starts the heartbeat.
 * Flight logic goes in its own tasks, created here alongside the heartbeat.
 */
#include "main.h"
#include "system_clock.h"
#include "gpio.h"
#include "usart.h"
#include "sdmmc.h"
#include "libc/locks.h"

#include "FreeRTOS.h"
#include "task.h"

#include "tasks/airbrake_task.h"
#include "tasks/sd_card_task.h"
#include "tasks/state_estimation_task.h"

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
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART3_UART_Init();
    MX_SDMMC1_SD_Init();

    /* Before any task exists, so every task sees working C-library locks. */
    libc_locks_init();

    if (xTaskCreate(vHeartbeatTask,
                    "heartbeat",
                    configMINIMAL_STACK_SIZE * 2,
                    NULL,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        Error_Handler();
    }

    /* Stub tasks -- see src/tasks/. Priorities/stack sizes are unset
     * placeholders; pick real ones once the tasks do real work. */
    if (xTaskCreate(vStateEstimationTask,
                    "state_est",
                    configMINIMAL_STACK_SIZE * 4,
                    NULL,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        Error_Handler();
    }

    if (xTaskCreate(vAirbrakeTask,
                    "airbrake",
                    configMINIMAL_STACK_SIZE * 4,
                    NULL,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        Error_Handler();
    }

    if (xTaskCreate(vSdCardTask,
                    "sd_card",
                    configMINIMAL_STACK_SIZE * 4,
                    NULL,
                    tskIDLE_PRIORITY + 1,
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

/* FreeRTOS owns SysTick, so HAL's millisecond tick has to be driven from here.
 * configTICK_RATE_HZ is 1000, which is exactly what HAL_GetTick() expects. */
void vApplicationTickHook(void)
{
    HAL_IncTick();
}

void vApplicationMallocFailedHook(void)
{
    Error_Handler();
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    Error_Handler();
}

/* --------------------------------------------------------------------------- */

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
    }
}
