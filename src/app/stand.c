#include "app/stand.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/crc32.h"
#include "drivers/w25q128.h"

// Block 0 of the flash is kept out of the log's ring. Its first sector holds the
// load-cell calibration (lc_task.c); this is its second.
#define STAND_ADDRESS  4096UL
#define STAND_MAGIC    0x31445453UL   // "STD1"

// How the number sits in flash. The CRC catches a power cut in the middle of a save.
typedef struct {
    uint32_t magic;
    uint32_t number;
    uint32_t crc;
} saved_stand_t;

_Static_assert(VALVE_DUMP - VALVE_DETACH + 1 == RADIO_VALVE_COUNT && VALVE_MOV - VALVE_DETACH == RADIO_VALVE_MOV,
               "drives_t lists the valves in radio_valve_t's order");

typedef struct {
    const char *text;
    uint8_t first;   // place of its first channel in RADIO_NAME_CHANNELS's order
    uint8_t count;
} kind_t;

static const kind_t kinds[KIND_COUNT] = {
    [KIND_PT]  = { "pt",  0,  2 },
    [KIND_TC]  = { "tc",  2,  4 },
    [KIND_LC]  = { "lc",  6,  2 },
    [KIND_SOL] = { "sol", 8,  4 },
    [KIND_SRV] = { "srv", 12, 4 },
};

// drives_t's order.
static const char *const drives_texts[] = {
    "nothing", "detach valve", "supply valve", "tank valve", "vent valve", "line vent valve", "MOV",
    "dump valve", "igniter 1", "igniter 2",
};

// One byte, so tasks read it without a lock. The console writes it.
static uint8_t selected = 1;

uint8_t stand_number(void)
{
    return selected;
}

void stand_select(uint8_t stand)
{
    if (stand >= 1 && stand <= RADIO_STAND_MAX) {
        selected = stand;
    }
}

bool stand_load(void)
{
    saved_stand_t saved;
    if (!w25q128_read(STAND_ADDRESS, (uint8_t *)&saved, sizeof saved)
        || saved.magic != STAND_MAGIC
        || saved.crc != crc32(&saved, offsetof(saved_stand_t, crc))
        || saved.number < 1 || saved.number > RADIO_STAND_MAX) {
        return false;
    }
    selected = (uint8_t)saved.number;
    return true;
}

bool stand_save(void)
{
    saved_stand_t saved = { .magic = STAND_MAGIC, .number = selected };
    saved.crc = crc32(&saved, offsetof(saved_stand_t, crc));

    return w25q128_erase_sector(STAND_ADDRESS)
           && w25q128_program_page(STAND_ADDRESS, (const uint8_t *)&saved, sizeof saved);
}

const char *stand_kind_text(channel_kind_t kind)
{
    return kinds[kind].text;
}

unsigned stand_kind_count(channel_kind_t kind)
{
    return kinds[kind].count;
}

// A row left out of the table has no name at all: that reads as "" too.
static const char *name_in(const stand_t *stand, channel_kind_t kind, unsigned index)
{
    const char *name = NULL;
    switch (kind) {
    case KIND_PT:  name = stand->pt[index];       break;
    case KIND_TC:  name = stand->tc[index];       break;
    case KIND_LC:  name = stand->lc[index];       break;
    case KIND_SOL: name = stand->sol[index].name; break;
    case KIND_SRV: name = stand->srv[index].name; break;
    default:                                      break;
    }
    return name != NULL ? name : "";
}

static const output_t *output_in(const stand_t *stand, unsigned output)
{
    return output < OUTPUT_COUNT / 2 ? &stand->sol[output] : &stand->srv[output - OUTPUT_COUNT / 2];
}

const char *stand_name(channel_kind_t kind, unsigned index)
{
    return name_in(&stand_table[selected - 1], kind, index);
}

const char *stand_channel_name(unsigned channel)
{
    for (unsigned kind = 0; kind < KIND_COUNT; kind++) {
        if (channel < (unsigned)kinds[kind].first + kinds[kind].count) {
            return stand_name((channel_kind_t)kind, channel - kinds[kind].first);
        }
    }
    return "";
}

void stand_spaced_name(channel_kind_t kind, unsigned index, char text[NAME_SIZE + 1])
{
    const char *name = stand_name(kind, index);
    text[0] = '\0';
    if (name[0] != '\0') {
        snprintf(text, NAME_SIZE + 1, " %s", name);
    }
}

int stand_find_name(const stand_t *stand, channel_kind_t kind, const char *name)
{
    for (unsigned i = 0; i < kinds[kind].count; i++) {
        if (strcmp(name_in(stand, kind, i), name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int stand_index(channel_kind_t kind, const char *word)
{
    char *end;
    long number = strtol(word, &end, 10);
    if (*end == '\0') {
        return number >= 1 && number <= (long)kinds[kind].count ? (int)number - 1 : -1;
    }
    return stand_find_name(&stand_table[selected - 1], kind, word);
}

const output_t *stand_output(unsigned output)
{
    return output_in(&stand_table[selected - 1], output);
}

int stand_valve(const output_t *row)
{
    return row->drives >= VALVE_DETACH && row->drives <= VALVE_DUMP ? (int)row->drives - VALVE_DETACH : -1;
}

const char *stand_drives_text(drives_t drives)
{
    return drives <= IGNITER_2 ? drives_texts[drives] : "?";
}

uint16_t stand_drive(unsigned output, const output_t *row, bool open)
{
    if (output < OUTPUT_COUNT / 2) {
        return open != row->inverted;
    }
    return open ? row->open_us : row->closed_us;
}

bool stand_is_open(unsigned output, const output_t *row, uint16_t drive)
{
    if (output < OUTPUT_COUNT / 2) {
        return (drive != 0) != row->inverted;
    }
    return drive == row->open_us;
}

bool stand_safe_drive(unsigned output, const output_t *row, uint16_t *drive)
{
    int valve = stand_valve(row);
    if (row->drives == NOT_A_VALVE) {
        *drive = 0;
        return true;
    }
    if (valve < 0) {
        *drive = stand_drive(output, row, false);   // an igniter: off
        return true;
    }
    if (RADIO_VALVE_ABORT[valve] < 0) {
        return false;
    }
    *drive = stand_drive(output, row, RADIO_VALVE_ABORT[valve] == 1);
    return true;
}

// 1 to 11 characters: a letter, then letters, digits or '_'. One word, so it can
// sit in a command and a CSV header; a letter first, so "sol 3 on" is never a name.
static bool name_ok(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length >= NAME_SIZE || !isalpha((unsigned char)name[0])) {
        return false;
    }
    for (size_t i = 1; i < length; i++) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_') {
            return false;
        }
    }
    return true;
}

bool stand_check(const stand_t *table, unsigned count, char *why, size_t size)
{
    unsigned driven = 0;   // bit per drives_t, over every stand: a valve is on one output only

    for (unsigned s = 0; s < count; s++) {
        for (unsigned kind = 0; kind < KIND_COUNT; kind++) {
            for (unsigned i = 0; i < kinds[kind].count; i++) {
                const char *name = name_in(&table[s], (channel_kind_t)kind, i);
                const char *problem = NULL;
                if (name[0] == '\0') {
                    continue;
                }
                if (!name_ok(name)) {
                    problem = "a name is 1 to 11 letters, digits or _, a letter first";
                } else if (stand_find_name(&table[s], (channel_kind_t)kind, name) != (int)i) {
                    problem = "two of a kind have this name";
                }
                if (problem != NULL) {
                    snprintf(why, size, "stand %u %s %u: %s", s + 1, kinds[kind].text, i + 1, problem);
                    return false;
                }
            }
        }

        for (unsigned output = 0; output < OUTPUT_COUNT; output++) {
            const output_t *row = output_in(&table[s], output);
            bool solenoid = output < OUTPUT_COUNT / 2;
            const char *problem = NULL;
            if (row->drives > IGNITER_2) {
                problem = "drives something that isn't in drives_t";
            } else if (row->inverted && (!solenoid || stand_valve(row) < 0)) {
                // An inverted igniter would be on whenever it isn't firing.
                problem = "inverted is only for a solenoid valve";
            } else if (row->drives != NOT_A_VALVE && (driven & (1U << row->drives))) {
                problem = "its valve is already on another output";
            } else if (row->drives != NOT_A_VALVE && !solenoid
                       && (row->closed_us < 500 || row->closed_us > 2500 || row->open_us < 500
                           || row->open_us > 2500 || row->closed_us == row->open_us)) {
                problem = "a servo valve needs closed_us and open_us, 500 to 2500 and different";
            }
            if (problem != NULL) {
                snprintf(why, size, "stand %u %s %u: %s", s + 1, solenoid ? "sol" : "srv",
                         solenoid ? output + 1 : output - OUTPUT_COUNT / 2 + 1, problem);
                return false;
            }
            driven |= 1U << row->drives;
        }
    }
    return true;
}
