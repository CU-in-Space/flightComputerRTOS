#include "drivers/w25q128.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "main.h"
#include "drivers/spi4_bus.h"

#define CMD_WRITE_ENABLE  0x06U
#define CMD_READ_STATUS   0x05U
#define CMD_READ_DATA     0x03U
#define CMD_PAGE_PROGRAM  0x02U
#define CMD_SECTOR_ERASE  0x20U
#define CMD_BLOCK_ERASE   0xD8U
#define CMD_JEDEC_ID      0x9FU

#define STATUS_BUSY       0x01U

// Datasheet maximums, rounded up.
#define PROGRAM_TIMEOUT_MS  5U
#define SECTOR_TIMEOUT_MS   500U
#define BLOCK_TIMEOUT_MS    2500U

static SemaphoreHandle_t chip_mutex;

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

void w25q128_init(void)
{
    chip_mutex = xSemaphoreCreateMutex();
    if (chip_mutex == NULL) {
        Error_Handler();
    }
}

bool w25q128_present(void)
{
    const uint8_t command = CMD_JEDEC_ID;
    uint8_t id[3];
    xSemaphoreTake(chip_mutex, portMAX_DELAY);
    bool ok = spi4_transfer(SPI4_FLASH, &command, 1, id, sizeof id);
    xSemaphoreGive(chip_mutex);
    // Winbond (EF), then device ID 4018 for the W25Q128JV.
    return ok && id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x18;
}

bool w25q128_read(uint32_t address, uint8_t *data, uint16_t length)
{
    uint8_t command[4];
    put_command(command, CMD_READ_DATA, address);
    xSemaphoreTake(chip_mutex, portMAX_DELAY);
    bool ok = spi4_transfer(SPI4_FLASH, command, sizeof command, data, length);
    xSemaphoreGive(chip_mutex);
    return ok;
}

bool w25q128_program_page(uint32_t address, const uint8_t *data, uint16_t length)
{
    // Command, address and data have to go out in one chip-select window.
    uint8_t buffer[4 + W25Q128_PAGE_SIZE];
    put_command(buffer, CMD_PAGE_PROGRAM, address);
    memcpy(&buffer[4], data, length);

    xSemaphoreTake(chip_mutex, portMAX_DELAY);
    bool ok = write_enable()
              && spi4_transfer(SPI4_FLASH, buffer, (uint16_t)(4 + length), NULL, 0)
              && wait_until_ready(PROGRAM_TIMEOUT_MS);
    xSemaphoreGive(chip_mutex);
    return ok;
}

static bool erase(uint8_t opcode, uint32_t address, uint32_t timeout_ms)
{
    uint8_t command[4];
    put_command(command, opcode, address);
    xSemaphoreTake(chip_mutex, portMAX_DELAY);
    bool ok = write_enable()
              && spi4_transfer(SPI4_FLASH, command, sizeof command, NULL, 0)
              && wait_until_ready(timeout_ms);
    xSemaphoreGive(chip_mutex);
    return ok;
}

bool w25q128_erase_sector(uint32_t address)
{
    return erase(CMD_SECTOR_ERASE, address, SECTOR_TIMEOUT_MS);
}

bool w25q128_erase_block(uint32_t address)
{
    return erase(CMD_BLOCK_ERASE, address, BLOCK_TIMEOUT_MS);
}
