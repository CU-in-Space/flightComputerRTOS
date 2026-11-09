#ifndef TASKS_SD_CARD_TASK_H
#define TASKS_SD_CARD_TASK_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

/* Owns the SD card: mounts it and writes the flight log, FLIGHT.LOG. Other
 * tasks never touch the card; they log values through the calls below.
 *
 * Usage -- register channels in main(), before vTaskStartScheduler():
 *
 *     static int baro_alt;
 *     baro_alt = sd_log_register("baro_alt_m", SD_LOG_FLOAT);
 *
 * then log from any task, as often as the value changes:
 *
 *     sd_log_float(baro_alt, altitude);
 *
 * Logging never blocks. If the card falls behind and the buffer fills, the
 * record is dropped and counted in the log. tools/sd_log_decode.py turns
 * FLIGHT.LOG into CSV. */
void vSdCardTask(void *pvParameters);

#define SD_LOG_MAX_CHANNELS 24
#define SD_LOG_NAME_LEN     16 /* including the terminating NUL */

typedef enum {
    SD_LOG_FLOAT = 1,
    SD_LOG_INT32 = 2,
} sd_log_type_t;

/* Register a channel to be logged. Call before vTaskStartScheduler(). Names
 * longer than SD_LOG_NAME_LEN - 1 are cut short. Returns the channel id to
 * log with, or -1 if the table is full or the scheduler is already running. */
int sd_log_register(const char *name, sd_log_type_t type);

/* Log one value, timestamped now. Safe from any task (not from interrupts).
 * Returns pdFAIL if the id or type is wrong, or the record was dropped. */
BaseType_t sd_log_float(int channel, float value);
BaseType_t sd_log_int32(int channel, int32_t value);

#endif /* TASKS_SD_CARD_TASK_H */
