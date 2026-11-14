#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/stand.h"
#include "drivers/w25q128.h"

// A pretend W25Q128, just the stand number's sector and its neighbours.
static uint8_t chip[3 * W25Q128_SECTOR_SIZE];
static bool chip_fails;

bool w25q128_read(uint32_t address, uint8_t *data, uint16_t length)
{
    assert(address + length <= sizeof chip);
    memcpy(data, &chip[address], length);
    return !chip_fails;
}

bool w25q128_program_page(uint32_t address, const uint8_t *data, uint16_t length)
{
    assert(length > 0 && length <= W25Q128_PAGE_SIZE);
    assert(address / W25Q128_PAGE_SIZE == (address + length - 1) / W25Q128_PAGE_SIZE);
    for (uint16_t i = 0; i < length; i++) {
        assert(chip[address + i] == 0xFF);   // must have been erased first
        chip[address + i] = data[i];
    }
    return !chip_fails;
}

bool w25q128_erase_sector(uint32_t address)
{
    memset(&chip[address & ~(W25Q128_SECTOR_SIZE - 1UL)], 0xFF, W25Q128_SECTOR_SIZE);
    return !chip_fails;
}

// A table the way a test stand might fill it in.
static const stand_t example[2] = {
    {
        .pt = { "ox_tank", "chamber" },
        .tc = { "", "nozzle" },
        .lc = { "thrust", "bottle" },
        .sol = {
            { .name = "vent", .drives = VALVE_VENT },
            { .name = "dump", .drives = VALVE_DUMP, .inverted = true },
            { .name = "purge" },
            { .name = "ign_a", .drives = IGNITER_1 },
        },
        .srv = {
            { .name = "mov", .drives = VALVE_MOV, .closed_us = 1900, .open_us = 1100 },
        },
    },
    {
        .pt = { "supply" },
        .sol = { { .name = "supply", .drives = VALVE_SUPPLY }, { .name = "detach", .drives = VALVE_DETACH } },
    },
};

// The table that gets flashed has no mistakes in it.
static void test_table(void)
{
    char why[96] = "";
    bool ok = stand_check(stand_table, RADIO_STAND_MAX, why, sizeof why);
    if (!ok) {
        printf("stand_table.c: %s\n", why);
    }
    assert(ok);
}

// True if the checker turns the example down once one row is replaced, and
// says which row.
static bool refused(unsigned stand, unsigned output, output_t row, const char *where)
{
    stand_t table[2] = { example[0], example[1] };
    char why[96] = "";
    if (output < 4) {
        table[stand].sol[output] = row;
    } else {
        table[stand].srv[output - 4] = row;
    }
    return !stand_check(table, 2, why, sizeof why) && strstr(why, where) != NULL;
}

static void test_check(void)
{
    char why[96] = "";
    assert(stand_check(example, 2, why, sizeof why));

    // Names: one word, a letter first, 11 characters at most.
    assert(!refused(0, 2, (output_t){ .name = "abcdefghijk" }, "sol 3"));             // 11 is fine
    assert(refused(0, 2, (output_t){ .name = "abcdefghijkl" }, "stand 1 sol 3"));    // 12
    assert(refused(0, 2, (output_t){ .name = "2nd" }, "sol 3"));                     // would read as a number
    assert(refused(0, 2, (output_t){ .name = "ox tank" }, "sol 3"));
    assert(refused(0, 2, (output_t){ .name = "vent" }, "sol 3"));                    // SOL1 is "vent" already
    assert(!refused(0, 5, (output_t){ .name = "vent" }, "srv 2"));                   // names are per kind

    // A valve is on one output: of this stand, and of the two together.
    assert(refused(0, 2, (output_t){ .name = "vent2", .drives = VALVE_VENT }, "stand 1 sol 3"));
    assert(refused(1, 2, (output_t){ .name = "vent", .drives = VALVE_VENT }, "stand 2 sol 3"));
    assert(refused(1, 2, (output_t){ .name = "ign", .drives = IGNITER_1 }, "stand 2 sol 3"));

    // A servo valve needs both pulses, in range and different.
    output_t tank = { .name = "tank", .drives = VALVE_TANK };
    assert(refused(0, 5, tank, "stand 1 srv 2"));
    tank.closed_us = 1500;
    tank.open_us = 1500;
    assert(refused(0, 5, tank, "srv 2"));
    tank.closed_us = 400;
    assert(refused(0, 5, tank, "srv 2"));
    tank.closed_us = 1000;
    tank.open_us = 2000;
    assert(!refused(0, 5, tank, "srv 2"));

    // "inverted" is for a solenoid valve only. An inverted igniter would be on
    // whenever it isn't firing.
    assert(refused(1, 2, (output_t){ .name = "ign_b", .drives = IGNITER_2, .inverted = true }, "stand 2 sol 3"));
    tank.inverted = true;
    assert(refused(0, 5, tank, "srv 2"));
    assert(refused(0, 2, (output_t){ .name = "purge", .inverted = true }, "sol 3"));
}

static void test_names(void)
{
    assert(stand_find_name(&example[0], KIND_TC, "nozzle") == 1);
    assert(stand_find_name(&example[0], KIND_SOL, "vent") == 0);
    assert(stand_find_name(&example[0], KIND_SRV, "vent") == -1);    // names are per kind
    assert(stand_find_name(&example[0], KIND_TC, "throat") == -1);
    assert(stand_find_name(&example[1], KIND_SOL, "supply") == 0);
    assert(stand_find_name(&example[1], KIND_TC, "") == 0);          // rows left out read as ""

    // Every channel is used once, in the order radio_link.h documents.
    assert(stand_kind_count(KIND_PT) + stand_kind_count(KIND_TC) + stand_kind_count(KIND_LC)
           + stand_kind_count(KIND_SOL) + stand_kind_count(KIND_SRV) == CHANNEL_COUNT);
    assert(strcmp(stand_kind_text(KIND_TC), "tc") == 0);

    // A number works whether or not the channel has a name.
    assert(stand_index(KIND_SOL, "3") == 2 && stand_index(KIND_SOL, "5") == -1);
    assert(stand_index(KIND_LC, "2") == 1 && stand_index(KIND_LC, "0") == -1);
    assert(stand_find_name(&example[0], KIND_LC, "bottle") == 1);
    assert(stand_index(KIND_SOL, "nobody") == -1);
    assert(strcmp(stand_channel_name(CHANNEL_COUNT), "") == 0);
}

static void test_drive(void)
{
    const unsigned sol = 1;   // SOL2
    const unsigned srv = 5;   // SRV2
    uint16_t drive;

    // A solenoid valve that opens when on.
    output_t vent = { .name = "vent", .drives = VALVE_VENT };
    assert(stand_valve(&vent) == RADIO_VALVE_VENT);
    assert(stand_drive(sol, &vent, true) == 1 && stand_drive(sol, &vent, false) == 0);
    assert(stand_is_open(sol, &vent, 1) && !stand_is_open(sol, &vent, 0));
    assert(stand_safe_drive(sol, &vent, &drive) && drive == 1);       // vent opens on abort

    // A normally-open one: off opens it.
    output_t dump = { .name = "dump", .drives = VALVE_DUMP, .inverted = true };
    assert(stand_valve(&dump) == RADIO_VALVE_DUMP);
    assert(stand_drive(sol, &dump, true) == 0 && stand_drive(sol, &dump, false) == 1);
    assert(stand_is_open(sol, &dump, 0) && !stand_is_open(sol, &dump, 1));
    assert(stand_safe_drive(sol, &dump, &drive) && drive == 0);       // dump opens on abort: coil off

    // A servo valve, wired so the higher pulse closes it.
    output_t mov = { .name = "mov", .drives = VALVE_MOV, .closed_us = 1900, .open_us = 1100 };
    assert(stand_valve(&mov) == RADIO_VALVE_MOV);
    assert(stand_drive(srv, &mov, true) == 1100 && stand_drive(srv, &mov, false) == 1900);
    assert(stand_is_open(srv, &mov, 1100) && !stand_is_open(srv, &mov, 1900));
    assert(!stand_is_open(srv, &mov, 0));                              // not driven: not known open
    assert(stand_safe_drive(srv, &mov, &drive) && drive == 1900);      // MOV closes on abort

    // Detach is left as it is on abort.
    output_t detach = { .name = "detach", .drives = VALVE_DETACH };
    assert(stand_valve(&detach) == RADIO_VALVE_DETACH);
    assert(!stand_safe_drive(sol, &detach, &drive));

    // An igniter and an output that drives nothing are no valve, and both go off.
    output_t igniter = { .name = "ign_a", .drives = IGNITER_1 };
    output_t nothing = { .name = "purge" };
    assert(stand_valve(&igniter) == -1 && stand_valve(&nothing) == -1);
    assert(stand_safe_drive(sol, &igniter, &drive) && drive == 0);
    drive = 7;
    assert(stand_safe_drive(srv, &nothing, &drive) && drive == 0);
}

static void test_number(void)
{
    memset(chip, 0xFF, sizeof chip);

    // Stand 1 until told otherwise, and only 1 or 2.
    assert(stand_number() == 1);
    stand_select(3);
    assert(stand_number() == 1);
    stand_select(2);
    assert(stand_number() == 2);

    // A blank chip holds nothing, and the number is left alone.
    assert(!stand_load() && stand_number() == 2);

    assert(stand_save());
    stand_select(1);
    assert(stand_load() && stand_number() == 2);

    // It stays in its own sector: the load-cell calibration's and the next are untouched.
    for (unsigned i = 0; i < W25Q128_SECTOR_SIZE; i++) {
        assert(chip[i] == 0xFF && chip[2 * W25Q128_SECTOR_SIZE + i] == 0xFF);
    }

    // Saving again over the old copy works (the sector is erased first).
    stand_select(1);
    assert(stand_save());
    stand_select(2);
    assert(stand_load() && stand_number() == 1);

    // One flipped bit, as a power cut mid-save would leave: refused.
    chip[W25Q128_SECTOR_SIZE + 5] ^= 0x01;
    stand_select(2);
    assert(!stand_load() && stand_number() == 2);

    chip_fails = true;
    assert(!stand_save() && !stand_load());
    chip_fails = false;
    stand_select(1);
}

int main(void)
{
    test_table();
    test_check();
    test_names();
    test_drive();
    test_number();
    return 0;
}
