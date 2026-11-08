#include "drivers/w25q128.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "drivers/spi4_bus.h"

#define CMD_WRITE_ENABLE  0x06U
#define CMD_READ_STATUS   0x05U
#define CMD_READ_DATA     0x03U
#define CMD_PAGE_PROGRAM  0x02U
#define CMD_SECTOR_ERASE  0x20U
#define CMD_JEDEC_ID      0x9FU

#define STATUS_BUSY       0x01U

// Datasheet maximums, rounded up.
#define PROGRAM_TIMEOUT_MS  5U
#define ERASE_TIMEOUT_MS    500U

// Command byte, then the 24-bit address, most significant byte first.
static void put_command(uint8_t *buffer, uint8_t command, uint32_t address)
{
    buffer[0] = command;
    buffer[1] = (uint8_t)(address >> 16);
    buffer[2] = (uint8_t)(address >> 8);
    buffer[3] = (uint8_t)address;
}

// Polls the BUSY bit until the program or erase is done. Each poll is its own
// transfer, so the thermocouples keep reading in between.
static bool wait_until_ready(uint32_t timeout_ms)
{
    const uint8_t command = CMD_READ_STATUS;
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        uint8_t status;
        if (!spi4_transfer(SPI4_FLASH, &command, 1, &status, 1)) {
            return false;
        }
        if ((status & STATUS_BUSY) == 0) {
            return true;
        }
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(1);
    }
}

// Every program and erase needs this first; the chip clears it again when done.
static bool write_enable(void)
{
    const uint8_t command = CMD_WRITE_ENABLE;
    return spi4_transfer(SPI4_FLASH, &command, 1, NULL, 0);
}

bool w25q128_init(void)
{
    const uint8_t command = CMD_JEDEC_ID;
    uint8_t id[3];
    // Winbond (EF), then device ID 4018 for the W25Q128JV.
    return spi4_transfer(SPI4_FLASH, &command, 1, id, sizeof id)
           && id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x18;
}

bool w25q128_read(uint32_t address, uint8_t *data, uint16_t length)
{
    uint8_t command[4];
    put_command(command, CMD_READ_DATA, address);
    return spi4_transfer(SPI4_FLASH, command, sizeof command, data, length);
}

bool w25q128_program_page(uint32_t address, const uint8_t *data, uint16_t length)
{
    // Command, address and data have to go out in one chip-select window.
    uint8_t buffer[4 + W25Q128_PAGE_SIZE];
    put_command(buffer, CMD_PAGE_PROGRAM, address);
    memcpy(&buffer[4], data, length);

    return write_enable()
           && spi4_transfer(SPI4_FLASH, buffer, (uint16_t)(4 + length), NULL, 0)
           && wait_until_ready(PROGRAM_TIMEOUT_MS);
}

bool w25q128_erase_sector(uint32_t address)
{
    uint8_t command[4];
    put_command(command, CMD_SECTOR_ERASE, address);
    return write_enable()
           && spi4_transfer(SPI4_FLASH, command, sizeof command, NULL, 0)
           && wait_until_ready(ERASE_TIMEOUT_MS);
}
