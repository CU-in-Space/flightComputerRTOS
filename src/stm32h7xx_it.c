/* Cortex-M core exception handlers.
 *
 * SVC_Handler, PendSV_Handler and SysTick_Handler are intentionally absent --
 * FreeRTOS supplies them, aliased to these names in FreeRTOSConfig.h. Defining
 * them here too would be a duplicate-symbol link error.
 *
 * Peripheral IRQ handlers (DMA, SPI, USART, ...) go here as they get used;
 * anything not defined falls through to Default_Handler in the startup file.
 */
#include "stm32h7xx_hal.h"

void NMI_Handler(void)
{
    for (;;) {
    }
}

void HardFault_Handler(void)
{
    for (;;) {
    }
}

void MemManage_Handler(void)
{
    for (;;) {
    }
}

void BusFault_Handler(void)
{
    for (;;) {
    }
}

void UsageFault_Handler(void)
{
    for (;;) {
    }
}

void DebugMon_Handler(void)
{
}
