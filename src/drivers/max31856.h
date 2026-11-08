#ifndef DRIVERS_MAX31856_H
#define DRIVERS_MAX31856_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/spi4_bus.h"

// MAX31856 thermocouple converter. Set up for type K with 60 Hz rejection,
// converting by itself every ~100 ms, so a read just takes the latest result.

typedef struct {
    float temp_c;           // hot junction: linearised, cold junction already added
    float cold_junction_c;  // the chip's own temperature
    uint8_t faults;         // fault status register; 0 = none. Bits named below
} max31856_reading_t;

// Name of each fault bit, indexed by bit number (0 = open circuit).
extern const char *const max31856_fault_names[8];

// Writes the configuration and reads it back. False if the chip doesn't answer.
bool max31856_configure(spi4_device_t device);

// Reads the latest temperatures and fault bits.
bool max31856_read(spi4_device_t device, max31856_reading_t *reading);

#endif /* DRIVERS_MAX31856_H */
