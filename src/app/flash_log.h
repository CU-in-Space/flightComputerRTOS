#ifndef APP_FLASH_LOG_H
#define APP_FLASH_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/log_records.h"

// The backup log in the W25Q128: a ring of 64 KB blocks that overwrites its oldest
// when full, about 17 minutes at 16 kB/s. Each log start begins a new run in the
// next block. Nothing is kept in RAM between power cycles: the block headers say
// where the ring is. The layout is in app/log_records.h.
//
// Only the datalog task writes; any task may list runs.

#define FLASH_SETTINGS_ADDRESS  0UL       // block 0 is kept out of the ring for this
#define FLASH_LOG_BLOCK_SIZE    65536UL
#define FLASH_LOG_FIRST_BLOCK   1UL
#define FLASH_LOG_LAST_BLOCK    255UL

typedef struct {
    uint32_t run;
    uint32_t first_block;   // oldest block still on the chip
    uint32_t blocks;        // consecutive, wrapping from the last block to the first
    bool start_lost;        // the ring has overwritten this run's first blocks
} flash_log_run_t;

// Finds the end of the ring and sets up a new run, returned in *run. start goes at
// the head of every block. False if the chip doesn't answer.
bool flash_log_begin(const record_file_start_t *start, uint32_t *run);

// Adds one record, header and payload, as it came out of datalog_write. It may
// wait up to 2 s for a block to erase. False on a flash error.
bool flash_log_append(const void *record, size_t length);

// Writes out the last, part-filled page.
bool flash_log_end(void);

// Lists up to max runs, newest first. False if the chip doesn't answer.
bool flash_log_runs(flash_log_run_t *runs, unsigned max, unsigned *count);

// Flash address of a run's nth surviving block.
uint32_t flash_log_block_address(const flash_log_run_t *run, uint32_t n);

#endif /* APP_FLASH_LOG_H */
