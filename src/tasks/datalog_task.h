#ifndef TASKS_DATALOG_TASK_H
#define TASKS_DATALOG_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/log_records.h"

// Records sensor data to the SD card and, as a backup, the W25Q128. Between
// datalog_start and datalog_stop, tasks hand records to datalog_write; this task
// writes them to a new file, LOGnnnn.BIN, flushed once a second, and to a new
// run in the flash ring (app/flash_log.h). Either one carries on if the other
// fails. The format is app/log_records.h.

typedef struct {
    bool recording;
    char file_name[13];     // 8.3 name of the current or last file, "" if none
    uint32_t bytes_written; // into that file
    uint32_t flash_run;     // current or last flash run, 0 if none
    uint32_t dropped;       // records that didn't fit in the buffer
} datalog_status_t;

// Creates the request semaphore and mutex. Call before the scheduler starts.
void datalog_init(void);

// Writes the queued records out every 100 ms while recording.
void vDatalogTask(void *pvParameters);

// Open a new file, or write out the rest and close it. Both wait until the task
// has done it and printed the outcome, which can take a few hundred ms. False if
// it failed (or there was nothing to stop). Any task may call them.
bool datalog_start(void);
bool datalog_stop(void);

// Copies the task's state, for the console.
void datalog_status(datalog_status_t *status);

// Queues one record of at most 255 bytes, stamped with the time now. Any task
// may call it, not an interrupt. False (nothing queued) when not recording or
// the buffer is full.
bool datalog_write(record_type_t type, const void *payload, size_t length);

#endif /* TASKS_DATALOG_TASK_H */
