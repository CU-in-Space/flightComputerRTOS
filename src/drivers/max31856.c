#include "drivers/max31856.h"

#include <stddef.h>

// Register addresses. Setting bit 7 of the address byte makes it a write.
#define REG_CR0    0x00U
#define REG_CJTH   0x0AU   // CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR follow in order
#define WRITE_BIT  0x80U

// CR0: automatic conversions (CMODE), open-circuit check for probes under 5 kΩ
// (OCFAULT = 01), 60 Hz rejection (bit 0 clear).
#define CR0_VALUE  0x90U
// CR1: one sample per conversion, type K.
#define CR1_VALUE  0x03U

const char *const max31856_fault_names[8] = {
    "open", "over/under voltage", "tc low", "tc high",
    "cj low", "cj high", "tc out of range", "cj out of range",
};

bool max31856_configure(spi4_device_t device)
{
    // The address auto-increments, so one write sets CR0 and then CR1.
    const uint8_t write[] = { REG_CR0 | WRITE_BIT, CR0_VALUE, CR1_VALUE };
    const uint8_t read = REG_CR0;
    uint8_t config[2];


    //write then read back to confirm it was taken and the presesnce of the chip
    return spi4_transfer(device, write, sizeof write, NULL, 0)
           && spi4_transfer(device, &read, 1, config, sizeof config)
           && config[0] == CR0_VALUE && config[1] == CR1_VALUE;
}

bool max31856_read(spi4_device_t device, max31856_reading_t *reading)
{
    const uint8_t address = REG_CJTH;
    uint8_t data[6];
    if (!spi4_transfer(device, &address, 1, data, sizeof data)) {
        return false;
    }

    // Both are two's complement and left-aligned. Shifting the bytes up to the top
    // of a signed int puts the sign bit in the sign position; shifting back down
    // (an arithmetic shift) keeps it.
    // Cold junction: 14 bits in CJTH:CJTL, 1/64 °C each.
    int16_t cold_junction = (int16_t)(((uint16_t)data[0] << 8) | data[1]) >> 2;
    // Hot junction: 19 bits in LTCBH:LTCBM:LTCBL, 1/128 °C each.
    int32_t temp = (int32_t)(((uint32_t)data[2] << 24) | ((uint32_t)data[3] << 16)
                             | ((uint32_t)data[4] << 8)) >> 13;

    reading->cold_junction_c = (float)cold_junction / 64.0f;
    reading->temp_c          = (float)temp / 128.0f;
    reading->faults          = data[5];
    return true;
}
