#ifndef DRIVERS_SD_CARD_H
#define DRIVERS_SD_CARD_H

#include <stdbool.h>

// The HAT's microSD on SDMMC1, 4-bit at 24 MHz. This file also provides the disk
// functions FatFs calls (disk_initialize, disk_read, ...), so FatFs is the only
// way in: f_mount starts the card.

// Turns on SDMMC1's clock. The pins stay high impedance until sd_card_power.
void sd_card_init(void);

// Called by the interlock as the HAT gains or loses power, like spi4_bus_power.
// Off also marks the card as needing a new f_mount.
void sd_card_power(bool hat_powered);

#endif /* DRIVERS_SD_CARD_H */
