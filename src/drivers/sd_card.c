/* SD card on SDMMC1, through the HAL.
 *
 * Transfers run on the SDMMC1 DMA. The HAL's completion callbacks (called from
 * SDMMC1_IRQHandler) wake the waiting task with a task notification, so the CPU
 * is free while the card works. One task owns the card; these calls are not
 * meant to be shared between tasks.
 */
#include "drivers/sd_card.h"

#include "sdmmc.h"

#include "FreeRTOS.h"
#include "task.h"

/* Covers the data transfer and the card's busy time after it. The SD spec
 * allows an SDHC card up to 250 ms of busy per write, so leave margin. */
#define SD_CARD_TIMEOUT_MS 1000U

/* The task blocked in a transfer, for the completion callbacks to wake. */
static TaskHandle_t waiting_task;

/* SDMMC1's DMA reaches AXI SRAM (0x24000000, 512 KB) but not DTCM, and needs
 * a word-aligned buffer (ST AN5200). */
static int dma_reachable(const void *buf)
{
    const uintptr_t addr = (uintptr_t)buf;

    return addr >= 0x24000000U && addr < 0x24080000U && (addr & 3U) == 0U;
}

static void wake_waiting_task(void)
{
    BaseType_t higher_priority_woken = pdFALSE;

    if (waiting_task != NULL) {
        vTaskNotifyGiveFromISR(waiting_task, &higher_priority_woken);
    }
    portYIELD_FROM_ISR(higher_priority_woken);
}

void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hsd)
{
    (void)hsd;
    wake_waiting_task();
}

void HAL_SD_ErrorCallback(SD_HandleTypeDef *hsd)
{
    (void)hsd;
    wake_waiting_task();
}

/* Wait for the DMA transfer, then for the card to be ready for the next one. */
static HAL_StatusTypeDef wait_for_transfer(void)
{
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SD_CARD_TIMEOUT_MS)) == 0U) {
        (void)HAL_SD_Abort(&hsd1);
        return HAL_TIMEOUT;
    }
    if (hsd1.ErrorCode != HAL_SD_ERROR_NONE) {
        return HAL_ERROR;
    }

    const TickType_t start = xTaskGetTickCount();
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(SD_CARD_TIMEOUT_MS)) {
            return HAL_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return HAL_OK;
}

HAL_StatusTypeDef sd_card_init(void)
{
    if (hsd1.State != HAL_SD_STATE_RESET) {
        (void)HAL_SD_DeInit(&hsd1);
    }
    return HAL_SD_Init(&hsd1);
}

HAL_StatusTypeDef sd_card_read(uint8_t *buf, uint32_t block, uint32_t count)
{
    HAL_StatusTypeDef status;

    if (!dma_reachable(buf)) {
        return HAL_ERROR;
    }

    /* Drop any stale notification, then start the transfer. */
    (void)ulTaskNotifyTake(pdTRUE, 0);
    waiting_task = xTaskGetCurrentTaskHandle();

    status = HAL_SD_ReadBlocks_DMA(&hsd1, buf, block, count);
    if (status == HAL_OK) {
        status = wait_for_transfer();
    }

    waiting_task = NULL;
    return status;
}
