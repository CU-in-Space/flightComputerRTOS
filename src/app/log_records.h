#ifndef APP_LOG_RECORDS_H
#define APP_LOG_RECORDS_H

#include <stdint.h>

/* The data log's file format: a stream of records, each a header and then its
 * payload, little-endian, no padding. tools/hat_log.py reads it and must change
 * with it; so must LOG_FORMAT_VERSION. */

#define LOG_MAGIC           "HATL"
#define LOG_FORMAT_VERSION  1U

typedef enum {
    RECORD_FILE_START = 1,   // first record of every file
    RECORD_PT,               // 10 ms of raw PT samples
    RECORD_LC,               // 10 ms of raw load-cell codes
    RECORD_TC,               // all four thermocouples, every 100 ms
    RECORD_ACTUATORS,        // interlock, VBAT and every output, every 10 ms
    RECORD_LC_CALIBRATION,   // after lc tare or lc cal
    RECORD_TEXT,             // a console command line, as typed
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
    record_lc_calibration_t lc;
} record_file_start_t;

// Samples 1 ms apart, oldest first; the last was taken just before time_ms.
// Raw ADC counts as pt_adc gives them: per sample PT1 supply, PT1 output, PT2
// supply, PT2 output.
typedef struct __attribute__((packed)) {
    uint16_t counts[10][2][2];
} record_pt_t;

// Codes 1 ms apart (the ADS1220's own clock), oldest first.
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

#endif /* APP_LOG_RECORDS_H */
