#include "tasks/datalog_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "main.h"
#include "ff.h"
#include "drivers/pt_adc.h"
#include "tasks/interlock_task.h"
#include "tasks/lc_task.h"
#include "tasks/pt_task.h"

// About 4 s of records at ~16 kB/s, for when the card stalls. A power of two, so
// the free-running counters below stay right when they wrap.
#define RING_SIZE   65536U
#define PERIOD_MS   100U    // how often the buffer is written out
#define SYNC_MS     1000U   // how often the file is flushed

#define START_BIT   (1U << 0)
#define STOP_BIT    (1U << 1)

static TaskHandle_t datalog_task_handle;
static SemaphoreHandle_t request_done;   // given after each start or stop request
static volatile bool request_ok;

// Records wait here for the card. head counts every byte ever put in, tail every
// byte taken out, so head - tail is what's waiting, even across a wrap.
static uint8_t ring[RING_SIZE];
static volatile uint32_t head;   // written by any task, in critical sections
static volatile uint32_t tail;   // written only by this task

static volatile bool recording;
static datalog_status_t status;   // copied in critical sections

static FATFS fs;
static FIL file;

void datalog_init(void)
{
    request_done = xSemaphoreCreateBinary();
    if (request_done == NULL) {
        Error_Handler();
    }
}

// Copies into the ring at head, wrapping at the end. Called in a critical section.
static void copy_in(const void *data, size_t length)
{
    size_t at = head % RING_SIZE;
    size_t first = length < RING_SIZE - at ? length : RING_SIZE - at;
    memcpy(&ring[at], data, first);
    memcpy(ring, (const uint8_t *)data + first, length - first);
    head += length;
}

bool datalog_write(record_type_t type, const void *payload, size_t length)
{
    if (!recording) {
        return false;
    }

    // Header and payload go in together, so records from different tasks never mix.
    taskENTER_CRITICAL();
    record_header_t header = {
        .type    = (uint8_t)type,
        .length  = (uint8_t)length,
        .time_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
    };
    bool fits = recording && RING_SIZE - (head - tail) >= sizeof header + length;
    if (fits) {
        copy_in(&header, sizeof header);
        copy_in(payload, length);
    } else if (recording) {
        status.dropped++;
    }
    taskEXIT_CRITICAL();
    return fits;
}

// Sends the task a request and waits until it has been handled. The task prints
// the outcome first, so it comes out before the console's next prompt.
static bool request(uint32_t bit)
{
    xTaskNotify(datalog_task_handle, bit, eSetBits);
    xSemaphoreTake(request_done, portMAX_DELAY);
    return request_ok;
}

bool datalog_start(void)
{
    return request(START_BIT);
}

bool datalog_stop(void)
{
    return request(STOP_BIT);
}

void datalog_status(datalog_status_t *copy)
{
    taskENTER_CRITICAL();
    *copy = status;
    taskEXIT_CRITICAL();
}

// One past the highest LOGnnnn.BIN already on the card.
static bool next_file_name(char name[13])
{
    DIR dir;
    FILINFO info;
    unsigned long highest = 0;

    if (f_opendir(&dir, "/") != FR_OK) {
        return false;
    }
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
        if (strlen(info.fname) == 11 && strncmp(info.fname, "LOG", 3) == 0
            && strcmp(&info.fname[7], ".BIN") == 0) {
            unsigned long number = strtoul(&info.fname[3], NULL, 10);
            if (number > highest) {
                highest = number;
            }
        }
    }
    f_closedir(&dir);

    if (highest >= 9999) {
        return false;
    }
    snprintf(name, 13, "LOG%04lu.BIN", highest + 1);
    return true;
}

// Mounts the card, opens a new file and starts taking records.
static bool start_file(void)
{
    if (interlock_current_state() != INTERLOCK_ENABLED) {
        printf("log: HAT has no power\r\n");
        return false;
    }

    // Mounting starts the card (disk_initialize in sd_card.c).
    FRESULT result = f_mount(&fs, "", 1);
    if (result != FR_OK) {
        if (result == FR_NOT_READY) {
            printf("log: no SD card\r\n");
        } else if (result == FR_NO_FILESYSTEM) {
            printf("log: card isn't FAT32, format it on a computer\r\n");
        } else {
            printf("log: SD error %d\r\n", (int)result);
        }
        f_mount(NULL, "", 0);
        return false;
    }

    char name[13];
    if (!next_file_name(name) || f_open(&file, name, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        printf("log: can't create a file\r\n");
        f_mount(NULL, "", 0);
        return false;
    }

    // What the decoder needs to turn raw counts into psi and newtons.
    record_file_start_t start = { .version = LOG_FORMAT_VERSION };
    memcpy(start.magic, LOG_MAGIC, sizeof start.magic);
    for (unsigned i = 0; i < PT_COUNT; i++) {
        const pt_calibration_t *pt = pt_calibration(i);
        start.pt[i] = (record_pt_calibration_t){ pt->ratio_at_zero, pt->ratio_at_full, pt->full_scale };
    }
    lc_calibration_t lc;
    lc_calibration(&lc);
    start.lc = (record_lc_calibration_t){ lc.zero_code, lc.newtons_per_code };

    // Queued in the same critical section that turns recording on, so it's
    // always the first record. (Critical sections nest.)
    taskENTER_CRITICAL();
    head = 0;
    tail = 0;
    status = (datalog_status_t){ .recording = true };
    strcpy(status.file_name, name);
    recording = true;
    datalog_write(RECORD_FILE_START, &start, sizeof start);
    taskEXIT_CRITICAL();

    printf("log: writing %s\r\n", name);
    return true;
}

// Writes out everything waiting in the ring. False on a card error.
static bool drain(void)
{
    uint32_t end = head;
    while (tail != end) {
        // Up to the end of the ring; the rest, from the start, next time round.
        size_t at = tail % RING_SIZE;
        size_t length = end - tail;
        if (length > RING_SIZE - at) {
            length = RING_SIZE - at;
        }

        UINT written;
        if (f_write(&file, &ring[at], length, &written) != FR_OK || written != length) {
            return false;
        }
        tail += length;

        taskENTER_CRITICAL();
        status.bytes_written += length;
        taskEXIT_CRITICAL();
    }
    return true;
}

// Stops taking records. With close, writes out the rest and closes the file;
// without (the card is gone), just lets it go: the card keeps what the last
// flush wrote.
static bool end_file(bool close)
{
    taskENTER_CRITICAL();
    recording = false;
    status.recording = false;
    taskEXIT_CRITICAL();

    bool ok = close && drain() && f_close(&file) == FR_OK;
    f_mount(NULL, "", 0);
    tail = head;
    return ok;
}

void vDatalogTask(void *pvParameters)
{
    (void)pvParameters;

    datalog_task_handle = xTaskGetCurrentTaskHandle();
    uint32_t last_sync_ms = 0;

    for (;;) {
        // While recording, wake every PERIOD_MS to write out. Otherwise sleep
        // until a request.
        uint32_t requests = 0;
        xTaskNotifyWait(0, START_BIT | STOP_BIT, &requests,
                        recording ? pdMS_TO_TICKS(PERIOD_MS) : portMAX_DELAY);
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

        if (requests & STOP_BIT) {
            if (!recording) {
                printf("log: not recording\r\n");
                request_ok = false;
            } else if (end_file(true)) {
                printf("log: closed %s, %lu bytes, %lu records dropped\r\n", status.file_name,
                       (unsigned long)status.bytes_written, (unsigned long)status.dropped);
                request_ok = true;
            } else {
                printf("log: SD write failed, %s may be cut short\r\n", status.file_name);
                request_ok = false;
            }
            xSemaphoreGive(request_done);
        }

        if (requests & START_BIT) {
            if (recording) {
                printf("log: already writing %s\r\n", status.file_name);
                request_ok = false;
            } else {
                request_ok = start_file();
                last_sync_ms = now_ms;
            }
            xSemaphoreGive(request_done);
        }

        if (!recording) {
            continue;
        }

        // The card has no power without the HAT's. Stop rather than wait for
        // each write to time out.
        if (interlock_current_state() != INTERLOCK_ENABLED) {
            end_file(false);
            printf("log: HAT lost power, %s kept up to the last flush\r\n", status.file_name);
            continue;
        }

        bool ok = drain();
        if (ok && now_ms - last_sync_ms >= SYNC_MS) {
            ok = f_sync(&file) == FR_OK;
            last_sync_ms = now_ms;
        }
        if (!ok) {
            end_file(false);
            printf("log: SD write failed, %s kept up to the last flush\r\n", status.file_name);
        }
    }
}
