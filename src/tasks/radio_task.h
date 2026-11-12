#ifndef TASKS_RADIO_TASK_H
#define TASKS_RADIO_TASK_H

#include <stdbool.h>
#include <stdint.h>

// Owns the radio: the link to the launch box (app/radio_link.h). It listens,
// makes the outputs follow the launch box's STATE packets, answers each packet
// addressed to this stand with telemetry, hands command lines to the remote
// task, and sends every output to its safe state on a SAFE packet or when the
// launch box goes quiet.

typedef struct {
    bool started;        // the SX1262 answered and is listening
    bool linked;         // the launch box has been heard within the timeout
    bool aborted;        // lost the launch box and went safe; cleared by its next STATE without ARMED
    uint32_t received;   // good packets from the launch box
    uint32_t rejected;   // failed the CRC, or not this link's
    uint32_t sent;       // telemetry packets
    uint32_t restarts;   // times the chip had to be set up again
    int8_t rssi_dbm;     // of the last good packet
    int8_t snr_db;
} radio_status_t;

// Starts the SX1262 each time the HAT gets power, then runs the link.
void vRadioTask(void *pvParameters);

// Copies the task's state, for the console.
void radio_status(radio_status_t *copy);

// Transmits one telemetry packet without being asked by the launch box, to
// check the transmitter. False if the radio isn't running.
bool radio_send_test(void);

#endif /* TASKS_RADIO_TASK_H */
