#include "tasks/tc_task.h"

#include <stdio.h>

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

static tc_reading_t latest[TC_COUNT];

void vTcTask(void *pvParameters)
{
    (void)pvParameters;

    bool hat_powered = false;
    bool present[TC_COUNT] = {0};
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
