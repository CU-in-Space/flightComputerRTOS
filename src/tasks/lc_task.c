#include "tasks/lc_task.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "app/crc32.h"
#include "app/flash_log.h"
#include "board/board_pins.h"
#include "drivers/ads1220.h"
#include "drivers/w25q128.h"
#include "tasks/datalog_task.h"
#include "tasks/interlock_task.h"

#define BLOCK_SAMPLES    10      // 10 ms at 1000 samples/s
#define LC2_EVERY_BLOCKS 10      // one LC2 sample after every 10 blocks of LC1: about 10 a second
#define DRDY_TIMEOUT_MS  10U     // a sample is due every 1 ms; longer means none is coming
#define GAIN             128.0f
#define SAVED_MAGIC      0x3241434CUL   // "LCA2"

static TaskHandle_t lc_task_handle;
static lc_reading_t latest[LC_COUNT];

// Tare and calibration. Loaded from flash when the HAT gets power, saved after
// each change. Written by the console task, read by this one, so copied in
// critical sections.
static lc_calibration_t calibration[LC_COUNT];

// How they sit in flash. The CRC catches a power cut in the middle of a save.
typedef struct {
    uint32_t magic;
    lc_calibration_t calibration[LC_COUNT];
    uint32_t crc;
} saved_calibration_t;

// DRDY fell: a sample is ready.
void EXTI0_IRQHandler(void)
{
    __HAL_GPIO_EXTI_CLEAR_IT(LC_DRDY_Pin);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(lc_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static bool load_calibration(void)
{
    saved_calibration_t saved;
    if (!w25q128_read(FLASH_SETTINGS_ADDRESS, (uint8_t *)&saved, sizeof saved)
        || saved.magic != SAVED_MAGIC
        || saved.crc != crc32(&saved, offsetof(saved_calibration_t, crc))) {
        return false;
    }
    taskENTER_CRITICAL();
    memcpy(calibration, saved.calibration, sizeof calibration);
    taskEXIT_CRITICAL();
    return true;
}

// Publishes a cell's reading. The code is (output / excitation) x gain x 2^23,
// because the reference is the excitation itself.
static void publish(unsigned cell, int32_t code)
{
    lc_reading_t reading = { .present = true, .code = code };
    reading.mv_per_v = (float)code / (float)ADS1220_FULL_SCALE / GAIN * 1000.0f;

    taskENTER_CRITICAL();
    reading.calibrated = calibration[cell].newtons_per_code != 0.0f;
    reading.newtons = (float)(code - calibration[cell].zero_code) * calibration[cell].newtons_per_code;
    latest[cell] = reading;
    taskEXIT_CRITICAL();
}

// Points the chip at a cell (0 = LC1, 1 = LC2). If this task was held up, the
// old cell may have finished one more sample first, with its DRDY still
// waiting: drop it, so the next DRDY is the new cell's.
static bool select_cell(unsigned cell)
{
    bool ok = ads1220_select(cell);
    ulTaskNotifyTake(pdTRUE, 0);
    return ok;
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
    unsigned blocks = 0;    // of LC1, since LC2 was last read
    bool on_lc2 = false;    // the chip is converting LC2: the next sample is its

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
            if (hat_powered) {
                printf("lc: %s\r\n", load_calibration() ? "tare and calibration loaded from flash"
                                                         : "no calibration saved in flash");
            }
            sum = 0;
            count = 0;
            blocks = 0;
            on_lc2 = false;   // ads1220_start leaves the chip on LC1
            continue;
        }

        // No sample (HAT off, chip not started, or DRDY stopped) or a failed read:
        // publish "no reading" rather than leave an old one standing.
       
        int32_t code;
        if (!sample_ready || !started || !ads1220_read(&code)) {
            taskENTER_CRITICAL();
            latest[0].present = false;
            latest[1].present = false;
            taskEXIT_CRITICAL();
            sum = 0;
            count = 0;
            blocks = 0;
            // Back to LC1, or its blocks would fill with LC2's samples.
            if (on_lc2) {
                on_lc2 = !select_cell(0);
            }
            continue;
        }

        // The one sample from LC2. Back to LC1 first, to keep the gap in its
        // samples short. If the switch fails, the next sample is LC2's again.
        if (on_lc2) {
            on_lc2 = !select_cell(0);
            datalog_write(RECORD_LC2, &code, sizeof code);
            publish(1, code);
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
        publish(0, (int32_t)(sum / BLOCK_SAMPLES));
        sum = 0;
        count = 0;

        // Between two blocks, every tenth time: one sample from LC2. LC1 misses
        // one sample for it, so its blocks stay whole.
        if (++blocks >= LC2_EVERY_BLOCKS) {
            blocks = 0;
            on_lc2 = select_cell(1);
        }
    }
}

void lc_latest(unsigned cell, lc_reading_t *reading)
{
    taskENTER_CRITICAL();
    *reading = latest[cell];
    taskEXIT_CRITICAL();
}

void lc_calibration(unsigned cell, lc_calibration_t *copy)
{
    taskENTER_CRITICAL();
    *copy = calibration[cell];
    taskEXIT_CRITICAL();
}

// After a cell's new zero or scale: records it in the data log, so the decoder
// can follow it, and saves both cells' to flash. The save takes about 50 ms.
static void calibration_changed(unsigned cell)
{
    saved_calibration_t saved = { .magic = SAVED_MAGIC };
    lc_calibration(0, &saved.calibration[0]);
    lc_calibration(1, &saved.calibration[1]);

    record_lc_calibration_t record = { saved.calibration[cell].zero_code,
                                       saved.calibration[cell].newtons_per_code };
    datalog_write(cell == 0 ? RECORD_LC_CALIBRATION : RECORD_LC2_CALIBRATION, &record, sizeof record);

    saved.crc = crc32(&saved, offsetof(saved_calibration_t, crc));
    if (!w25q128_erase_sector(FLASH_SETTINGS_ADDRESS)
        || !w25q128_program_page(FLASH_SETTINGS_ADDRESS, (const uint8_t *)&saved, sizeof saved)) {
        printf("lc: not saved to flash, kept until power off\r\n");
    }
}

// Averages a cell's readings over one second, looking every 10 ms. LC2 only
// has a new one every 100 ms, so each of its ten counts ten times. Runs in the
// caller's task.
static bool average_one_second(unsigned cell, int32_t *code)
{
    int64_t sum = 0;
    for (unsigned i = 0; i < 100; i++) {
        lc_reading_t reading;
        lc_latest(cell, &reading);
        if (!reading.present) {
            return false;
        }
        sum += reading.code;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    *code = (int32_t)(sum / 100);
    return true;
}

bool lc_tare(unsigned cell)
{
    int32_t code;
    if (!average_one_second(cell, &code)) {
        return false;
    }
    taskENTER_CRITICAL();
    calibration[cell].zero_code = code;
    taskEXIT_CRITICAL();
    calibration_changed(cell);
    return true;
}

bool lc_calibrate(unsigned cell, float newtons)
{
    int32_t code;
    // Equal codes would divide by zero. Any real load moves the code by thousands.
    if (!average_one_second(cell, &code) || code == calibration[cell].zero_code) {
        return false;
    }
    taskENTER_CRITICAL();
    calibration[cell].newtons_per_code = newtons / (float)(code - calibration[cell].zero_code);
    taskEXIT_CRITICAL();
    calibration_changed(cell);
    return true;
}
