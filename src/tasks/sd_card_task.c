/* SD card task: writes the flight log.
 *
 * FLIGHT.LOG is created once, at full size, in one contiguous run (f_expand).
 * After that the task only overwrites whole sectors inside it -- it never
 * syncs or closes the file -- so the FAT and directory are never written
 * while logging, and a power cut can lose at most the sector being written.
 * Power-pull tested on the bench: no data written before a cut was lost, and
 * the file system stayed intact.
 *
 * Every sector carries the file id, its own position, a session number (one
 * per boot) and a CRC, so data from an earlier file or a torn write never
 * reads as valid. Sessions are appended after the last valid sector, found by
 * binary search at boot since sectors are written strictly in order. Each
 * session starts with a sector listing its channels, so the file describes
 * itself. tools/sd_log_decode.py reads it; keep the two in sync.
 *
 *   sector 0           file header (file id, format version)
 *   per session        channel sector, then data sectors of 40 records
 *
 * All fields are little-endian.
 */
#include "sd_card_task.h"

#include <stddef.h>
#include <string.h>

#include "stm32h7xx_hal.h"
#include "ff.h"
#include "queue.h"

#define SD_LOG_FILE_NAME   "FLIGHT.LOG"
#define SD_LOG_FILE_BYTES  (128UL * 1024UL * 1024UL) /* ~3 h at 10k records/s */
#define SD_LOG_QUEUE_LEN   1024U /* records buffered while the card is busy */
#define SD_LOG_FLUSH_MS    100U  /* write a part-filled sector after this long */
#define SD_CARD_RETRY_MS   1000U /* wait before retrying a card that failed */

#define SD_LOG_MAGIC       0x474C4346U /* "FCLG" */
#define SD_LOG_VERSION     1U

#define SECTOR_SIZE        512U
#define RECORDS_PER_SECTOR 40U

enum { SECTOR_FILE = 1, SECTOR_CHANNELS = 2, SECTOR_DATA = 3 };

typedef struct {
    uint32_t t_ms;    /* ticks since boot, 1 ms each */
    uint16_t channel; /* id from sd_log_register() */
    uint16_t reserved;
    uint32_t value;   /* float or int32, as the channel's type says */
} log_record_t;

typedef struct {
    char name[SD_LOG_NAME_LEN];
    uint8_t type; /* sd_log_type_t */
    uint8_t reserved[3];
} log_channel_t;

typedef struct {
    uint32_t magic;   /* SD_LOG_MAGIC */
    uint32_t file_id; /* random, picked when the file is created */
    uint32_t index;   /* this sector's position in the file */
    uint16_t session; /* boot number within this file, from 1 */
    uint8_t type;     /* SECTOR_* */
    uint8_t count;    /* records or channels in this sector */
    uint32_t dropped; /* records dropped this session so far */
    union {
        log_record_t record[RECORDS_PER_SECTOR];
        log_channel_t channel[SD_LOG_MAX_CHANNELS];
        uint32_t version; /* SECTOR_FILE */
        uint8_t payload[488];
    };
    uint32_t crc;     /* CRC-32 (zlib) of everything above */
} log_sector_t;

_Static_assert(sizeof(log_record_t) == 12, "record layout");
_Static_assert(sizeof(log_channel_t) == 20, "channel layout");
_Static_assert(sizeof(log_sector_t) == SECTOR_SIZE, "sector layout");

static log_channel_t channels[SD_LOG_MAX_CHANNELS];
static int channel_count;
static QueueHandle_t record_queue;
static uint32_t dropped;

static FATFS fs;
static FIL log_file;
static log_sector_t sector; /* the one being built or retried */
static log_sector_t scratch;
static uint32_t file_id;
static uint32_t file_sectors;
static uint32_t next_sector;
static uint16_t session;

/* ------------------------------------------------------------ logging API */

int sd_log_register(const char *name, sd_log_type_t type)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        channel_count >= SD_LOG_MAX_CHANNELS) {
        return -1;
    }
    if (record_queue == NULL) {
        record_queue = xQueueCreate(SD_LOG_QUEUE_LEN, sizeof(log_record_t));
        if (record_queue == NULL) {
            return -1;
        }
    }

    log_channel_t *c = &channels[channel_count];
    strncpy(c->name, name, SD_LOG_NAME_LEN - 1U);
    c->type = (uint8_t)type;
    return channel_count++;
}

static BaseType_t log_value(int channel, sd_log_type_t type, uint32_t value)
{
    if (channel < 0 || channel >= channel_count ||
        channels[channel].type != type) {
        return pdFAIL;
    }

    const log_record_t r = {
        .t_ms = xTaskGetTickCount(),
        .channel = (uint16_t)channel,
        .value = value,
    };
    if (xQueueSend(record_queue, &r, 0) != pdPASS) {
        __atomic_fetch_add(&dropped, 1U, __ATOMIC_RELAXED);
        return pdFAIL;
    }
    return pdPASS;
}

BaseType_t sd_log_float(int channel, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof bits);
    return log_value(channel, SD_LOG_FLOAT, bits);
}

BaseType_t sd_log_int32(int channel, int32_t value)
{
    return log_value(channel, SD_LOG_INT32, (uint32_t)value);
}

/* --------------------------------------------------------------- the file */

/* CRC-32, same as zlib.crc32(). */
static uint32_t crc32(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFU;

    while (len-- > 0U) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c & 1U) ? (c >> 1) ^ 0xEDB88320U : c >> 1;
        }
    }
    return ~c;
}

static uint32_t sector_crc(const log_sector_t *s)
{
    return crc32(s, offsetof(log_sector_t, crc));
}

static FRESULT write_at(uint32_t index, const log_sector_t *s)
{
    UINT written;
    FRESULT fr = f_lseek(&log_file, (FSIZE_t)index * SECTOR_SIZE);

    if (fr == FR_OK) {
        fr = f_write(&log_file, s, SECTOR_SIZE, &written);
    }
    return (fr == FR_OK && written != SECTOR_SIZE) ? FR_DISK_ERR : fr;
}

/* 1 = a valid sector of this file, 0 = not, -1 = read error. */
static int read_valid(uint32_t index)
{
    UINT got;

    if (f_lseek(&log_file, (FSIZE_t)index * SECTOR_SIZE) != FR_OK ||
        f_read(&log_file, &scratch, SECTOR_SIZE, &got) != FR_OK ||
        got != SECTOR_SIZE) {
        return -1;
    }
    return scratch.magic == SD_LOG_MAGIC && scratch.file_id == file_id &&
           scratch.index == index && scratch.crc == sector_crc(&scratch);
}

/* Create FLIGHT.LOG at full size with its header. Only ever runs on a card
 * without the file, before any logging. */
static FRESULT create_log(void)
{
    UINT written;
    FRESULT fr;

    memset(&scratch, 0, sizeof scratch);
    scratch.magic = SD_LOG_MAGIC;
    scratch.file_id = (HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^
                       SysTick->VAL ^ xTaskGetTickCount()) | 1U;
    scratch.type = SECTOR_FILE;
    scratch.version = SD_LOG_VERSION;
    scratch.crc = sector_crc(&scratch);

    fr = f_open(&log_file, SD_LOG_FILE_NAME, FA_CREATE_NEW | FA_WRITE);
    if (fr == FR_OK) {
        fr = f_expand(&log_file, SD_LOG_FILE_BYTES, 1);
    }
    if (fr == FR_OK) {
        fr = f_write(&log_file, &scratch, SECTOR_SIZE, &written);
    }
    if (fr == FR_OK) {
        fr = f_close(&log_file); /* the only FAT/directory update */
    }
    return fr;
}

/* Mount the card and open FLIGHT.LOG (creating it if missing), check its
 * header and find where the data ends. */
static FRESULT open_log(void)
{
    UINT got;
    FRESULT fr = f_mount(&fs, "", 1);

    if (fr == FR_OK) {
        fr = f_open(&log_file, SD_LOG_FILE_NAME, FA_READ | FA_WRITE);
        if (fr == FR_NO_FILE) {
            fr = create_log();
            if (fr == FR_OK) {
                fr = f_open(&log_file, SD_LOG_FILE_NAME, FA_READ | FA_WRITE);
            }
        }
    }
    if (fr == FR_OK) {
        fr = f_read(&log_file, &scratch, SECTOR_SIZE, &got);
    }
    if (fr != FR_OK) {
        return fr;
    }
    if (got != SECTOR_SIZE || scratch.magic != SD_LOG_MAGIC ||
        scratch.type != SECTOR_FILE || scratch.version != SD_LOG_VERSION ||
        scratch.crc != sector_crc(&scratch)) {
        return FR_NO_FILESYSTEM; /* not ours, or damaged: don't write to it */
    }
    file_id = scratch.file_id;
    file_sectors = (uint32_t)(f_size(&log_file) / SECTOR_SIZE);
    return FR_OK;
}

/* First sector after the data already in the file. */
static FRESULT find_end(uint32_t *end)
{
    uint32_t lo = 1U;
    uint32_t hi = file_sectors;

    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2U;
        const int valid = read_valid(mid);

        if (valid < 0) {
            return FR_DISK_ERR;
        }
        if (valid) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    *end = lo;
    return FR_OK;
}

/* Fill in the header and CRC of `sector` and write it at next_sector. On a
 * card error, re-open the card and retry the same sector until it goes in;
 * new records wait in the queue meanwhile. */
static void commit_sector(uint8_t type, uint8_t count)
{
    sector.magic = SD_LOG_MAGIC;
    sector.file_id = file_id;
    sector.index = next_sector;
    sector.session = session;
    sector.type = type;
    sector.count = count;
    sector.dropped = __atomic_load_n(&dropped, __ATOMIC_RELAXED);
    sector.crc = sector_crc(&sector);

    while (write_at(next_sector, &sector) != FR_OK) {
        vTaskDelay(pdMS_TO_TICKS(SD_CARD_RETRY_MS));
        (void)f_close(&log_file); /* drop the failed handle; nothing to flush */
        (void)open_log();
    }
    next_sector++;
}

/* --------------------------------------------------------------- the task */

void vSdCardTask(void *pvParameters)
{
    uint32_t end = 0;
    log_record_t r;
    uint8_t count = 0;
    TickType_t opened = 0;

    (void)pvParameters;

    if (record_queue == NULL) {
        /* Nothing registered: keep a queue anyway so the log still records
         * the session. */
        record_queue = xQueueCreate(1, sizeof(log_record_t));
    }

    /* Keep trying, so a card that is missing or slow at power-up still gets
     * used once it answers. */
    while (open_log() != FR_OK || find_end(&end) != FR_OK) {
        (void)f_close(&log_file);
        vTaskDelay(pdMS_TO_TICKS(SD_CARD_RETRY_MS));
    }

    next_sector = end;
    session = 1U;
    if (end > 1U && read_valid(end - 1U) == 1) {
        session = (uint16_t)(scratch.session + 1U);
    }

    if (next_sector < file_sectors) {
        memset(&sector, 0, sizeof sector);
        memcpy(sector.channel, channels, sizeof channels);
        commit_sector(SECTOR_CHANNELS, (uint8_t)channel_count);
    }

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (count > 0U) {
            const TickType_t age = xTaskGetTickCount() - opened;
            wait = (age < pdMS_TO_TICKS(SD_LOG_FLUSH_MS))
                       ? pdMS_TO_TICKS(SD_LOG_FLUSH_MS) - age
                       : 0;
        }

        if (xQueueReceive(record_queue, &r, wait) == pdPASS) {
            if (next_sector >= file_sectors) {
                continue; /* file full: nowhere to put it */
            }
            if (count == 0U) {
                memset(&sector, 0, sizeof sector);
                opened = xTaskGetTickCount();
            }
            sector.record[count++] = r;
        }

        if (count == RECORDS_PER_SECTOR ||
            (count > 0U &&
             xTaskGetTickCount() - opened >= pdMS_TO_TICKS(SD_LOG_FLUSH_MS))) {
            commit_sector(SECTOR_DATA, count);
            count = 0;
        }
    }
}
