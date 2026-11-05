// Crash record, kept in the RTC backup registers: they survive a reset, not a power cycle.
#include "crash.h"

#include <stdio.h>
#include <string.h>

#include "stm32h7xx_hal.h"

#define CRASH_MAGIC  0x43525348U   // "CRSH"

typedef enum {
    CRASH_FAULT = 1,
    CRASH_ASSERT,
    CRASH_STACK_OVERFLOW,
    CRASH_ERROR,
} crash_reason_t;

// Laid over RTC->BKP0R..BKP13R. Word-sized fields only: the registers are 32-bit.
typedef struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;            // fault: the faulting instruction; error: the caller
    uint32_t lr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t bfar;
    uint32_t mmfar;
    uint32_t file;          // assert
    uint32_t line;
    uint32_t task_name[4];  // stack overflow
} crash_record_t;

static volatile crash_record_t *const record = (volatile crash_record_t *)&RTC->BKP0R;

static const struct {
    uint32_t flag;
    const char *name;
} reset_reasons[] = {
    // Checked in order: a power-on also sets BORRST and PINRST, a software reset sets PINRST.
    { RCC_FLAG_IWDG1RST, "watchdog" },
    { RCC_FLAG_WWDG1RST, "window watchdog" },
    { RCC_FLAG_SFTRST,   "software" },
    { RCC_FLAG_PORRST,   "power-on" },
    { RCC_FLAG_BORRST,   "brownout" },
    { RCC_FLAG_PINRST,   "reset pin" },
};

void crash_init(void)
{
    HAL_PWR_EnableBkUpAccess();   // the backup domain is write-protected after reset
    __HAL_RCC_RTC_CLK_ENABLE();
}

__attribute__((noreturn)) static void save_and_reset(crash_reason_t reason)
{
    record->reason = reason;
    record->magic = CRASH_MAGIC;
    NVIC_SystemReset();
}

void crash_fault(const uint32_t *stack_frame)
{
    // The CPU pushed r0-r3, r12, lr, pc, xPSR on entry.
    record->lr = stack_frame[5];
    record->pc = stack_frame[6];
    record->cfsr = SCB->CFSR;
    record->hfsr = SCB->HFSR;
    record->bfar = SCB->BFAR;
    record->mmfar = SCB->MMFAR;
    save_and_reset(CRASH_FAULT);
}

void crash_assert(const char *file, int line)
{
    record->file = (uint32_t)file;   // the string itself is in flash
    record->line = (uint32_t)line;
    save_and_reset(CRASH_ASSERT);
}

void crash_stack_overflow(const char *task_name)
{
    uint32_t name[4] = {0};
    strncpy((char *)name, task_name, sizeof name - 1);
    for (int word = 0; word < 4; word++) {
        record->task_name[word] = name[word];
    }
    save_and_reset(CRASH_STACK_OVERFLOW);
}

void crash_error(void *address)
{
    record->pc = (uint32_t)address;
    save_and_reset(CRASH_ERROR);
}

void crash_report(void)
{
    const char *reset_reason = "unknown";
    for (size_t index = 0; index < sizeof reset_reasons / sizeof reset_reasons[0]; index++) {
        if (__HAL_RCC_GET_FLAG(reset_reasons[index].flag)) {
            reset_reason = reset_reasons[index].name;
            break;
        }
    }
    printf("reset: %s\n", reset_reason);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    if (record->magic != CRASH_MAGIC) {
        return;
    }
    record->magic = 0;

    switch (record->reason) {
    case CRASH_FAULT:
        printf("crash: fault pc=0x%08lx lr=0x%08lx cfsr=0x%08lx hfsr=0x%08lx bfar=0x%08lx mmfar=0x%08lx\n",
               record->pc, record->lr, record->cfsr, record->hfsr, record->bfar, record->mmfar);
        break;
    case CRASH_ASSERT:
        printf("crash: assert %s:%lu\n", (const char *)record->file, record->line);
        break;
    case CRASH_STACK_OVERFLOW: {
        uint32_t name[4];
        for (int word = 0; word < 4; word++) {
            name[word] = record->task_name[word];
        }
        printf("crash: stack overflow in %s\n", (const char *)name);
        break;
    }
    case CRASH_ERROR:
        printf("crash: error at 0x%08lx\n", record->pc);
        break;
    }
}
