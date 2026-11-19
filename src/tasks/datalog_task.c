#include "tasks/datalog_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "main.h"
#include "crash.h"
#include "ff.h"
#include "app/stand.h"
#include "drivers/pt_adc.h"
#include "drivers/spi4_bus.h"
#include "tasks/interlock_task.h"
#include "tasks/lc_task.h"
#include "tasks/pt_task.h"
#include "tasks/tc_task.h"

// About 3.7 s of records at ~17.5 kB/s, for when the card stalls. A power of two,
// so the free-running counters below stay right when they wrap.
#define RING_SIZE   65536U
#define PERIOD_MS   100U    // how often the buffer is written out
// How often the file is flushed. A reset or power loss loses what came after the
// last flush, so at most this plus PERIOD_MS.
#define SYNC_MS     200U
// After the HAT gets power, the card has this long to start before it's mounted.
#define AUTO_START_MS  200U
// A new SD file every hour, about 63 MB, so one decodes on a laptop in seconds.
#define NEW_FILE_MS    (60UL * 60UL * 1000UL)
#define HEALTH_MS      1000U   // how often RECORD_HEALTH goes in
#define TASKS_MAX      20U     // for the stack check; there are about 14

#define START_BIT   (1U << 0)
#define STOP_BIT    (1U << 1)

static TaskHandle_t datalog_task_handle;
static SemaphoreHandle_t request_done;   // given after each start or stop request
static SemaphoreHandle_t request_mutex;  // one request at a time: console and radio both ask
static volatile bool request_ok;

// Records wait here for the card. head counts every byte ever put in, tail every
// byte taken out, so head - tail is what's waiting, even across a wrap.
static uint8_t ring[RING_SIZE];
static volatile uint32_t head;         // written by any task, in critical sections
static volatile uint32_t tail;         // written only by this task
static volatile bool recording;
static uint32_t ring_peak;             // most bytes waiting since the last health record

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
        if (head - tail > ring_peak) {
            ring_peak = head - tail;
        }
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
static void fill_names(record_names_t *names)
{
    memset(names, 0, sizeof *names);
    for (unsigned channel = 0; channel < CHANNEL_COUNT; channel++) {
        strncpy(names->names[channel], stand_channel_name(channel), NAME_SIZE - 1);
    }
}

// What the decoder needs to turn raw counts into psi and newtons.
static void fill_file_start(record_file_start_t *start)
{
    memset(start, 0, sizeof *start);
    memcpy(start->magic, LOG_MAGIC, sizeof start->magic);
    start->version = LOG_FORMAT_VERSION;
    for (unsigned i = 0; i < PT_COUNT; i++) {
        const pt_calibration_t *pt = pt_calibration(i);
        start->pt[i] = (record_pt_calibration_t){ pt->ratio_at_zero, pt->ratio_at_full, pt->full_scale };
    }
    lc_calibration_t lc;
    lc_calibration(0, &lc);
    start->lc = (record_lc_calibration_t){ lc.zero_code, lc.newtons_per_code };
    lc_calibration(1, &lc);
    start->lc2 = (record_lc_calibration_t){ lc.zero_code, lc.newtons_per_code };
}

// crash_report's line, and which stand this is. The radio task reads the stand
// number from flash within 100 ms of the HAT getting power, before the log starts.
static size_t boot_line(char *text, size_t size)
{
    snprintf(text, size, "%s; stand %u", crash_boot_text(), stand_number());
    return strlen(text);
}

// Opens a new file and starts taking records.
static bool start_recording(void)
{
    if (interlock_current_state() != INTERLOCK_ENABLED) {
        printf("log: HAT has no power\r\n");
        return false;
    }

    record_file_start_t start;
    fill_file_start(&start);
    record_names_t names;
    fill_names(&names);
    char boot[UINT8_MAX];
    size_t boot_length = boot_line(boot, sizeof boot);

    char name[13] = "";
    if (!open_sd_file(name)) {
        return false;
    }

    // Queued in the same critical section that turns recording on, so they're
    // always the first records. (Critical sections nest.)
    taskENTER_CRITICAL();
    head = 0;
    tail = 0;
    ring_peak = 0;
    status = (datalog_status_t){ .recording = true };
    strcpy(status.file_name, name);
    recording = true;
    datalog_write(RECORD_FILE_START, &start, sizeof start);
    datalog_write(RECORD_NAMES, &names, sizeof names);
    datalog_write(RECORD_TEXT, boot, boot_length);
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

// One record straight into the file, not through the ring.
static bool write_to_file(record_type_t type, const void *payload, size_t length)
{
    record_header_t header = {
        .type    = (uint8_t)type,
        .length  = (uint8_t)length,
        .time_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
    };
    UINT written;
    return f_write(&file, &header, sizeof header, &written) == FR_OK && written == sizeof header
           && f_write(&file, payload, length, &written) == FR_OK && written == length;
}

// Closes the file and carries on in the next one. The ring keeps filling
// meanwhile, and what's in it goes to the new file after its file start, names
// and boot line, so the new file decodes alone.
static bool next_file(void)
{
    record_file_start_t start;
    fill_file_start(&start);
    record_names_t names;
    fill_names(&names);
    char boot[UINT8_MAX];
    size_t boot_length = boot_line(boot, sizeof boot);
    char continues[24];
    snprintf(continues, sizeof continues, "continues %s", status.file_name);

    char name[13];
    if (f_close(&file) != FR_OK || !next_file_name(name)
        || f_open(&file, name, FA_WRITE | FA_CREATE_NEW) != FR_OK
        || !write_to_file(RECORD_FILE_START, &start, sizeof start)
        || !write_to_file(RECORD_NAMES, &names, sizeof names)
        || !write_to_file(RECORD_TEXT, boot, boot_length)
        || !write_to_file(RECORD_TEXT, continues, strlen(continues))) {
        return false;
    }

    taskENTER_CRITICAL();
    strcpy(status.file_name, name);
    status.bytes_written = 0;
    taskEXIT_CRITICAL();
    printf("log: carrying on in %s\r\n", name);
    return true;
}

static void stop_taking_records(void)
{
    taskENTER_CRITICAL();
    recording = false;
    status.recording = false;
    taskEXIT_CRITICAL();
}

// Stops taking records, writes out the rest and closes the file.
static bool stop_recording(void)
{
    stop_taking_records();
    bool ok = drain() && f_close(&file) == FR_OK;
    f_mount(NULL, "", 0);

    if (ok) {
        printf("log: closed %s, %lu bytes\r\n", status.file_name, (unsigned long)status.bytes_written);
    } else {
        printf("log: SD write failed, %s may be cut short\r\n", status.file_name);
    }
    printf("log: %lu records dropped\r\n", (unsigned long)status.dropped);
    return ok;
}

// Once a second: is the logging keeping up, and is anything else going wrong
// quietly. The counts from the other tasks are since the board started.
static void write_health(uint32_t write_max_ms)
{
    static TaskStatus_t tasks[TASKS_MAX];
    record_health_t health = {
        .write_max_ms = (uint16_t)(write_max_ms > UINT16_MAX ? UINT16_MAX : write_max_ms),
        .stack_min_bytes = UINT16_MAX,
        .lc_misses = lc_misses(),
        .tc_setups_again = tc_setups_again(),
        .spi4_errors = spi4_bus_errors(),
    };

    UBaseType_t count = uxTaskGetSystemState(tasks, TASKS_MAX, NULL);
    for (UBaseType_t i = 0; i < count; i++) {
        uint32_t spare = tasks[i].usStackHighWaterMark * sizeof(StackType_t);
        if (spare < health.stack_min_bytes) {
            health.stack_min_bytes = (uint16_t)spare;
            strncpy(health.stack_min_task, tasks[i].pcTaskName, sizeof health.stack_min_task);
        }
    }

    taskENTER_CRITICAL();
    health.dropped = status.dropped;
    health.ring_peak_bytes = ring_peak;
    ring_peak = head - tail;
    taskEXIT_CRITICAL();

    datalog_write(RECORD_HEALTH, &health, sizeof health);
}

void vDatalogTask(void *pvParameters)
{
    (void)pvParameters;

    datalog_task_handle = xTaskGetCurrentTaskHandle();
    uint32_t last_sync_ms = 0;
    uint32_t file_started_ms = 0;
    uint32_t powered_since_ms = 0;
    uint32_t last_health_ms = 0;
    uint32_t write_max_ms = 0;  // longest write-out since the last health record
    bool stopped = false;       // by "log stop", until "log start"
    bool start_tried = false;   // the automatic start, once each time the HAT gets power

    for (;;) {
        // Wake every PERIOD_MS: to write out while recording, and otherwise to
        // see whether the HAT has power, so recording can start by itself.
        uint32_t requests = 0;
        xTaskNotifyWait(0, START_BIT | STOP_BIT, &requests, pdMS_TO_TICKS(PERIOD_MS));
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

        if (requests & STOP_BIT) {
            stopped = true;
            if (!recording) {
                printf("log: not recording\r\n");
                request_ok = false;
            } else {
                request_ok = stop_recording();
            }
            xSemaphoreGive(request_done);
        }

        if (requests & START_BIT) {
            stopped = false;
            if (recording) {
                printf("log: already recording\r\n");
                request_ok = false;
            } else {
                request_ok = start_recording();
                last_sync_ms = now_ms;
                file_started_ms = now_ms;
            }
            xSemaphoreGive(request_done);
        }

        // Recording runs whenever the HAT has power, unless "log stop" said not to.
        if (interlock_current_state() != INTERLOCK_ENABLED) {
            powered_since_ms = now_ms;
            start_tried = false;
        } else if (!recording && !stopped && !start_tried
                   && now_ms - powered_since_ms >= AUTO_START_MS) {
            start_tried = true;
            start_recording();
            last_sync_ms = now_ms;
            file_started_ms = now_ms;
        }

        if (!recording) {
            continue;
        }

        // The card has no power without the HAT's. Stop rather than wait for
        // each write to time out.
        if (interlock_current_state() != INTERLOCK_ENABLED) {
            stop_taking_records();
            f_mount(NULL, "", 0);
            printf("log: HAT lost power, the card keeps up to the last flush\r\n");
            continue;
        }

        if (now_ms - last_health_ms >= HEALTH_MS) {
            write_health(write_max_ms);
            last_health_ms = now_ms;
            write_max_ms = 0;
        }

        bool ok = drain();
        if (ok && now_ms - file_started_ms >= NEW_FILE_MS) {
            ok = next_file();   // closing the old file flushes it
            file_started_ms = now_ms;
            last_sync_ms = now_ms;
        } else if (ok && now_ms - last_sync_ms >= SYNC_MS) {
            ok = f_sync(&file) == FR_OK;
            last_sync_ms = now_ms;
        }
        uint32_t took_ms = pdTICKS_TO_MS(xTaskGetTickCount()) - now_ms;
        if (took_ms > write_max_ms) {
            write_max_ms = took_ms;
        }

        if (!ok) {
            printf("log: SD write failed, %s kept up to the last flush\r\n", status.file_name);
            stop_taking_records();
            f_mount(NULL, "", 0);
        }
    }
}
