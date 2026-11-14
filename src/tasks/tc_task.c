#include "tasks/tc_task.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "drivers/spi4_bus.h"
#include "tasks/datalog_task.h"
#include "tasks/interlock_task.h"

// The chips convert every ~100 ms by themselves, so reading faster gains nothing.
#define TC_PERIOD_MS  100U
// The first conversion after setup takes up to 185 ms. Until then the
// temperature registers still read 0.
#define FIRST_CONVERSION_MS  200U
// How often each chip's setup is read back. A dip in the HAT's supply can reset
// a chip without resetting the STM32, and a reset chip stops converting.
#define SETUP_CHECK_MS  1000U

static tc_reading_t latest[TC_COUNT];
static volatile uint32_t setups_again;

// To the console and the data log.
static void note(const char *text)
{
    printf("%s\r\n", text);
    datalog_write(RECORD_TEXT, text, strlen(text));
}

// Sets up again every chip that has lost its setup. True if any was.
static bool check_setup(bool present[TC_COUNT])
{
    bool set_up_again = false;
    for (unsigned i = 0; i < TC_COUNT; i++) {
        spi4_device_t device = (spi4_device_t)(SPI4_TC1 + i);
        char text[40];

        bool ok = max31856_configured(device);
        if (!ok) {
            ok = max31856_configure(device);
            if (ok) {
                snprintf(text, sizeof text, "tc%u lost its setup: set up again", i + 1);
                note(text);
                set_up_again = true;
                setups_again++;
            }
        }
        if (!ok && present[i]) {
            snprintf(text, sizeof text, "tc%u not responding", i + 1);
            note(text);
        }
        present[i] = ok;
    }
    return set_up_again;
}

void vTcTask(void *pvParameters)
{
    (void)pvParameters;

    bool hat_powered = false;
    bool present[TC_COUNT] = {0};
    unsigned reads_since_check = 0;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        // ENABLED means the HAT has power (the interlock switches SPI4 with it).
        // The chips lose their setup without power, so set them up each time.
        bool powered_now = interlock_current_state() == INTERLOCK_ENABLED;
        if (powered_now != hat_powered) {
            hat_powered = powered_now;
            for (unsigned i = 0; i < TC_COUNT; i++) {
                present[i] = hat_powered && max31856_configure((spi4_device_t)(SPI4_TC1 + i));
                if (hat_powered && !present[i]) {
                    printf("tc%u not responding\r\n", i + 1);
                }
            }
            if (hat_powered) {
                vTaskDelay(pdMS_TO_TICKS(FIRST_CONVERSION_MS));
            }
            reads_since_check = 0;
        }

        if (hat_powered && ++reads_since_check >= SETUP_CHECK_MS / TC_PERIOD_MS) {
            reads_since_check = 0;
            if (check_setup(present)) {
                vTaskDelay(pdMS_TO_TICKS(FIRST_CONVERSION_MS));
            }
        }

        record_tc_t record = {0};
        for (unsigned i = 0; i < TC_COUNT; i++) {
            tc_reading_t reading = {0};
            reading.present = present[i] && max31856_read((spi4_device_t)(SPI4_TC1 + i), &reading.value);

            // Several words, so copied whole, like the PTs.
            taskENTER_CRITICAL();
            latest[i] = reading;
            taskEXIT_CRITICAL();

            if (reading.present) {
                record.tc[i] = (record_tc_channel_t){
                    .present         = 1,
                    .faults          = reading.value.faults,
                    .temp_c          = reading.value.temp_c,
                    .cold_junction_c = reading.value.cold_junction_c,
                };
            }
        }
        datalog_write(RECORD_TC, &record, sizeof record);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TC_PERIOD_MS));
    }
}

void tc_latest(unsigned tc, tc_reading_t *reading)
{
    taskENTER_CRITICAL();
    *reading = latest[tc];
    taskEXIT_CRITICAL();
}

uint32_t tc_setups_again(void)
{
    return setups_again;
}
