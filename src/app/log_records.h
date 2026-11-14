#ifndef APP_LOG_RECORDS_H
#define APP_LOG_RECORDS_H

#include <stdint.h>

/* The data log's file format: a stream of records, each a header and then its
 * payload, little-endian, no padding. tools/hat_log.py reads it and must change
 * with it; so must LOG_FORMAT_VERSION. */

#define LOG_MAGIC           "HATL"
#define LOG_FORMAT_VERSION  5U   // 2: a second load cell, so 16 names and two calibrations. 3: RECORD_RADIO. 4: rail lows and servo targets in RECORD_ACTUATORS.
                                 // 5: RECORD_HEALTH, RECORD_CONSOLE, inputs and solenoid targets in RECORD_ACTUATORS, the STATE count in RECORD_RADIO

typedef enum {
    RECORD_FILE_START = 1,   // first record of every file
    RECORD_PT,               // 10 ms of raw PT samples
    RECORD_LC,               // 10 samples of raw codes from the first load cell
    RECORD_TC,               // all four thermocouples, every 100 ms
    RECORD_ACTUATORS,        // interlock, VBAT and every output, every 10 ms
    RECORD_LC_CALIBRATION,   // after lc tare or lc cal on the first load cell
    RECORD_TEXT,             // a console command line as typed, or a note from the firmware
    RECORD_NAMES,            // every channel's name, straight after the file start
    RECORD_LC2,              // one raw code (an int32_t) from the second load cell, about 10 a second
    RECORD_LC2_CALIBRATION,  // after lc tare or lc cal on the second load cell
    RECORD_RADIO,            // the radio's counts and the launch box's last STATE, every second
    RECORD_HEALTH,           // record_health_t, every second
    RECORD_CONSOLE,          // what printf printed: one or more lines, as text
} record_type_t;

typedef struct __attribute__((packed)) {
    uint8_t type;       // record_type_t
    uint8_t length;     // payload bytes that follow
    uint32_t time_ms;   // when the record was written, ms since boot
} record_header_t;

// Same layout as pt_calibration_t.
typedef struct __attribute__((packed)) {
    float ratio_at_zero;
    float ratio_at_full;
    float full_scale_psi;
} record_pt_calibration_t;

typedef struct __attribute__((packed)) {
    int32_t zero_code;
    float newtons_per_code;
} record_lc_calibration_t;

typedef struct __attribute__((packed)) {
    char magic[4];         // LOG_MAGIC, no terminator
    uint16_t version;      // LOG_FORMAT_VERSION
    record_pt_calibration_t pt[2];
    record_lc_calibration_t lc;    // the first load cell's
    record_lc_calibration_t lc2;   // the second's
} record_file_start_t;

// Samples 1 ms apart, oldest first; the last was taken just before time_ms.
// Raw ADC counts as pt_adc gives them: per sample PT1 supply, PT1 output, PT2
// supply, PT2 output.
typedef struct __attribute__((packed)) {
    uint16_t counts[10][2][2];
} record_pt_t;

// The first load cell's codes, 1 ms apart (the ADS1220's own clock), oldest
// first. After every tenth record the chip takes one sample from the second
// load cell (a RECORD_LC2), so the next record starts 1 ms later than it would.
typedef struct __attribute__((packed)) {
    int32_t codes[10];
} record_lc_t;

typedef struct __attribute__((packed)) {
    uint8_t present;          // 0: no reading, the rest is 0
    uint8_t faults;           // MAX31856 fault status register
    float temp_c;
    float cold_junction_c;
} record_tc_channel_t;

typedef struct __attribute__((packed)) {
    record_tc_channel_t tc[4];
} record_tc_t;

typedef struct __attribute__((packed)) {
    uint8_t interlock;        // interlock_state_t
    uint32_t vbat_mv;         // the interlock's reading
    uint8_t solenoids;        // bit n set: SOL n+1 on
    uint16_t servo_us[4];     // pulse being sent, 0 = off
    int32_t stepper_position;
    uint8_t stepper_awake;
    uint16_t vbat_min_mv;     // lowest of ~10 readings since the last record (from version 4)
    uint16_t vdda_min_mv;     // the STM32's 3.3 V rail, the same way
    uint16_t servo_target_us[4];   // what each servo was told; servo_us walks to it
    uint8_t inputs;           // RECORD_INPUT_ bits (from version 5)
    uint8_t solenoids_wanted; // bit n set: SOL n+1 commanded on; on waits its turn
} record_actuators_t;

#define RECORD_INPUT_STEPPER_FAULT  (1U << 0)   // the DRV8825's FAULT line is low (ignore while asleep)
#define RECORD_INPUT_LIMIT_SWITCH   (1U << 1)   // the stepper's home switch is pressed, or its wire is broken

#define RECORD_RADIO_LISTENING    (1U << 0)   // the SX1262 answered and is listening
#define RECORD_RADIO_LINKED       (1U << 1)   // the launch box heard within the link timeout
#define RECORD_RADIO_ABORTED      (1U << 2)   // went safe on losing the launch box
#define RECORD_RADIO_STATE_HEARD  (1U << 3)   // state_valves and state_flags hold a STATE

// Written every second, and straight away when a STATE asks for something new.
// The counts are since the board started, as `radio` prints them.
typedef struct __attribute__((packed)) {
    uint32_t received;        // good packets from the launch box (node 0)
    uint32_t damaged;         // failed the CRC or the header check
    uint32_t other_version;   // this link's network, another protocol version
    uint32_t rejected;        // whole, but not this link's
    uint32_t sent;            // telemetry packets
    uint32_t restarts;        // times the chip was set up again
    int8_t rssi_dbm;          // of the last good packet from the launch box
    int8_t snr_db;
    uint8_t flags;            // RECORD_RADIO_ bits
    uint8_t state_valves;     // the last STATE: bit n set, valve n (radio_valve_t) open
    uint8_t state_flags;      // and its RADIO_STATE_ bits
    uint8_t state_count;      // the launch box's count in that STATE, wrapping (from version 5)
    uint32_t state_heard_ms;  // when it arrived, ms since boot: pairs a stand's clock with the box's
} record_radio_t;

// 16 names of 12 bytes, each ending in a 0, in the order of RADIO_NAME_CHANNELS
// (app/radio_link.h): PT1-2, TC1-4, LC1-2, SOL1-4, SRV1-4. "" = not named.
typedef struct __attribute__((packed)) {
    char names[16][12];
} record_names_t;

// Once a second: whether logging keeps up, and what goes wrong quietly elsewhere.
typedef struct __attribute__((packed)) {
    uint32_t dropped;            // records that didn't fit in the buffer, since the log started
    uint32_t ring_peak_bytes;    // most waiting in the buffer during the last second (of 65536)
    uint16_t write_max_ms;       // longest write-out to the card (with its flush), the same second
    uint16_t stack_min_bytes;    // least stack any task has had to spare since the board started
    char stack_min_task[16];     // that task, ending in a 0 unless all 16 are used
    uint32_t lc_misses;          // load-cell samples that didn't come or didn't read, since boot
    uint32_t tc_setups_again;    // thermocouple chips set up again after losing it, since boot
    uint32_t spi4_errors;        // failed SPI4 transfers (thermocouples, flash), since boot
} record_health_t;

#endif /* APP_LOG_RECORDS_H */
