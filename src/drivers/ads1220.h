#ifndef DRIVERS_ADS1220_H
#define DRIVERS_ADS1220_H

#include <stdbool.h>
#include <stdint.h>

// ADS1220 load-cell ADC, alone on SPI5 (mode 1, 3.75 MHz). Set up for cell A
// (AIN0 - AIN1), gain 128, 1000 samples/s continuous, referenced to the bridge
// excitation, so a reading is the bridge's output as a fraction of its supply.
// DRDY falls on PF0 when each sample is ready.

#define ADS1220_FULL_SCALE  8388608   // 2^23: code at +VREF / gain

// Sets up SPI5 and its mutex. The pins stay high impedance until ads1220_power.
void ads1220_init(void);

// Hands SPI5 and DRDY to the chip while the HAT has power, back to high
// impedance when it doesn't. DRDY's interrupt (EXTI0) is on only while powered.
void ads1220_power(bool hat_powered);

// Resets the chip, writes the configuration, reads it back and starts
// continuous conversions. False if the chip doesn't answer. Call from a task.
bool ads1220_start(void);

// Reads the latest sample as a signed 24-bit code.
bool ads1220_read(int32_t *code);

#endif /* DRIVERS_ADS1220_H */
