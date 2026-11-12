#ifndef DRIVERS_E22_H
#define DRIVERS_E22_H

#include <stdbool.h>
#include <stdint.h>

// E22-900M30S radio module, alone on SPI1 (mode 0, 3.75 MHz). Inside it is an
// SX1262, driven by Semtech's driver in lib/sx126x_driver. This file is what
// that driver can't know: the HAT's pins, the HAT's power, and the module
// around the chip (a 1 W amplifier and a receive amplifier behind an RF switch,
// and a TCXO). The chip's DIO2 turns the transmit side on by itself; RXEN is
// switched here. DIO1 rises on PF3 when a packet has been sent or received.
// The LoRa settings come from app/radio_link.h.
//
// Semtech's functions take a "context" first. There is one radio, so it's
// always NULL.

// SetTxParams power at the SX1262, -9 to +22 dBm. The module's amplifier adds
// about 10 dB at 915 MHz (Ebyte manual, chart 2): 20 is the full 30 dBm (1 W),
// 16 about 26 dBm, and -9 a milliwatt or so, for the bench. Above that, never
// transmit without an antenna or with the two antennas a few metres apart.
#define E22_TX_POWER_DBM  (-9)

// Sets up SPI1 and its mutex. The pins stay high impedance until e22_power.
void e22_init(void);

// Hands SPI1 and the control pins to the module while the HAT has power, back
// to high impedance when it doesn't. DIO1's interrupt (EXTI3) is on only while
// powered.
void e22_power(bool hat_powered);

// Resets the chip and writes the whole configuration. False if the chip doesn't
// answer. Leaves it in standby: call e22_receive next. Call from a task.
bool e22_start(void);

// Listens until told otherwise. Each packet raises DIO1.
bool e22_receive(void);

// Stops listening and transmits one packet. DIO1 rises when it's out; call
// e22_receive again after that.
bool e22_send(const uint8_t *data, uint8_t length);

// Copies out the packet just received, with its signal strength and
// signal-to-noise ratio. False if it's empty or longer than size.
bool e22_read_packet(uint8_t *data, uint8_t size, uint8_t *length,
                     int8_t *rssi_dbm, int8_t *snr_db);

// True if the chip answers and is in receive mode. False after it has reset
// itself, for example on a supply dip.
bool e22_listening(void);

#endif /* DRIVERS_E22_H */
