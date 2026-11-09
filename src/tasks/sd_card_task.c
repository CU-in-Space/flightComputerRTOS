/* TODO: SD card logging task -- only mounts the card so far.
 *
 *   - next: a logging API that other tasks register their values with at
 *     init, and that this task drains into one pre-allocated log file, so a
 *     power cut can't damage the file system
 */
#include "sd_card_task.h"

#include "ff.h"

/* How long to wait before trying a card that failed to mount again. */
#define SD_CARD_RETRY_MS 1000U

static FATFS fs;

void vSdCardTask(void *pvParameters)
{
    (void)pvParameters;

    /* Mount now (opt 1) rather than on first use, and keep trying, so a card
     * that is missing or slow at power-up still gets used once it answers. */
    while (f_mount(&fs, "", 1) != FR_OK) {
        vTaskDelay(pdMS_TO_TICKS(SD_CARD_RETRY_MS));
    }

    for (;;) {
        /* TODO: block on the log buffer and write it to the card. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
