#ifndef DRIVERS_SD_CARD_H
#define DRIVERS_SD_CARD_H

#include <stdint.h>

#include "stm32h7xx_hal.h"

#define SD_CARD_BLOCK_SIZE 512U

/* Bring the card up on SDMMC1 (4-bit, clock set in sdmmc.c). Call from a
 * task, after MX_SDMMC1_SD_Init(). Calling it again tears the interface down
 * first, so it also recovers from a card error. */
HAL_StatusTypeDef sd_card_init(void);

/* Read count blocks of SD_CARD_BLOCK_SIZE bytes, starting at block, into buf.
 * Blocks the calling task until the card is done.
 *
 * buf must be 4-byte aligned and in AXI SRAM: the SDMMC1 DMA can't reach DTCM.
 * The D-cache is off (see main.c); this needs cache maintenance before it is
 * turned on. */
HAL_StatusTypeDef sd_card_read(uint8_t *buf, uint32_t block, uint32_t count);

#endif /* DRIVERS_SD_CARD_H */
