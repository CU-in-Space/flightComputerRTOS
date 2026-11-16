#include "drivers/ads1220.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "main.h"
#include "board/board_pins.h"

#define TIMEOUT_MS  10U

#define CMD_RESET   0x06U
#define CMD_START   0x08U
#define CMD_RDATA   0x10U
#define CMD_RREG    0x23U   // read 4 registers from register 0
#define CMD_WREG    0x43U   // write 4 registers from register 0
#define CMD_WREG0   0x40U   // write register 0 only

// Register 0, gain 128, for each cell.
#define CELL_A      0x0EU   // AINP = AIN0, AINN = AIN1
#define CELL_B      0x5EU   // AINP = AIN2, AINN = AIN3

// Register 0: AINP = AIN0, AINN = AIN1 (cell A), gain 128.
// Register 1: 1000 SPS, normal mode, continuous conversion.
// Register 2: reference REFP0 - REFN0 (the excitation), no 50/60 Hz filter
//             (it only works at 20 SPS), no current sources.
// Register 3: defaults: IDACs off, DRDY on its own pin.
static const uint8_t config[4] = { CELL_A, 0xC4, 0x40, 0x00 };

static SPI_HandleTypeDef hspi5;
static SemaphoreHandle_t bus_mutex;
static bool powered;   // only changed with the mutex held

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} pin_t;

static const pin_t bus_pins[] = {
    { SPI5_SCK_GPIO_Port,  SPI5_SCK_Pin },
    { SPI5_MISO_GPIO_Port, SPI5_MISO_Pin },
    { SPI5_MOSI_GPIO_Port, SPI5_MOSI_Pin },
};

void ads1220_init(void)
{
    // SPI5's kernel clock is PCLK2 (120 MHz) from reset, like SPI4: /32 is 3.75 MHz.
    __HAL_RCC_SPI5_CLK_ENABLE();

    hspi5.Instance                    = SPI5;
    hspi5.Init.Mode                   = SPI_MODE_MASTER;
    hspi5.Init.Direction              = SPI_DIRECTION_2LINES;
    hspi5.Init.DataSize               = SPI_DATASIZE_8BIT;
    hspi5.Init.CLKPolarity            = SPI_POLARITY_LOW;    // mode 1: clock idles low,
    hspi5.Init.CLKPhase               = SPI_PHASE_2EDGE;     // data read on the falling edge
    hspi5.Init.NSS                    = SPI_NSS_SOFT;
    hspi5.Init.BaudRatePrescaler      = SPI_BAUDRATEPRESCALER_32;
    hspi5.Init.FirstBit               = SPI_FIRSTBIT_MSB;
    hspi5.Init.TIMode                 = SPI_TIMODE_DISABLE;
    hspi5.Init.CRCCalculation         = SPI_CRCCALCULATION_DISABLE;
    hspi5.Init.NSSPMode               = SPI_NSS_PULSE_DISABLE;
    hspi5.Init.FifoThreshold          = SPI_FIFO_THRESHOLD_01DATA;
    // Same as SPI4: keep the clock pin driven between the send and the read.
    hspi5.Init.MasterKeepIOState      = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    hspi5.Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_ENABLE;
    if (HAL_SPI_Init(&hspi5) != HAL_OK) {
        Error_Handler();
    }

    // One task reads the chip, but the interlock switches the pins, so the two
    // must not overlap.
    bus_mutex = xSemaphoreCreateMutex();
    if (bus_mutex == NULL) {
        Error_Handler();
    }

    // Priority 6: the handler wakes the load-cell task.
    HAL_NVIC_SetPriority(EXTI0_IRQn, 6, 0);
}

void ads1220_power(bool hat_powered)
{
    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    if (hat_powered) {
        GPIO_InitTypeDef pin_config = {0};
        pin_config.Pull  = GPIO_NOPULL;
        pin_config.Speed = GPIO_SPEED_FREQ_LOW;

        // set the pin before enabling it as an output
        HAL_GPIO_WritePin(LC_CS_GPIO_Port, LC_CS_Pin, GPIO_PIN_SET);
        pin_config.Mode = GPIO_MODE_OUTPUT_PP;
        pin_config.Pin  = LC_CS_Pin;
        HAL_GPIO_Init(LC_CS_GPIO_Port, &pin_config);

        pin_config.Mode      = GPIO_MODE_AF_PP;
        pin_config.Alternate = SPI5_AF;
        for (unsigned i = 0; i < sizeof bus_pins / sizeof bus_pins[0]; i++) {
            pin_config.Pin = bus_pins[i].pin;
            HAL_GPIO_Init(bus_pins[i].port, &pin_config);
        }

        pin_config.Mode      = GPIO_MODE_IT_FALLING;
        pin_config.Alternate = 0;
        pin_config.Pin       = LC_DRDY_Pin;
        HAL_GPIO_Init(LC_DRDY_GPIO_Port, &pin_config);
        __HAL_GPIO_EXTI_CLEAR_IT(LC_DRDY_Pin);   // drop any edge from before
        HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    } else {
        // DeInit returns a pin to its reset state, analog, and also clears its
        // interrupt setup, which DRDY needs.
        HAL_NVIC_DisableIRQ(EXTI0_IRQn);
        HAL_GPIO_DeInit(LC_DRDY_GPIO_Port, LC_DRDY_Pin);
        HAL_GPIO_DeInit(LC_CS_GPIO_Port, LC_CS_Pin);
        for (unsigned i = 0; i < sizeof bus_pins / sizeof bus_pins[0]; i++) {
            HAL_GPIO_DeInit(bus_pins[i].port, bus_pins[i].pin);
        }
    }
    powered = hat_powered;

    xSemaphoreGive(bus_mutex);
}

// One transaction: chip select low, send tx, read rx_length bytes, chip select high.
static bool transfer(const uint8_t *tx, uint16_t tx_length, uint8_t *rx, uint16_t rx_length)
{
    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    if (!powered) {
        xSemaphoreGive(bus_mutex);
        return false;
    }

    // On the bench the ADS1220 reset whenever a task broke in with chip select low,
    // so nothing may. A read takes 15 µs. TIM6 keeps running for the HAL timeouts.
    taskENTER_CRITICAL();
    HAL_GPIO_WritePin(LC_CS_GPIO_Port, LC_CS_Pin, GPIO_PIN_RESET);
    bool ok = HAL_SPI_Transmit(&hspi5, (uint8_t *)tx, tx_length, TIMEOUT_MS) == HAL_OK;
    if (ok && rx_length > 0) {
        // Clocks out zeros so DIN stays driven low while data comes back.
        static const uint8_t zeros[4] = {0};
        ok = HAL_SPI_TransmitReceive(&hspi5, (uint8_t *)zeros, rx, rx_length, TIMEOUT_MS) == HAL_OK;
    }
    HAL_GPIO_WritePin(LC_CS_GPIO_Port, LC_CS_Pin, GPIO_PIN_SET);
    taskEXIT_CRITICAL();

    xSemaphoreGive(bus_mutex);
    return ok;
}

bool ads1220_start(void)
{
    const uint8_t reset = CMD_RESET;
    const uint8_t start = CMD_START;
    const uint8_t read_config = CMD_RREG;
    uint8_t write_config[1 + sizeof config] = { CMD_WREG };
    memcpy(&write_config[1], config, sizeof config);
    uint8_t readback[sizeof config];

    if (!transfer(&reset, 1, NULL, 0)) {
        return false;
    }
    // The reset takes about 60 µs. vTaskDelay(2) waits at least 1 ms.
    vTaskDelay(2);

    // A chip that's missing or not soldered won't echo the configuration back.
    // START then begins the continuous conversions.
    return transfer(write_config, sizeof write_config, NULL, 0)
           && transfer(&read_config, 1, readback, sizeof readback)
           && memcmp(readback, config, sizeof config) == 0
           && transfer(&start, 1, NULL, 0);
}

bool ads1220_read(int32_t *code)
{
    const uint8_t command = CMD_RDATA;
    uint8_t data[3];
    if (!transfer(&command, 1, data, sizeof data)) {
        return false;
    }

    // 24-bit two's complement, most significant byte first. Shifting it to the
    // top of a signed int and back keeps the sign, same as the MAX31856.
    *code = (int32_t)(((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                      | ((uint32_t)data[2] << 8)) >> 8;
    return true;
}

bool ads1220_select(unsigned cell)
{
    const uint8_t command[2] = { CMD_WREG0, cell == 0 ? CELL_A : CELL_B };
    return transfer(command, sizeof command, NULL, 0);
}
