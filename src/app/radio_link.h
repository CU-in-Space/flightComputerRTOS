#ifndef APP_RADIO_LINK_H
#define APP_RADIO_LINK_H

#include <stdint.h>

/* Everything the launch box and the stands have to agree on: the LoRa settings
 * and the packets. No other includes and nothing a C++ compiler objects to, so
 * the launch box sketch uses a copy of this file as it is. This one is the
 * master: change it here, then copy it to lunch-box/launch_box/ and
 * tools/lora_bench/.
 * docs/radio-link.md explains it.
 *
 * The launch box talks first, always. A stand only ever transmits one telemetry
 * packet, in answer to a packet addressed to it. */

// ---- LoRa settings

#define RADIO_FREQUENCY_HZ      915000000UL
#define RADIO_BANDWIDTH_KHZ     500U    // 500 kHz: 1 W needs no frequency hopping under FCC 15.247
#define RADIO_SPREADING_FACTOR  7U
#define RADIO_CODING_RATE       5U      // 4/5
#define RADIO_PREAMBLE_SYMBOLS  8U
#define RADIO_SYNC_WORD         0x12U   // the SX127x's reset value, which RadioHead leaves alone
// Explicit header, CRC on, IQ not inverted.

// ---- Who is who

#define RADIO_NODE_BOX   0U      // the launch box
#define RADIO_STAND_MAX  2U      // the stands are 1 and 2
#define RADIO_NODE_ALL   0xFFU   // everyone listening

// The first 4 bytes of every packet. The same 4 bytes the RadioHead library
// puts in front of everything it sends and takes off everything it receives
// (to, from, id, flags), so a RadioHead sketch never sees them as data: it
// sets and reads them with setHeaderTo(), headerFrom() and so on.
typedef struct __attribute__((packed)) {
    uint8_t to;         // RADIO_NODE_BOX, a stand's number or RADIO_NODE_ALL
    uint8_t from;
    // TELEMETRY: the number of the last COMMAND or SAFE this stand took.
    // COMMAND and SAFE: that number plus one (255 wraps to 0). The launch box
    // repeats the packet, same number, until the stand's telemetry carries it
    // back. Others: not used.
    uint8_t sequence;
    uint8_t spare;      // 0
} radio_address_t;

// ---- Packets. Little-endian, no padding. Each follows a radio_address_t.

#define RADIO_NETWORK_ID        0x4355U   // "CU": anything else on the channel is dropped
#define RADIO_PROTOCOL_VERSION  3U        // change it whenever a packet below changes

typedef enum {
    RADIO_POLL = 1,     // launch box: nothing to do, just answer
    RADIO_COMMAND,      // launch box: a console command line follows the tag
    RADIO_SAFE,         // launch box: every output to its safe state, now
    RADIO_TELEMETRY,    // stand: the answer to a packet addressed to it
    RADIO_STATE,        // launch box: where every valve should be
} radio_packet_type_t;

// Starts every packet. 4 bytes.
typedef struct __attribute__((packed)) {
    uint16_t network;   // RADIO_NETWORK_ID
    uint8_t version;    // RADIO_PROTOCOL_VERSION
    uint8_t type;       // radio_packet_type_t
} radio_tag_t;

// A COMMAND is the tag, then the line as typed at the console ("log start"),
// with no terminator. The packet's length says where it ends.
#define RADIO_COMMAND_MAX  60U

// The valves, in the order of the launch box's switches for them. The valves
// themselves are on the stands: each stand's row in app/stand_table.c says which
// of its outputs is which.
typedef enum {
    RADIO_VALVE_DETACH,
    RADIO_VALVE_SUPPLY,
    RADIO_VALVE_TANK,
    RADIO_VALVE_VENT,
    RADIO_VALVE_LINE_VENT,
    RADIO_VALVE_MOV,
    RADIO_VALVE_DUMP,
    RADIO_VALVE_COUNT,
} radio_valve_t;

// Where each valve goes on an abort: 1 open, 0 closed, -1 left as it is. The
// launch box uses it for its abort button, a stand when it loses the launch box.
static const int8_t RADIO_VALVE_ABORT[RADIO_VALVE_COUNT] = { -1, 0, 1, 1, 1, 0, 1 };

#define RADIO_STATE_ARMED          (1U << 0)   // key in: the stands follow valves and igniters
#define RADIO_STATE_ABORT          (1U << 1)   // abort latched: valves holds the abort positions, follow them
#define RADIO_STATE_IGNITER_1      (1U << 2)   // igniter I on
#define RADIO_STATE_IGNITER_2      (1U << 3)
#define RADIO_STATE_FIRE_SEQUENCE  (1U << 4)   // the fire sequence is running (for the record only)

// STATE: the launch box sends it all the time, to each stand in turn. Every
// stand that hears one acts on it, whoever it is addressed to. 6 bytes.
typedef struct __attribute__((packed)) {
    radio_tag_t tag;
    uint8_t valves;     // bit n set: valve n (radio_valve_t) open
    uint8_t flags;      // RADIO_STATE_ bits. Neither ARMED nor ABORT: hold everything as it is
} radio_state_t;

#define RADIO_VALID_PT1             (1U << 0)   // pt_psi[0] is a reading
#define RADIO_VALID_PT2             (1U << 1)
#define RADIO_VALID_LC1             (1U << 2)   // lc_code[0] is a reading
#define RADIO_VALID_LC1_CALIBRATED  (1U << 3)   // lc_newtons[0] means something
#define RADIO_VALID_TC1             (1U << 4)   // tc_c[0] is a reading with no fault; TC2-4 are bits 5-7
#define RADIO_VALID_LC2             (1U << 8)   // lc_code[1] is a reading
#define RADIO_VALID_LC2_CALIBRATED  (1U << 9)   // lc_newtons[1] means something

#define RADIO_FLAG_LOGGING         (1U << 0)   // the data log is recording, to the SD card, the flash or both
// The stand lost the launch box for too long and went to the abort positions by
// itself. It ignores STATE while this is set; a STATE without ARMED clears it.
#define RADIO_FLAG_ABORTED         (1U << 1)

#define RADIO_NAME_SIZE      12U   // 11 characters and a 0
#define RADIO_NAME_CHANNELS  16U   // 0-1 PT1-2, 2-5 TC1-4, 6-7 LC1-2, 8-11 SOL1-4, 12-15 SRV1-4

// 83 bytes.
typedef struct __attribute__((packed)) {
    radio_tag_t tag;
    uint32_t time_ms;       // stand time since boot
    uint16_t valid;         // RADIO_VALID_ bits
    uint8_t flags;          // RADIO_FLAG_ bits
    uint8_t solenoids;      // bit n set: SOL n+1 on
    uint8_t valves_mapped;  // bit n set: this stand drives valve n
    uint8_t valves;         // of those, bit n set: valve n is open
    uint16_t servo_us[4];   // pulse being sent, 0 = off
    uint16_t vbat_mv;
    float pt_psi[2];
    float lc_newtons[2];    // LC1 (the fast one), LC2 (the slow weight)
    int32_t lc_code[2];     // raw: LC1's 10 ms average, LC2's latest sample
    float tc_c[4];
    uint32_t log_bytes;     // written to the current SD file; stays 0 if only the flash is recording
    int8_t rssi_dbm;        // how the stand heard the packet this answers
    int8_t snr_db;
    // One channel's name per packet, each channel in turn, so a listener has
    // them all after RADIO_NAME_CHANNELS packets. "" = not named.
    uint8_t name_channel;
    char name[RADIO_NAME_SIZE];
} radio_telemetry_t;

#endif /* APP_RADIO_LINK_H */
