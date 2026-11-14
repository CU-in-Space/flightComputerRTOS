#ifndef TASKS_RADIO_TASK_H
#define TASKS_RADIO_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Owns the radio: the link to the launch box (app/radio_link.h). It listens,
// makes the outputs follow the launch box's STATE packets, answers each packet
// addressed to this stand with telemetry, hands command lines to the remote
// task, and sends every output to its safe state on a SAFE packet or when the
// launch box goes quiet.

typedef struct {
    bool started;           // the SX1262 answered and is listening
    bool linked;            // the launch box has been heard within the timeout
    bool aborted;           // lost the launch box and went safe; cleared by its next STATE without ARMED
    uint32_t received;      // good packets from the launch box
    uint32_t damaged;       // failed the CRC or the header check: too weak, or two radios at once
    uint32_t other_version; // this link's network, another protocol version: radio_link.h not copied across
    uint32_t rejected;      // whole, but not this link's: another network, or a wrong length, type or sender
    uint32_t sent;          // telemetry packets
    uint32_t restarts;      // times the chip had to be set up again
    uint32_t box_heard_ms;  // when the last good packet from the launch box came, ms since boot
    int8_t rssi_dbm;        // of the last good packet from the launch box
    int8_t snr_db;
    bool state_heard;       // a STATE has come since the board started
    uint8_t state_valves;   // the last one's valves and flags
    uint8_t state_flags;
    uint8_t state_count;    // the launch box's count in it
    uint32_t state_heard_ms; // when it came
} radio_status_t;

// Starts the SX1262 each time the HAT gets power, then runs the link.
void vRadioTask(void *pvParameters);

// Copies the task's state, for the console.
void radio_status(radio_status_t *copy);

// Transmits one telemetry packet without being asked by the launch box, to
// check the transmitter. False if the radio isn't running.
bool radio_send_test(void);

// While on, the task prints a line for every packet it hears or fails to send.
void radio_watch(bool on);

// A STATE's valves and flags as text: "valves 0001010, armed". The valves are
// in the launch box's switch order, as its own serial monitor prints them.
void radio_state_text(uint8_t valves, uint8_t flags, char *text, size_t size);

#endif /* TASKS_RADIO_TASK_H */
