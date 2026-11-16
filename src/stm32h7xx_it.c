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

// Every fault lands here. Bit 2 of lr says which stack the CPU pushed the
// registers onto: the task's (psp) or the interrupt stack (msp).
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst   lr, #4      \n"
        "ite   eq          \n"
        "mrseq r0, msp     \n"
        "mrsne r0, psp     \n"
        "b     crash_fault \n");
}

void NMI_Handler(void) __attribute__((alias("HardFault_Handler")));
void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));

void DebugMon_Handler(void)
{
}
