#include "app/flash_log.h"

#include <string.h>

#include "drivers/w25q128.h"

#define BLOCK_COUNT  (FLASH_LOG_LAST_BLOCK - FLASH_LOG_FIRST_BLOCK + 1UL)

// Bytes wait here until they fill a page, since a page is the most one program
// can write. write_address is the next byte's place in flash.
static uint8_t page[W25Q128_PAGE_SIZE];
static size_t page_fill;
static uint32_t write_address;
static uint32_t block_end;      // 0 until the run's first block is open

static uint32_t current_block;
static uint32_t current_run;
static uint32_t next_sequence;
static record_file_start_t file_start;

static uint32_t block_after(uint32_t block)
{
    return block >= FLASH_LOG_LAST_BLOCK ? FLASH_LOG_FIRST_BLOCK : block + 1;
}

static uint32_t block_before(uint32_t block)
{
    return block <= FLASH_LOG_FIRST_BLOCK ? FLASH_LOG_LAST_BLOCK : block - 1;
}

// Reads a block's header. header->run is 0 if the block holds no log.
static bool read_header(uint32_t block, flash_block_header_t *header)
{
    if (!w25q128_read(block * FLASH_LOG_BLOCK_SIZE, (uint8_t *)header, sizeof *header)) {
        return false;
    }
    if (memcmp(header->magic, LOG_BLOCK_MAGIC, sizeof header->magic) != 0) {
        header->run = 0;
    }
    return true;
}

// The block written most recently: highest run, then highest sequence. Block 0
// and run 0 if the ring is empty.
static bool find_newest(uint32_t *newest_block, uint32_t *newest_run)
{
    uint32_t newest_sequence = 0;
    *newest_block = 0;
    *newest_run = 0;

    for (uint32_t block = FLASH_LOG_FIRST_BLOCK; block <= FLASH_LOG_LAST_BLOCK; block++) {
        flash_block_header_t header;
        if (!read_header(block, &header)) {
            return false;
        }
        if (header.run > *newest_run
            || (header.run == *newest_run && header.run != 0 && header.sequence >= newest_sequence)) {
            *newest_block = block;
            *newest_run = header.run;
            newest_sequence = header.sequence;
        }
    }
    return true;
}

static bool flush_page(void)
{
    if (page_fill == 0) {
        return true;
    }
    bool ok = w25q128_program_page(write_address - page_fill, page, (uint16_t)page_fill);
    page_fill = 0;
    return ok;
}

static bool put(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    while (length > 0) {
        size_t chunk = W25Q128_PAGE_SIZE - page_fill;
        if (chunk > length) {
            chunk = length;
        }
        memcpy(&page[page_fill], bytes, chunk);
        page_fill += chunk;
        write_address += chunk;
        bytes += chunk;
        length -= chunk;

        if (page_fill == W25Q128_PAGE_SIZE && !flush_page()) {
            return false;
        }
    }
    return true;
}

static bool open_next_block(uint32_t time_ms)
{
    current_block = block_after(current_block);
    write_address = current_block * FLASH_LOG_BLOCK_SIZE;
    block_end = write_address + FLASH_LOG_BLOCK_SIZE;
    if (!w25q128_erase_block(write_address)) {
        return false;
    }

    flash_block_header_t header = { .run = current_run, .sequence = next_sequence++ };
    memcpy(header.magic, LOG_BLOCK_MAGIC, sizeof header.magic);
    record_header_t record = {
        .type    = RECORD_FILE_START,
        .length  = sizeof file_start,
        .time_ms = time_ms,
    };
    return put(&header, sizeof header) && put(&record, sizeof record)
           && put(&file_start, sizeof file_start);
}

bool flash_log_begin(const record_file_start_t *start, uint32_t *run)
{
    uint32_t newest_run;
    if (!w25q128_present() || !find_newest(&current_block, &newest_run)) {
        return false;
    }

    current_run = newest_run + 1;
    next_sequence = 0;
    page_fill = 0;
    write_address = 0;
    block_end = 0;
    file_start = *start;
    *run = current_run;
    return true;
}

bool flash_log_append(const void *record, size_t length)
{
    record_header_t header;
    memcpy(&header, record, sizeof header);
    const uint8_t *payload = (const uint8_t *)record + sizeof header;

    // Every block already opens with this.
    if (header.type == RECORD_FILE_START) {
        return true;
    }
    // Later blocks should open with the calibration in force by then.
    if (header.type == RECORD_LC_CALIBRATION) {
        memcpy(&file_start.lc, payload, sizeof file_start.lc);
    }
    if (header.type == RECORD_LC2_CALIBRATION) {
        memcpy(&file_start.lc2, payload, sizeof file_start.lc2);
    }

    if (block_end - write_address < length
        && (!flush_page() || !open_next_block(header.time_ms))) {
        return false;
    }
    return put(record, length);
}

bool flash_log_end(void)
{
    return flush_page();
}

bool flash_log_runs(flash_log_run_t *runs, unsigned max, unsigned *count)
{
    uint32_t block;
    uint32_t newest_run;
    *count = 0;
    if (!find_newest(&block, &newest_run)) {
        return false;
    }
    if (newest_run == 0) {
        return true;
    }

    // Blocks are written in order, so walking back from the newest meets each
    // run whole, and in order of age.
    for (uint32_t i = 0; i < BLOCK_COUNT; i++) {
        flash_block_header_t header;
        if (!read_header(block, &header)) {
            return false;
        }
        if (header.run == 0) {
            break;
        }
        if (*count == 0 || runs[*count - 1].run != header.run) {
            if (*count == max) {
                break;
            }
            runs[(*count)++] = (flash_log_run_t){ .run = header.run };
        }
        flash_log_run_t *run = &runs[*count - 1];
        run->first_block = block;
        run->blocks++;
        run->start_lost = header.sequence != 0;
        block = block_before(block);
    }
    return true;
}

uint32_t flash_log_block_address(const flash_log_run_t *run, uint32_t n)
{
    uint32_t block = FLASH_LOG_FIRST_BLOCK
                     + (run->first_block - FLASH_LOG_FIRST_BLOCK + n) % BLOCK_COUNT;
    return block * FLASH_LOG_BLOCK_SIZE;
}
