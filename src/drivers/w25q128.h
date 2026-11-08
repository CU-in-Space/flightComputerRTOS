#ifndef DRIVERS_W25Q128_H
#define DRIVERS_W25Q128_H

#include <stdbool.h>
#include <stdint.h>

// Winbond W25Q128JV, 16 MB of flash on SPI4. Erasing sets bytes to 0xFF and
// programming can only clear bits, so a page must be erased before it's rewritten.
// The waits sleep the calling task, so call these from a task.

#define W25Q128_SIZE         (16UL * 1024UL * 1024UL)
#define W25Q128_SECTOR_SIZE  4096U   // smallest erase
#define W25Q128_PAGE_SIZE    256U    // largest program

// True if the chip answers with the W25Q128's JEDEC ID.
bool w25q128_init(void);

// Reads length bytes from any address.
bool w25q128_read(uint32_t address, uint8_t *data, uint16_t length);

// Programs up to one page and waits for it (up to 3 ms). The bytes must all fall
// in one 256-byte page: past its end the chip wraps to the page's start.
bool w25q128_program_page(uint32_t address, const uint8_t *data, uint16_t length);

// Erases the 4 KB sector holding address and waits for it (up to 400 ms).
// The bus is free for other tasks while it waits.
bool w25q128_erase_sector(uint32_t address);

#endif /* DRIVERS_W25Q128_H */
