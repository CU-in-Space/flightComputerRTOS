#include "app/stand.h"

// What is wired to each stand. Edit, run the host tests (cmake --preset test,
// they check this table), and flash both stands.
//
// A sensor is just its name: 1 to 11 letters, digits or _, a letter first.
// "" for none.
//
// An output is a name and, if it's a valve, which one:
//   { .name = "" }                                              nothing on it
//   { .name = "purge" }                                         a name only
//   { .name = "vent", .drives = VALVE_VENT }                    solenoid: on opens it
//   { .name = "dump", .drives = VALVE_DUMP, .inverted = true }  normally-open solenoid: off opens it
//   { .name = "mov", .drives = VALVE_MOV, .closed_us = 1000, .open_us = 2000 }
//                                                               servo: the pulses that close and open it
//   { .name = "ign_a", .drives = IGNITER_1 }                    an igniter
//
// The valves are VALVE_DETACH, VALVE_SUPPLY, VALVE_TANK, VALVE_VENT,
// VALVE_LINE_VENT, VALVE_MOV and VALVE_DUMP, then IGNITER_1 and IGNITER_2. Each
// goes on one output of one stand. A valve follows its switch on the launch
// box, and "safe" sends it to its place in RADIO_VALVE_ABORT (radio_link.h).
//
// Nothing is assigned yet: the plumbing isn't decided.

const stand_t stand_table[RADIO_STAND_MAX] = {
    // ---- Stand 1
    {
        .pt = { "", "" },           // PT1, PT2
        .tc = { "", "", "", "" },   // TC1-4
        .lc = { "", "" },           // LC1 (connector A, fast), LC2 (connector B, a slow weight)
        .sol = {
            { .name = "" },   // SOL1
            { .name = "" },   // SOL2
            { .name = "" },   // SOL3
            { .name = "" },   // SOL4
        },
        .srv = {
            { .name = "" },   // SRV1
            { .name = "" },   // SRV2
            { .name = "" },   // SRV3
            { .name = "" },   // SRV4
        },
    },
    // ---- Stand 2
    {
        .pt = { "", "" },
        .tc = { "", "", "", "" },
        .lc = { "", "" },
        .sol = {
            { .name = "" },   // SOL1
            { .name = "" },   // SOL2
            { .name = "" },   // SOL3
            { .name = "" },   // SOL4
        },
        .srv = {
            { .name = "" },   // SRV1
            { .name = "" },   // SRV2
            { .name = "" },   // SRV3
            { .name = "" },   // SRV4
        },
    },
};
