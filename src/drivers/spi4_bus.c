#include "drivers/spi4_bus.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include "main.h"
#include "board/board_pins.h"

#define TIMEOUT_MS  200U   // a whole transfer, so long enough for the longest flash read (64 KB)

static SPI_HandleTypeDef hspi4;
static SemaphoreHandle_t bus_mutex;
static bool powered;   // only changed with the mutex held

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} pin_t;

static const pin_t chip_selects[SPI4_DEVICE_COUNT] = {
    [SPI4_TC1]   = { TC1_CS_GPIO_Port,   TC1_CS_Pin },
    [SPI4_TC2]   = { TC2_CS_GPIO_Port,   TC2_CS_Pin },
    [SPI4_TC3]   = { TC3_CS_GPIO_Port,   TC3_CS_Pin },
    [SPI4_TC4]   = { TC4_CS_GPIO_Port,   TC4_CS_Pin },
    [SPI4_FLASH] = { FLASH_CS_GPIO_Port, FLASH_CS_Pin },
};

static const pin_t bus_pins[] = {
    { SPI4_SCK_GPIO_Port,  SPI4_SCK_Pin },
    { SPI4_MISO_GPIO_Port, SPI4_MISO_Pin },
    { SPI4_MOSI_GPIO_Port, SPI4_MOSI_Pin },
};

void spi4_bus_init(void)
{
    // SPI4's kernel clock is PCLK2 (120 MHz) from reset; /32 is 3.75 MHz, under
    // the MAX31856's 5 MHz.
    __HAL_RCC_SPI4_CLK_ENABLE();

    hspi4.Instance               = SPI4;
    hspi4.Init.Mode              = SPI_MODE_MASTER;
    hspi4.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi4.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi4.Init.CLKPolarity       = SPI_POLARITY_HIGH;   // mode 3: clock idles high,
    hspi4.Init.CLKPhase          = SPI_PHASE_2EDGE;     // data read on the rising edge
    hspi4.Init.NSS               = SPI_NSS_SOFT;        // chip selects are plain GPIO
    hspi4.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
    hspi4.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi4.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi4.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi4.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;
    hspi4.Init.FifoThreshold     = SPI_FIFO_THRESHOLD_01DATA;
    // The HAL switches SPI4 off between calls. Without this the clock pin would
    // float then, in the middle of a transaction that sends and then reads.
    hspi4.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    // When only reading, the clock would otherwise run on whether or not anything
    // empties the 8-byte receive FIFO. If the task is interrupted for 17 µs mid-read,
    // bytes would be lost. This pauses the clock while the FIFO is full.
    hspi4.Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_ENABLE;
    if (HAL_SPI_Init(&hspi4) != HAL_OK) {
        Error_Handler();
    }

    // A mutex rather than a plain semaphore: a low-priority task holding the bus
    // is lifted to the priority of the task waiting for it, so it finishes quickly.
    bus_mutex = xSemaphoreCreateMutex();
    if (bus_mutex == NULL) {
        Error_Handler();
    }
}

void spi4_bus_power(bool hat_powered)
{
    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    GPIO_InitTypeDef pin_config = {0};
    pin_config.Pull  = GPIO_NOPULL;
    pin_config.Speed = GPIO_SPEED_FREQ_LOW;   // soft edges on a long shared bus

    if (hat_powered) {
        // Chip selects first, high (deselected), then the bus.
        pin_config.Mode = GPIO_MODE_OUTPUT_PP;
        for (unsigned i = 0; i < SPI4_DEVICE_COUNT; i++) {
            HAL_GPIO_WritePin(chip_selects[i].port, chip_selects[i].pin, GPIO_PIN_SET);
            pin_config.Pin = chip_selects[i].pin;
            HAL_GPIO_Init(chip_selects[i].port, &pin_config);
        }
    } else {
        // Analog is the STM32's true high impedance: no output, no input buffer.
        pin_config.Mode = GPIO_MODE_ANALOG;
        for (unsigned i = 0; i < SPI4_DEVICE_COUNT; i++) {
            pin_config.Pin = chip_selects[i].pin;
            HAL_GPIO_Init(chip_selects[i].port, &pin_config);
        }
    }
    //configure bus pins
    pin_config.Mode      = GPIO_MODE_AF_PP;
    pin_config.Alternate = SPI4_AF;
    for (unsigned i = 0; i < sizeof bus_pins / sizeof bus_pins[0]; i++) {
        pin_config.Pin = bus_pins[i].pin;
        HAL_GPIO_Init(bus_pins[i].port, &pin_config);
    }
    powered = hat_powered;

    xSemaphoreGive(bus_mutex);
}

bool spi4_transfer(spi4_device_t device, const uint8_t *tx, uint16_t tx_length,
                   uint8_t *rx, uint16_t rx_length)
{
    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    if (!powered) {
        xSemaphoreGive(bus_mutex);
        return false;
    }

    // The HAL's transmit takes a non-const pointer but only reads from it.
    HAL_GPIO_WritePin(chip_selects[device].port, chip_selects[device].pin, GPIO_PIN_RESET);
    bool ok = HAL_SPI_Transmit(&hspi4, (uint8_t *)tx, tx_length, TIMEOUT_MS) == HAL_OK;
    if (ok && rx_length > 0) {
        ok = HAL_SPI_Receive(&hspi4, rx, rx_length, TIMEOUT_MS) == HAL_OK;
    }
    HAL_GPIO_WritePin(chip_selects[device].port, chip_selects[device].pin, GPIO_PIN_SET);

    xSemaphoreGive(bus_mutex);
    return ok;
}
