#ifndef DRIVERS_SPI4_BUS_H
#define DRIVERS_SPI4_BUS_H

#include <stdbool.h>
#include <stdint.h>

// SPI4: the four MAX31856 and the W25Q128 share it, each with its own chip select.
// Mode 3, 3.75 MHz for all of them. A mutex makes each transfer whole, so tasks
// can share the bus without taking turns by hand.

typedef enum {
    SPI4_TC1,
    SPI4_TC2,
    SPI4_TC3,
    SPI4_TC4,
    SPI4_FLASH,
    SPI4_DEVICE_COUNT,
} spi4_device_t;

// Sets up SPI4 and the mutex. The pins stay high impedance until spi4_bus_power.
void spi4_bus_init(void);

// Hands the pins to SPI4 (chip selects high) when the HAT has power, and back to
// high impedance when it doesn't, so nothing powers its chips through their pins.
void spi4_bus_power(bool hat_powered);

// One transaction with one device: chip select low, send tx, then read rx_length
// bytes into rx, chip select high. Waits for the bus if another task has it.
// False if the HAT has no power or the transfer failed.
bool spi4_transfer(spi4_device_t device, const uint8_t *tx, uint16_t tx_length,
                   uint8_t *rx, uint16_t rx_length);

#endif /* DRIVERS_SPI4_BUS_H */
