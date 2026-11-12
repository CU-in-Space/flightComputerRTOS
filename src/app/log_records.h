#ifndef APP_LOG_RECORDS_H
#define APP_LOG_RECORDS_H

#include <stdint.h>

/* The data log's file format: a stream of records, each a header and then its
 * payload, little-endian, no padding. tools/hat_log.py reads it and must change
 * with it; so must LOG_FORMAT_VERSION. */

#define LOG_MAGIC           "HATL"
#define LOG_FORMAT_VERSION  2U   // 2: a second load cell, so 16 names and two calibrations
#define LOG_BLOCK_MAGIC     "HATB"

typedef enum {
    RECORD_FILE_START = 1,   // first record of every file
    RECORD_PT,               // 10 ms of raw PT samples
    RECORD_LC,               // 10 samples of raw codes from the first load cell
    RECORD_TC,               // all four thermocouples, every 100 ms
    RECORD_ACTUATORS,        // interlock, VBAT and every output, every 10 ms
    RECORD_LC_CALIBRATION,   // after lc tare or lc cal on the first load cell
    RECORD_TEXT,             // a console command line, as typed
    RECORD_NAMES,            // every channel's name, straight after the file start
    RECORD_LC2,              // one raw code (an int32_t) from the second load cell, about 10 a second
    RECORD_LC2_CALIBRATION,  // after lc tare or lc cal on the second load cell
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
    uint32_t vbat_mv;
    uint8_t solenoids;        // bit n set: SOL n+1 on
    uint16_t servo_us[4];     // pulse being sent, 0 = off
    int32_t stepper_position;
    uint8_t stepper_awake;
} record_actuators_t;

// 16 names of 12 bytes, each ending in a 0, in the order of RADIO_NAME_CHANNELS
// (app/radio_link.h): PT1-2, TC1-4, LC1-2, SOL1-4, SRV1-4. "" = not named.
typedef struct __attribute__((packed)) {
    char names[16][12];
} record_names_t;

// Starts every 64 KB block of the flash log (app/flash_log.h). Then come records,
// whole ones only: a FILE_START first, so each block decodes alone, then the
// log. Bytes after the last record stay 0xFF.
typedef struct __attribute__((packed)) {
    char magic[4];         // LOG_BLOCK_MAGIC, no terminator
    uint32_t run;          // counts up with each log start, from 1
    uint32_t sequence;     // this block's place in its run, from 0
} flash_block_header_t;

#define DUMP_SYNC_0      0xA5U
#define DUMP_SYNC_1      0x5AU
#define DUMP_DATA_SIZE   116U

// `flash dump` sends a run's blocks in these. Always the full size; length says
// how much of data counts, and 0 ends the dump (offset is then the total).
typedef struct __attribute__((packed)) {
    uint8_t sync[2];                // DUMP_SYNC_0, DUMP_SYNC_1
    uint8_t length;
    uint32_t offset;                // of data[0], from the start of the dump
    uint8_t data[DUMP_DATA_SIZE];
    uint32_t crc;                   // CRC-32 of everything after sync, up to here
} flash_dump_frame_t;

#endif /* APP_LOG_RECORDS_H */
