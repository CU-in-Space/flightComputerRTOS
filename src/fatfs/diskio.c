/* FatFs disk I/O layer: connects FatFs (lib/fatfs) to the SD card driver.
 *
 * FatFs reaches its storage only through these functions. There is one drive,
 * number 0: the SD card on SDMMC1. Only the SD card task uses FatFs, so none
 * of this needs locking (FF_FS_REENTRANT is 0 in include/ffconf.h).
 *
 * After any card error the drive reports itself uninitialised again. FatFs
 * then re-initialises the card on its next call, so a glitch on the bus
 * doesn't need handling anywhere else.
 */
#include "ff.h"
#include "diskio.h"

#include <stdint.h>
#include <string.h>

#include "drivers/sd_card.h"

static DSTATUS drive_status = STA_NOINIT;

/* The SD card DMA needs word-aligned buffers, but FatFs can pass any address
 * (e.g. straight into a caller's array). Those go through here, one block at
 * a time. */
static uint8_t bounce[SD_CARD_BLOCK_SIZE] __attribute__((aligned(4)));

static int is_aligned(const void *buf)
{
    return ((uintptr_t)buf & 3U) == 0U;
}

static DRESULT card_failed(void)
{
    drive_status = STA_NOINIT;
    return RES_ERROR;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0U) {
        return STA_NOINIT;
    }
    drive_status = (sd_card_init() == HAL_OK) ? 0U : STA_NOINIT;
    return drive_status;
}

DSTATUS disk_status(BYTE pdrv)
{
    return (pdrv == 0U) ? drive_status : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0U || count == 0U) {
        return RES_PARERR;
    }
    if (drive_status & STA_NOINIT) {
        return RES_NOTRDY;
    }

    if (is_aligned(buff)) {
        return (sd_card_read(buff, sector, count) == HAL_OK) ? RES_OK
                                                             : card_failed();
    }
    for (UINT i = 0; i < count; i++) {
        if (sd_card_read(bounce, sector + i, 1) != HAL_OK) {
            return card_failed();
        }
        memcpy(buff + i * SD_CARD_BLOCK_SIZE, bounce, SD_CARD_BLOCK_SIZE);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0U || count == 0U) {
        return RES_PARERR;
    }
    if (drive_status & STA_NOINIT) {
        return RES_NOTRDY;
    }

    if (is_aligned(buff)) {
        return (sd_card_write(buff, sector, count) == HAL_OK) ? RES_OK
                                                              : card_failed();
    }
    for (UINT i = 0; i < count; i++) {
        memcpy(bounce, buff + i * SD_CARD_BLOCK_SIZE, SD_CARD_BLOCK_SIZE);
        if (sd_card_write(bounce, sector + i, 1) != HAL_OK) {
            return card_failed();
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0U) {
        return RES_PARERR;
    }
    if (drive_status & STA_NOINIT) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        /* Nothing to flush: sd_card_write() only returns once the card has
         * finished programming the data. */
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = sd_card_block_count();
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
