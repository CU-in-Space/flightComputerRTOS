#include "drivers/sd_card.h"

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "board/board_pins.h"

#include "diskio.h"

// SDMMC1's kernel clock is PLL1Q, 240 MHz from reset. SDMMC_CK = 240 / (2 x 5)
// = 24 MHz, under the 25 MHz of an SD card's default speed.
#define CLOCK_DIV         5U
// A card may stay busy up to 250 ms after each block it writes (500 ms for SDXC).
#define BUSY_MS           500U
// A transfer of many blocks can meet several of those. The ring buffer in the
// datalog task rides out about 4 s.
#define TRANSFER_MS       2000U

static SD_HandleTypeDef hsd1;
static volatile bool initialized;   // cleared when the HAT loses power

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} pin_t;

static const pin_t pins[] = {
    { SD_D0_GPIO_Port,  SD_D0_Pin },
    { SD_D1_GPIO_Port,  SD_D1_Pin },
    { SD_D2_GPIO_Port,  SD_D2_Pin },
    { SD_D3_GPIO_Port,  SD_D3_Pin },
    { SD_CK_GPIO_Port,  SD_CK_Pin },
    { SD_CMD_GPIO_Port, SD_CMD_Pin },
};

void sd_card_init(void)
{
    __HAL_RCC_SDMMC1_CLK_ENABLE();
}

// No mutex, unlike SPI4: an SD write can keep the card busy for hundreds of ms,
// and the interlock must never wait that long. If the pins go away mid-transfer,
// the transfer times out and FatFs reports an error.
void sd_card_power(bool hat_powered)
{
    if (hat_powered) {
        // CMD and D0-D3 have 10k pull-ups on the HAT.
        GPIO_InitTypeDef pin_config = {0};
        pin_config.Mode      = GPIO_MODE_AF_PP;
        pin_config.Pull      = GPIO_NOPULL;
        pin_config.Speed     = GPIO_SPEED_FREQ_HIGH;
        pin_config.Alternate = SD_AF;
        for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) {
            pin_config.Pin = pins[i].pin;
            HAL_GPIO_Init(pins[i].port, &pin_config);
        }
    } else {
        initialized = false;
        for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) {
            HAL_GPIO_DeInit(pins[i].port, pins[i].pin);
        }
    }
}

// After a transfer the card keeps programming its flash. It's ready again once
// it's back in the transfer state.
static bool wait_until_ready(void)
{
    TickType_t start = xTaskGetTickCount();
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(BUSY_MS)) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

// ------------------------------------------------- FatFs disk functions

// f_mount calls this. It starts the card from scratch every time, so a card
// that was swapped or lost power works again.
DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;

    initialized = false;
    __HAL_RCC_SDMMC1_FORCE_RESET();
    __HAL_RCC_SDMMC1_RELEASE_RESET();

    // A fresh handle too: the HAL never clears an old error code itself, and a
    // leftover one fails the next init.
    hsd1 = (SD_HandleTypeDef){
        .Instance = SDMMC1,
        .Init = {
            .ClockEdge           = SDMMC_CLOCK_EDGE_RISING,
            .ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE,
            .BusWide             = SDMMC_BUS_WIDE_4B,
            // Stops the clock when the FIFO runs empty or full, so a task switch
            // in the middle of a transfer can't under- or overrun it.
            .HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE,
            .ClockDiv            = CLOCK_DIV,
        },
    };

    // HAL_SD_Init identifies the card at 400 kHz, then switches to 4 bits at 24 MHz.
    if (HAL_SD_Init(&hsd1) != HAL_OK) {
        return STA_NOINIT;
    }
    initialized = true;
    return 0;
}

// FatFs checks this before every operation.
DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return initialized ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (HAL_SD_ReadBlocks(&hsd1, buff, sector, count, TRANSFER_MS) != HAL_OK || !wait_until_ready()) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (HAL_SD_WriteBlocks(&hsd1, buff, sector, count, TRANSFER_MS) != HAL_OK || !wait_until_ready()) {
        return RES_ERROR;
    }
    return RES_OK;
}

// Each write already waits for the card, so a sync has nothing left to wait for.
// FatFs asks for nothing else with this configuration.
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    (void)buff;
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
