/* TODO: SD card logging task -- only brings the card up so far.
 *
 *   - next: FatFs on top of drivers/sd_card.c, writing one pre-allocated log
 *     file, so a power cut can't damage the file system
 *   - then: a logging API that other tasks register their values with at
 *     init, and that this task drains to the card
 */
#include "sd_card_task.h"

#include "drivers/sd_card.h"

/* How long to wait before trying a card that failed to come up again. */
#define SD_CARD_RETRY_MS 1000U

void vSdCardTask(void *pvParameters)
{
    (void)pvParameters;

    /* Keep trying, so a card that is missing or slow at power-up still gets
     * used once it answers. */
    while (sd_card_init() != HAL_OK) {
        vTaskDelay(pdMS_TO_TICKS(SD_CARD_RETRY_MS));
    }

    for (;;) {
        /* TODO: block on the log buffer and write it to the card. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
