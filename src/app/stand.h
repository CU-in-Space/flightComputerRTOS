#ifndef APP_STAND_H
#define APP_STAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/radio_link.h"

// Which stand this is, and what's wired to it: a name for each sensor and
// output, and which valve each output drives.
// The wiring of both stands is one table, in stand_table.c. To change a name or
// a valve, edit it there and reflash. The only thing a stand saves is its
// number, in the HAT's flash; the number picks the table's row.

#define NAME_SIZE      RADIO_NAME_SIZE       // 11 characters and a 0
#define CHANNEL_COUNT  RADIO_NAME_CHANNELS   // everything that has a name
#define OUTPUT_COUNT   8U                    // SOL1-4 are outputs 0-3, SRV1-4 are 4-7

typedef enum {
    KIND_PT,
    KIND_TC,
    KIND_LC,
    KIND_SOL,
    KIND_SRV,
    KIND_COUNT,
} channel_kind_t;

// What an output drives. NOT_A_VALVE is 0, so an output the table says nothing
// about drives nothing.
typedef enum {
    NOT_A_VALVE,
    VALVE_DETACH,      // the valves, in radio_valve_t's order
    VALVE_SUPPLY,
    VALVE_TANK,
    VALVE_VENT,
    VALVE_LINE_VENT,
    VALVE_MOV,
    VALVE_DUMP,
    IGNITER_1,
    IGNITER_2,
} drives_t;

// One output's row in the table.
typedef struct {
    const char *name;     // "" = no name
    drives_t drives;
    bool inverted;        // solenoid valve: open with the coil off (normally open)
    uint16_t closed_us;   // servo valve: the pulse that closes it
    uint16_t open_us;     //              and the one that opens it
} output_t;

// One stand's row.
typedef struct {
    const char *pt[2];
    const char *tc[4];
    const char *lc[2];
    output_t sol[4];
    output_t srv[4];
} stand_t;

extern const stand_t stand_table[RADIO_STAND_MAX];   // stand_table.c

// Which stand this is: 1 or 2. 1 until told otherwise.
uint8_t stand_number(void);
void stand_select(uint8_t stand);

// Reads the number from the HAT's flash. False, with the number unchanged, if
// the chip doesn't answer or holds none.
bool stand_load(void);

// Writes it. Takes about 50 ms. False on a flash error.
bool stand_save(void);

// "pt", "tc", "lc", "sol" or "srv", and how many channels that kind has.
const char *stand_kind_text(channel_kind_t kind);
unsigned stand_kind_count(channel_kind_t kind);

// A channel's name on this stand, "" if it has none. By its kind and index
// (from 0), or by its place in RADIO_NAME_CHANNELS's order.
const char *stand_name(channel_kind_t kind, unsigned index);
const char *stand_channel_name(unsigned channel);

// The same with a space in front, ready to print after the number, or "" if it
// has none: "sol 3" + " vent".
void stand_spaced_name(channel_kind_t kind, unsigned index, char text[NAME_SIZE + 1]);

// Which channel of this kind has this name in a row, from 0. -1 if none.
int stand_find_name(const stand_t *stand, channel_kind_t kind, const char *name);

// The channel a word means on this stand, from 0: its number ("3") or its name
// ("vent"). -1 if neither.
int stand_index(channel_kind_t kind, const char *word);

// An output's row on this stand.
const output_t *stand_output(unsigned output);

// The valve it drives, as radio_valve_t numbers it. -1 for an igniter or nothing.
int stand_valve(const output_t *row);

// "vent valve", "igniter 1" and so on.
const char *stand_drives_text(drives_t drives);

// What to send an output so its valve is open or closed. For a solenoid 1 = on
// and 0 = off; for a servo the pulse in microseconds.
uint16_t stand_drive(unsigned output, const output_t *row, bool open);

// The other way round: is the valve open, given what the output is being sent?
bool stand_is_open(unsigned output, const output_t *row, uint16_t drive);

// Where an output goes on "safe". One driving nothing, or an igniter, goes off.
// One driving a valve takes the valve's place in RADIO_VALVE_ABORT. False if
// that says to leave it as it is.
bool stand_safe_drive(unsigned output, const output_t *row, uint16_t *drive);

// Looks a table over for mistakes: a name that can't be typed, two channels of
// a kind with one name, a valve on two outputs (on one stand or across them), a
// servo valve without its pulses, "inverted" where it means nothing. False with
// the first one found in why. The host tests run it on stand_table, and the
// console prints what it finds.
bool stand_check(const stand_t *table, unsigned count, char *why, size_t size);

#endif /* APP_STAND_H */
