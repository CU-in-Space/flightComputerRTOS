/* Scaffold entry point -- brings the clocks and kernel up, and nothing else.
 * Real flight logic goes in tasks created from vStartupTask().
 */
#include "main.h"
#include "system_clock.h"
#include "gpio.h"
#include "usart.h"

#include "FreeRTOS.h"
#include "task.h"

static void vStartupTask(void *pvParameters);

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
