#include "drivers/e22.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "sx126x.h"
#include "sx126x_hal.h"
#include "sx126x_regs.h"

#include "main.h"
#include "app/radio_link.h"
#include "board/board_pins.h"

#define SPI_TIMEOUT_MS   10U
// The longest command is the full calibration with the TCXO starting: about 10 ms.
#define BUSY_TIMEOUT_MS  50U

// SetDIO3AsTcxoCtrl counts in steps of 15.625 us: 5 ms for the TCXO to settle.
#define TCXO_START_STEPS  320U
// The longest packet is on air for under 40 ms.
#define TX_TIMEOUT_MS     200U

// The events DIO1 rises for.
#define DIO1_EVENTS  (SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_HEADER_ERROR \
                      | SX126X_IRQ_CRC_ERROR | SX126X_IRQ_TIMEOUT)

static SPI_HandleTypeDef hspi1;
static SemaphoreHandle_t bus_mutex;
static bool powered;   // only changed with the mutex held

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} pin_t;

static const pin_t bus_pins[] = {
    { SPI1_SCK_GPIO_Port,  SPI1_SCK_Pin },
    { SPI1_MISO_GPIO_Port, SPI1_MISO_Pin },
    { SPI1_MOSI_GPIO_Port, SPI1_MOSI_Pin },
};

void e22_init(void)
{
    // SPI1's kernel clock is PLL1Q (240 MHz) from reset; /64 is 3.75 MHz, the
    // same as the HAT's other buses. The chip allows 16 MHz.
    __HAL_RCC_SPI1_CLK_ENABLE();

    hspi1.Instance                    = SPI1;
    hspi1.Init.Mode                   = SPI_MODE_MASTER;
    hspi1.Init.Direction              = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize               = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity            = SPI_POLARITY_LOW;    // mode 0: clock idles low,
    hspi1.Init.CLKPhase               = SPI_PHASE_1EDGE;     // data read on the rising edge
    hspi1.Init.NSS                    = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler      = SPI_BAUDRATEPRESCALER_64;
    hspi1.Init.FirstBit               = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode                 = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation         = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.NSSPMode               = SPI_NSS_PULSE_DISABLE;
    hspi1.Init.FifoThreshold          = SPI_FIFO_THRESHOLD_01DATA;
    // Same as SPI4: keep the clock pin driven between the send and the read.
    hspi1.Init.MasterKeepIOState      = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    hspi1.Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_ENABLE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        Error_Handler();
    }

    // One task uses the chip, but the interlock switches the pins, so the two
    // must not overlap.
    bus_mutex = xSemaphoreCreateMutex();
    if (bus_mutex == NULL) {
        Error_Handler();
    }

    // Priority 6: the handler wakes the radio task.
    HAL_NVIC_SetPriority(EXTI3_IRQn, 6, 0);
}

void e22_power(bool hat_powered)
{
    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    if (hat_powered) {
        GPIO_InitTypeDef pin_config = {0};
        pin_config.Pull  = GPIO_NOPULL;
        pin_config.Speed = GPIO_SPEED_FREQ_LOW;

        // Levels first: not selected, not in reset, receive amplifier off.
        HAL_GPIO_WritePin(RADIO_NSS_GPIO_Port, RADIO_NSS_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(RADIO_NRST_GPIO_Port, RADIO_NRST_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(RADIO_RXEN_GPIO_Port, RADIO_RXEN_Pin, GPIO_PIN_RESET);
        pin_config.Mode = GPIO_MODE_OUTPUT_PP;
        pin_config.Pin  = RADIO_NSS_Pin;
        HAL_GPIO_Init(RADIO_NSS_GPIO_Port, &pin_config);
        pin_config.Pin  = RADIO_NRST_Pin;
        HAL_GPIO_Init(RADIO_NRST_GPIO_Port, &pin_config);
        pin_config.Pin  = RADIO_RXEN_Pin;
        HAL_GPIO_Init(RADIO_RXEN_GPIO_Port, &pin_config);

        pin_config.Mode      = GPIO_MODE_AF_PP;
        pin_config.Alternate = SPI1_AF;
        for (unsigned i = 0; i < sizeof bus_pins / sizeof bus_pins[0]; i++) {
            pin_config.Pin = bus_pins[i].pin;
            HAL_GPIO_Init(bus_pins[i].port, &pin_config);
        }

        // Pulled up: if BUSY's joint is open it reads busy for ever, and
        // e22_start fails cleanly instead of the chip half working.
        pin_config.Mode      = GPIO_MODE_INPUT;
        pin_config.Pull      = GPIO_PULLUP;
        pin_config.Alternate = 0;
        pin_config.Pin       = RADIO_BUSY_Pin;
        HAL_GPIO_Init(RADIO_BUSY_GPIO_Port, &pin_config);

        pin_config.Mode = GPIO_MODE_IT_RISING;
        pin_config.Pull = GPIO_PULLDOWN;
        pin_config.Pin  = RADIO_DIO1_Pin;
        HAL_GPIO_Init(RADIO_DIO1_GPIO_Port, &pin_config);
        __HAL_GPIO_EXTI_CLEAR_IT(RADIO_DIO1_Pin);   // drop any edge from before
        HAL_NVIC_EnableIRQ(EXTI3_IRQn);
    } else {
        // DeInit returns a pin to its reset state, analog, and clears DIO1's
        // interrupt setup.
        HAL_NVIC_DisableIRQ(EXTI3_IRQn);
        HAL_GPIO_DeInit(RADIO_DIO1_GPIO_Port, RADIO_DIO1_Pin);
        HAL_GPIO_DeInit(RADIO_BUSY_GPIO_Port, RADIO_BUSY_Pin);
        HAL_GPIO_DeInit(RADIO_RXEN_GPIO_Port, RADIO_RXEN_Pin);
        HAL_GPIO_DeInit(RADIO_NRST_GPIO_Port, RADIO_NRST_Pin);
        HAL_GPIO_DeInit(RADIO_NSS_GPIO_Port, RADIO_NSS_Pin);
        for (unsigned i = 0; i < sizeof bus_pins / sizeof bus_pins[0]; i++) {
            HAL_GPIO_DeInit(bus_pins[i].port, bus_pins[i].pin);
        }
    }
    powered = hat_powered;

    xSemaphoreGive(bus_mutex);
}

// BUSY is high while the chip works on the last command. It takes no new one
// until BUSY falls.
static bool wait_until_ready(void)
{
    uint32_t start_ms = HAL_GetTick();
    while (HAL_GPIO_ReadPin(RADIO_BUSY_GPIO_Port, RADIO_BUSY_Pin) == GPIO_PIN_SET) {
        if (HAL_GetTick() - start_ms > BUSY_TIMEOUT_MS) {
            return false;
        }
    }
    return true;
}

// One transaction: wait for BUSY, NSS low, send the command, then send `out` or
// read into `in` (length bytes; either may be NULL), NSS high.
static bool transfer(const uint8_t *command, uint16_t command_length,
                     const uint8_t *out, uint8_t *in, uint16_t length)
{
    // Before taking the mutex: BUSY can last milliseconds, and the interlock
    // must never wait that long for the pins.
    if (!wait_until_ready()) {
        return false;
    }
    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    if (!powered) {
        xSemaphoreGive(bus_mutex);
        return false;
    }

    HAL_GPIO_WritePin(RADIO_NSS_GPIO_Port, RADIO_NSS_Pin, GPIO_PIN_RESET);
    bool ok = HAL_SPI_Transmit(&hspi1, (uint8_t *)command, command_length, SPI_TIMEOUT_MS) == HAL_OK;
    if (ok && out != NULL) {
        ok = HAL_SPI_Transmit(&hspi1, (uint8_t *)out, length, SPI_TIMEOUT_MS) == HAL_OK;
    }
    if (ok && in != NULL) {
        // The chip expects zeros (NOP) while it sends. The same buffer serves both
        // ways: each zero has gone out before the byte that replaces it comes in.
        memset(in, 0, length);
        ok = HAL_SPI_TransmitReceive(&hspi1, in, in, length, SPI_TIMEOUT_MS) == HAL_OK;
    }
    HAL_GPIO_WritePin(RADIO_NSS_GPIO_Port, RADIO_NSS_Pin, GPIO_PIN_SET);

    // BUSY rises up to 600 ns after NSS. Wait a microsecond or two, so the next
    // command's check can't see it still low.
    for (volatile unsigned i = 0; i < 100; i++) {
    }

    xSemaphoreGive(bus_mutex);
    return ok;
}

// ---- What Semtech's driver asks the board for (sx126x_hal.h). It builds each
// command; these put it on the bus.

sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command, const uint16_t command_length,
                                     const uint8_t *data, const uint16_t data_length)
{
    (void)context;
    // Most commands have no data: Semtech passes NULL and 0.
    return transfer(command, command_length, data_length > 0 ? data : NULL, NULL, data_length)
               ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command, const uint16_t command_length,
                                    uint8_t *data, const uint16_t data_length)
{
    (void)context;
    return transfer(command, command_length, NULL, data, data_length)
               ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

// NRST low for 100 us resets the chip. It then needs 3.5 ms before it listens.
sx126x_hal_status_t sx126x_hal_reset(const void *context)
{
    (void)context;
    HAL_GPIO_WritePin(RADIO_NRST_GPIO_Port, RADIO_NRST_Pin, GPIO_PIN_RESET);
    vTaskDelay(2);
    HAL_GPIO_WritePin(RADIO_NRST_GPIO_Port, RADIO_NRST_Pin, GPIO_PIN_SET);
    vTaskDelay(pdMS_TO_TICKS(10));
    return SX126X_HAL_STATUS_OK;
}

// The chip is never put to sleep, so there is nothing to wake.
sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    (void)context;
    return SX126X_HAL_STATUS_OK;
}

// ---- The radio as the rest of the firmware sees it

static bool ok(sx126x_status_t status)
{
    return status == SX126X_STATUS_OK;
}

// Explicit header, CRC on, IQ not inverted. To send, the length is the
// packet's; to receive, the longest packet to accept.
static bool set_packet_length(uint8_t length)
{
    const sx126x_pkt_params_lora_t packet = {
        .preamble_len_in_symb = RADIO_PREAMBLE_SYMBOLS,
        .header_type          = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes     = length,
        .crc_is_on            = true,
        .invert_iq_is_on      = false,
    };
    return ok(sx126x_set_lora_pkt_params(NULL, &packet));
}

// Semtech's code for the bandwidth in radio_link.h.
static sx126x_lora_bw_t bandwidth(void)
{
    switch (RADIO_BANDWIDTH_KHZ) {
    case 125: return SX126X_LORA_BW_125;
    case 250: return SX126X_LORA_BW_250;
    default:  return SX126X_LORA_BW_500;
    }
}

bool e22_start(void)
{
    // The SX1262's full-power settings (datasheet table 13-21). SetTxParams
    // then sets the power.
    const sx126x_pa_cfg_params_t amplifier = {
        .pa_duty_cycle = 0x04,
        .hp_max        = 0x07,
        .device_sel    = 0x00,
        .pa_lut        = 0x01,
    };
    // The spreading factor and coding rate codes are the numbers themselves
    // (7, and 1 for 4/5). Low data rate optimisation is for symbols of 16 ms
    // and longer.
    const sx126x_mod_params_lora_t modulation = {
        .sf   = (sx126x_lora_sf_t)RADIO_SPREADING_FACTOR,
        .bw   = bandwidth(),
        .cr   = (sx126x_lora_cr_t)(RADIO_CODING_RATE - 4U),
        .ldro = (1U << RADIO_SPREADING_FACTOR) / RADIO_BANDWIDTH_KHZ >= 16,
    };
    // The registers hold the sync word's two digits as 0xX4 0xY4.
    const uint8_t sync_word[] = { (RADIO_SYNC_WORD & 0xF0) | 0x04, (uint8_t)(RADIO_SYNC_WORD << 4) | 0x04 };
    uint8_t readback[sizeof sync_word];

    // In the datasheet's order (14.5): packet type, then modulation, then packet
    // settings. Semtech's driver adds the datasheet's section 15 fixes by itself
    // (the modulation at 500 kHz, inside sx126x_set_lora_mod_params), except the
    // amplifier one, which is sx126x_cfg_tx_clamp here.
    return ok(sx126x_reset(NULL))
           && ok(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC))
           // The module's 32 MHz TCXO is powered from DIO3, at 2.2 V (Ebyte manual
           // 4.2). The chip calibrated itself at reset, before it knew that, so
           // that failed: calibrate again and clear the error it left.
           && ok(sx126x_set_dio3_as_tcxo_ctrl(NULL, SX126X_TCXO_CTRL_2_2V, TCXO_START_STEPS))
           && ok(sx126x_cal(NULL, SX126X_CAL_ALL))
           && ok(sx126x_clear_device_errors(NULL))
           // DIO2 follows the transmitter, and the HAT wires it to the module's TXEN.
           && ok(sx126x_set_dio2_as_rf_sw_ctrl(NULL, true))
           // After a packet, stay on the crystal instead of the RC oscillator, so
           // the TCXO needn't start again (5 ms) for the next one.
           && ok(sx126x_set_rx_tx_fallback_mode(NULL, SX126X_FALLBACK_STDBY_XOSC))
           && ok(sx126x_set_pkt_type(NULL, SX126X_PKT_TYPE_LORA))
           && ok(sx126x_set_rf_freq(NULL, RADIO_FREQUENCY_HZ))
           // Image calibration for 902-928 MHz (datasheet table 9-2).
           && ok(sx126x_cal_img(NULL, 0xE1, 0xE9))
           && ok(sx126x_set_pa_cfg(NULL, &amplifier))
           // Stops the amplifier backing off into a slightly mismatched antenna.
           && ok(sx126x_cfg_tx_clamp(NULL))
           && ok(sx126x_set_tx_params(NULL, E22_TX_POWER_DBM, SX126X_RAMP_200_US))
           && ok(sx126x_set_buffer_base_address(NULL, 0, 0))
           && ok(sx126x_set_lora_mod_params(NULL, &modulation))
           && set_packet_length(255)
           && ok(sx126x_set_lora_sync_word(NULL, RADIO_SYNC_WORD))
           // Which events are recorded, and that DIO1 shows them all. DIO2 and DIO3: none.
           && ok(sx126x_set_dio_irq_params(NULL, DIO1_EVENTS, DIO1_EVENTS, SX126X_IRQ_NONE, SX126X_IRQ_NONE))
           // A chip that's missing or not soldered won't echo the sync word back.
           && ok(sx126x_read_register(NULL, SX126X_REG_LR_SYNCWORD, readback, sizeof readback))
           && memcmp(readback, sync_word, sizeof sync_word) == 0;
}

bool e22_receive(void)
{
    // Packet settings only change in standby.
    if (!ok(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_XOSC)) || !set_packet_length(255)) {
        return false;
    }
    HAL_GPIO_WritePin(RADIO_RXEN_GPIO_Port, RADIO_RXEN_Pin, GPIO_PIN_SET);
    // Continuous: never time out, keep listening after each packet.
    return ok(sx126x_set_rx_with_timeout_in_rtc_step(NULL, SX126X_RX_CONTINUOUS));
}

bool e22_send(const uint8_t *data, uint8_t length)
{
    // The module's RF switch must never see RXEN and TXEN high together, and
    // DIO2 raises TXEN when the transmission starts.
    HAL_GPIO_WritePin(RADIO_RXEN_GPIO_Port, RADIO_RXEN_Pin, GPIO_PIN_RESET);
    // DIO1 stays high while any event is set. Clearing them all, once standby
    // has stopped the receiver, makes sure it's low and rises for this packet.
    return ok(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_XOSC))
           && ok(sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL))
           && set_packet_length(length)
           && ok(sx126x_write_buffer(NULL, 0, data, length))
           && ok(sx126x_set_tx(NULL, TX_TIMEOUT_MS));
}

bool e22_read_packet(uint8_t *data, uint8_t size, uint8_t *length,
                     int8_t *rssi_dbm, int8_t *snr_db)
{
    sx126x_rx_buffer_status_t buffer;   // the packet's length, and where it starts in the chip's buffer
    sx126x_pkt_status_lora_t signal;

    if (!ok(sx126x_get_rx_buffer_status(NULL, &buffer))
        || buffer.pld_len_in_bytes == 0 || buffer.pld_len_in_bytes > size
        || !ok(sx126x_read_buffer(NULL, buffer.buffer_start_pointer, data, buffer.pld_len_in_bytes))
        || !ok(sx126x_get_lora_pkt_status(NULL, &signal))) {
        return false;
    }

    *length = buffer.pld_len_in_bytes;
    *rssi_dbm = signal.rssi_pkt_in_dbm;
    *snr_db = signal.snr_pkt_in_db;
    return true;
}

bool e22_listening(void)
{
    sx126x_chip_status_t status;
    return ok(sx126x_get_status(NULL, &status)) && status.chip_mode == SX126X_CHIP_MODE_RX;
}
