/* Scaffold entry point -- brings the kernel up and nothing else.
 * Real flight logic goes in tasks created from vStartupTask().
 */
#include "stm32h7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

void Error_Handler(void);
static void vStartupTask(void *pvParameters);

int main(void)
{
    /* The H7 boots with both caches off. I-cache is free performance; D-cache
     * is deliberately left off until we have cache-maintenance discipline
     * around DMA buffers (SCB_CleanDCache_by_Addr / SCB_InvalidateDCache_by_Addr). */
    SCB_EnableICache();

    HAL_Init();

    /* TODO(clocks): call a CubeMX-generated SystemClock_Config() here. Until
     * then the core runs from the 64 MHz HSI, not the 480 MHz PLL. */

    if (xTaskCreate(vStartupTask,
                    "startup",
                    configMINIMAL_STACK_SIZE * 2,
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

static void vStartupTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
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
