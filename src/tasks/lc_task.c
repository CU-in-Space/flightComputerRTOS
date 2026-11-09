#include "tasks/lc_task.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "board/board_pins.h"
#include "drivers/ads1220.h"
#include "tasks/datalog_task.h"
#include "tasks/interlock_task.h"

#define BLOCK_SAMPLES   10      // 10 ms at 1000 samples/s
#define DRDY_TIMEOUT_MS 10U     // a sample is due every 1 ms; longer means none is coming
#define GAIN            128.0f

static TaskHandle_t lc_task_handle;
static lc_reading_t latest;

// Tare and calibration, in RAM until stage 7b keeps them in flash.
// Written by the console task, read by this one, so copied in critical sections.
static lc_calibration_t calibration;

// DRDY fell: a sample is ready.
void EXTI0_IRQHandler(void)
{
    __HAL_GPIO_EXTI_CLEAR_IT(LC_DRDY_Pin);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(lc_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

void vLcTask(void *pvParameters)
{
    (void)pvParameters;

    lc_task_handle = xTaskGetCurrentTaskHandle();
    bool hat_powered = false;
    bool started = false;
    int32_t codes[BLOCK_SAMPLES];
    int64_t sum = 0;
    unsigned count = 0;

    for (;;) {
        // Woken by each DRDY. The timeout lets it notice the HAT losing or getting
        // power even when no samples come.
        bool sample_ready = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DRDY_TIMEOUT_MS)) > 0;

        // Configure the ADS1220 on power on
        bool powered_now = interlock_current_state() == INTERLOCK_ENABLED;
        if (powered_now != hat_powered) {
            hat_powered = powered_now;
            started = hat_powered && ads1220_start();
            if (hat_powered && !started) {
                printf("lc: ADS1220 not responding\r\n");
            }
            sum = 0;
            count = 0;
            continue;
        }

        // No sample (HAT off, chip not started, or DRDY stopped) or a failed read:
        // publish "no reading" rather than leave an old one standing.
       
        int32_t code;
        if (!sample_ready || !started || !ads1220_read(&code)) {
            taskENTER_CRITICAL();
            latest.present = false;
            taskEXIT_CRITICAL();
            sum = 0;
            count = 0;
            continue;
        }

         //if the read is sucessful, code will contain the read value
        codes[count] = code;
        sum += code;
        if (++count < BLOCK_SAMPLES) {
            continue;
        }
        _Static_assert(sizeof(record_lc_t) == sizeof codes, "record_lc_t must match a block");
        datalog_write(RECORD_LC, codes, sizeof codes);

        //reach here after counting Block Samples
        lc_reading_t reading = { .present = true, .code = (int32_t)(sum / BLOCK_SAMPLES) };
        sum = 0;
        count = 0;

        // The code is (output / excitation) x gain x 2^23, because the reference
        // is the excitation itself.
        reading.mv_per_v = (float)reading.code / (float)ADS1220_FULL_SCALE / GAIN * 1000.0f;

        taskENTER_CRITICAL();
        reading.calibrated = calibration.newtons_per_code != 0.0f;
        reading.newtons = (float)(reading.code - calibration.zero_code) * calibration.newtons_per_code;
        latest = reading;
        taskEXIT_CRITICAL();
    }
}

void lc_latest(lc_reading_t *reading)
{
    taskENTER_CRITICAL();
    *reading = latest;
    taskEXIT_CRITICAL();
}

void lc_calibration(lc_calibration_t *copy)
{
    taskENTER_CRITICAL();
    *copy = calibration;
    taskEXIT_CRITICAL();
}

// Records a new zero or scale in the data log, so the decoder can follow it.
static void log_calibration(void)
{
    lc_calibration_t now;
    lc_calibration(&now);
    record_lc_calibration_t record = { now.zero_code, now.newtons_per_code };
    datalog_write(RECORD_LC_CALIBRATION, &record, sizeof record);
}

// Averages the 10 ms readings over one second. Runs in the caller's task.
static bool average_one_second(int32_t *code)
{
    int64_t sum = 0;
    for (unsigned i = 0; i < 100; i++) {
        lc_reading_t reading;
        lc_latest(&reading);
        if (!reading.present) {
            return false;
        }
        sum += reading.code;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    *code = (int32_t)(sum / 100);
    return true;
}

bool lc_tare(void)
{
    int32_t code;
    if (!average_one_second(&code)) {
        return false;
    }
    taskENTER_CRITICAL();
    calibration.zero_code = code;
    taskEXIT_CRITICAL();
    log_calibration();
    return true;
}

bool lc_calibrate(float newtons)
{
    int32_t code;
    // Equal codes would divide by zero. Any real load moves the code by thousands.
    if (!average_one_second(&code) || code == calibration.zero_code) {
        return false;
    }
    taskENTER_CRITICAL();
    calibration.newtons_per_code = newtons / (float)(code - calibration.zero_code);
    taskEXIT_CRITICAL();
    log_calibration();
    return true;
}
