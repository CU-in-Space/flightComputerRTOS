#include "tasks/datalog_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "main.h"
#include "ff.h"
#include "app/flash_log.h"
#include "app/stand.h"
#include "drivers/pt_adc.h"
#include "tasks/interlock_task.h"
#include "tasks/lc_task.h"
#include "tasks/pt_task.h"

// About 4 s of records at ~16 kB/s, for when the card or flash stalls. A power of
// two, so the free-running counters below stay right when they wrap.
#define RING_SIZE   65536U
#define PERIOD_MS   100U    // how often the buffer is written out
#define SYNC_MS     1000U   // how often the file is flushed

#define START_BIT   (1U << 0)
#define STOP_BIT    (1U << 1)

static TaskHandle_t datalog_task_handle;
static SemaphoreHandle_t request_done;   // given after each start or stop request
static SemaphoreHandle_t request_mutex;  // one request at a time: console and radio both ask
static volatile bool request_ok;

// Records wait here for the card and the flash. head counts every byte ever put
// in, each tail every byte taken out, so head - tail is what's waiting, even
// across a wrap.
static uint8_t ring[RING_SIZE];
static volatile uint32_t head;         // written by any task, in critical sections
static volatile uint32_t sd_tail;      // the tails are written only by this task
static volatile uint32_t flash_tail;

// Each stops on its own failure. Recording means at least one is still going.
static volatile bool sd_active;
static volatile bool flash_active;

static datalog_status_t status;   // copied in critical sections

static FATFS fs;
static FIL file;

void datalog_init(void)
{
    request_done = xSemaphoreCreateBinary();
    request_mutex = xSemaphoreCreateMutex();
    if (request_done == NULL || request_mutex == NULL) {
        Error_Handler();
    }
}

static bool is_recording(void)
{
    return sd_active || flash_active;
}

// Room left in the ring. Whichever of the two is further behind decides.
// Called in a critical section.
static uint32_t free_space(void)
{
    uint32_t sd_waiting = sd_active ? head - sd_tail : 0;
    uint32_t flash_waiting = flash_active ? head - flash_tail : 0;
    return RING_SIZE - (sd_waiting > flash_waiting ? sd_waiting : flash_waiting);
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

// Copies out of the ring from a free-running position, wrapping at the end.
static void copy_out(uint32_t from, void *data, size_t length)
{
    size_t at = from % RING_SIZE;
    size_t first = length < RING_SIZE - at ? length : RING_SIZE - at;
    memcpy(data, &ring[at], first);
    memcpy((uint8_t *)data + first, ring, length - first);
}

bool datalog_write(record_type_t type, const void *payload, size_t length)
{
    if (!is_recording()) {
        return false;
    }

    // Header and payload go in together, so records from different tasks never mix.
    taskENTER_CRITICAL();
    record_header_t header = {
        .type    = (uint8_t)type,
        .length  = (uint8_t)length,
        .time_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
    };
    bool recording = is_recording();
    bool fits = recording && free_space() >= sizeof header + length;
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
    xSemaphoreTake(request_mutex, portMAX_DELAY);
    xTaskNotify(datalog_task_handle, bit, eSetBits);
    xSemaphoreTake(request_done, portMAX_DELAY);
    bool ok = request_ok;
    xSemaphoreGive(request_mutex);
    return ok;
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

// Mounts the card and opens a new file. Says why if it can't.
static bool open_sd_file(char name[13])
{
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

    if (!next_file_name(name) || f_open(&file, name, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        printf("log: can't create a file\r\n");
        f_mount(NULL, "", 0);
        return false;
    }
    return true;
}

_Static_assert(sizeof(record_names_t) == CHANNEL_COUNT * NAME_SIZE, "RECORD_NAMES holds every channel's name");

// Every channel's name on this stand, as one record.
static void write_names(void)
{
    record_names_t names = { 0 };
    for (unsigned channel = 0; channel < CHANNEL_COUNT; channel++) {
        strncpy(names.names[channel], stand_channel_name(channel), NAME_SIZE - 1);
    }
    datalog_write(RECORD_NAMES, &names, sizeof names);
}

// Opens a new SD file and flash run and starts taking records. One of the two is
// enough: the flash is there for when the card isn't.
static bool start_recording(void)
{
    if (interlock_current_state() != INTERLOCK_ENABLED) {
        printf("log: HAT has no power\r\n");
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
    lc_calibration(0, &lc);
    start.lc = (record_lc_calibration_t){ lc.zero_code, lc.newtons_per_code };
    lc_calibration(1, &lc);
    start.lc2 = (record_lc_calibration_t){ lc.zero_code, lc.newtons_per_code };

    char name[13] = "";
    bool sd = open_sd_file(name);

    uint32_t run = 0;
    bool flash = flash_log_begin(&start, &run);
    if (!flash) {
        printf("log: flash not responding, no backup\r\n");
    }
    if (!sd && !flash) {
        return false;
    }

    // Queued in the same critical section that turns recording on, so it's
    // always the first record. (Critical sections nest.)
    taskENTER_CRITICAL();
    head = 0;
    sd_tail = 0;
    flash_tail = 0;
    status = (datalog_status_t){ .recording = true, .flash_run = run };
    strcpy(status.file_name, name);
    sd_active = sd;
    flash_active = flash;
    datalog_write(RECORD_FILE_START, &start, sizeof start);
    write_names();
    taskEXIT_CRITICAL();

    if (sd && flash) {
        printf("log: writing %s and flash run %lu\r\n", name, (unsigned long)run);
    } else if (sd) {
        printf("log: writing %s\r\n", name);
    } else {
        printf("log: writing flash run %lu only\r\n", (unsigned long)run);
    }
    return true;
}

// Writes out everything waiting in the ring. False on a card error.
static bool drain_sd(void)
{
    uint32_t end = head;
    while (sd_tail != end) {
        // Up to the end of the ring; the rest, from the start, next time round.
        size_t at = sd_tail % RING_SIZE;
        size_t length = end - sd_tail;
        if (length > RING_SIZE - at) {
            length = RING_SIZE - at;
        }

        UINT written;
        if (f_write(&file, &ring[at], length, &written) != FR_OK || written != length) {
            return false;
        }
        sd_tail += length;

        taskENTER_CRITICAL();
        status.bytes_written += length;
        taskEXIT_CRITICAL();
    }
    return true;
}

// Hands what's waiting to the flash log, one whole record at a time. head only
// ever moves by whole records. False on a flash error.
static bool drain_flash(void)
{
    uint8_t record[sizeof(record_header_t) + UINT8_MAX];
    uint32_t end = head;

    while (flash_tail != end) {
        record_header_t header;
        copy_out(flash_tail, &header, sizeof header);
        size_t length = sizeof header + header.length;
        copy_out(flash_tail, record, length);

        if (!flash_log_append(record, length)) {
            return false;
        }
        flash_tail += length;
    }
    return true;
}

static void stop_taking_records(void)
{
    taskENTER_CRITICAL();
    sd_active = false;
    flash_active = false;
    status.recording = false;
    taskEXIT_CRITICAL();
}

// Stops taking records, writes out the rest and closes both.
static bool stop_recording(void)
{
    bool sd = sd_active;
    bool flash = flash_active;
    stop_taking_records();

    bool sd_ok = !sd || (drain_sd() && f_close(&file) == FR_OK);
    bool flash_ok = !flash || (drain_flash() && flash_log_end());
    if (sd) {
        f_mount(NULL, "", 0);
    }

    if (!sd_ok) {
        printf("log: SD write failed, %s may be cut short\r\n", status.file_name);
    } else if (sd) {
        printf("log: closed %s, %lu bytes\r\n", status.file_name, (unsigned long)status.bytes_written);
    }
    if (!flash_ok) {
        printf("log: flash write failed, run %lu may be cut short\r\n", (unsigned long)status.flash_run);
    } else if (flash) {
        printf("log: closed flash run %lu\r\n", (unsigned long)status.flash_run);
    }
    printf("log: %lu records dropped\r\n", (unsigned long)status.dropped);
    return sd_ok && flash_ok;
}

// The card failed: the flash, if it's still going, carries on alone.
static void sd_failed(void)
{
    printf("log: SD write failed, %s kept up to the last flush\r\n", status.file_name);
    taskENTER_CRITICAL();
    sd_active = false;
    status.file_name[0] = '\0';
    status.recording = flash_active;
    taskEXIT_CRITICAL();
    f_mount(NULL, "", 0);
}

// The flash failed: the card, if it's still going, carries on alone.
static void flash_failed(void)
{
    printf("log: flash write failed, run %lu stops here\r\n", (unsigned long)status.flash_run);
    taskENTER_CRITICAL();
    flash_active = false;
    status.flash_run = 0;
    status.recording = sd_active;
    taskEXIT_CRITICAL();
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
                        is_recording() ? pdMS_TO_TICKS(PERIOD_MS) : portMAX_DELAY);
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

        if (requests & STOP_BIT) {
            if (!is_recording()) {
                printf("log: not recording\r\n");
                request_ok = false;
            } else {
                request_ok = stop_recording();
            }
            xSemaphoreGive(request_done);
        }

        if (requests & START_BIT) {
            if (is_recording()) {
                printf("log: already recording\r\n");
                request_ok = false;
            } else {
                request_ok = start_recording();
                last_sync_ms = now_ms;
            }
            xSemaphoreGive(request_done);
        }

        if (!is_recording()) {
            continue;
        }

        // The card and flash have no power without the HAT's. Stop rather than
        // wait for each write to time out.
        if (interlock_current_state() != INTERLOCK_ENABLED) {
            bool sd = sd_active;
            stop_taking_records();
            if (sd) {
                f_mount(NULL, "", 0);
            }
            printf("log: HAT lost power, the card keeps up to the last flush\r\n");
            continue;
        }

        if (sd_active) {
            bool ok = drain_sd();
            if (ok && now_ms - last_sync_ms >= SYNC_MS) {
                ok = f_sync(&file) == FR_OK;
                last_sync_ms = now_ms;
            }
            if (!ok) {
                sd_failed();
            }
        }

        if (flash_active && !drain_flash()) {
            flash_failed();
        }
    }
}
