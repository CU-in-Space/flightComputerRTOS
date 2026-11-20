#!/usr/bin/env python3
"""Decode a Sensor HAT data log (S1_nnnn.BIN or S2_nnnn.BIN) into CSV files.

Writes, into a folder named after the log:
  pt.csv         every PT sample (1 kHz): raw counts, ratio and psi
  lc.csv         every sample of load cell 1 (about 1 kHz): raw code, mV/V and newtons
  lc2.csv        load cell 2, the slow one (about 10 a second): the same columns
  tc.csv         the four thermocouples, every 100 ms
  actuators.csv  interlock, VBAT and every output, every 10 ms. From format 4 also
                 the lowest VBAT and 3.3 V rail in each 10 ms and the servo targets;
                 from format 5 the solenoids asked for, the stepper's FAULT line and
                 its limit switch (blank in older logs)
  radio.csv      the radio's counts and the launch box's last STATE, every second
                 and at each new STATE (format 3). From format 5 that STATE's
                 count from the launch box and when it came
  health.csv     once a second (format 5): records dropped, the buffer's peak, the
                 slowest card write, the least stack any task has left and which,
                 load-cell misses, thermocouple re-setups, SPI4 errors
  console.csv    every line the firmware printed while recording (format 5)
  events.csv     console commands ("typed>" or "radio>" from format 5), load-cell
                 calibrations, channel names, the boot line (format 4: boot count,
                 reset reason, any crash; format 5: firmware and stand), and the
                 radio's link and the launch box's STATE as they change

From format 4 the firmware starts a new file every hour. Each decodes on its own,
and near the top of events.csv a later one says "continues S1_nnnn.BIN".

A channel with a name in the firmware's stand table has it in its columns:
tc2_nozzle_c instead of tc2_c.

The format is src/app/log_records.h; change both together. Times are seconds
since the board booted. Psi and newtons use the calibration recorded in the file,
so a file decodes the same however the firmware has changed since.

Usage:
    python3 tools/hat_log.py S1_0001.BIN [S1_0002.BIN ...] [--out DIR]
"""
import argparse
import csv
import struct
from pathlib import Path

HEADER = struct.Struct("<BBI")              # type, payload length, time_ms
FILE_START = struct.Struct("<4sH" + "fff" * 2 + "if" * 2)   # version 2: both load cells' calibration
FILE_START_V1 = struct.Struct("<4sH" + "fff" * 2 + "if")    # version 1: one load cell
PT = struct.Struct("<" + "H" * 40)          # 10 samples x 2 PTs x (supply, output)
LC = struct.Struct("<" + "i" * 10)
TC = struct.Struct("<" + "BBff" * 4)
ACTUATORS = struct.Struct("<BIB4HiB")
ACTUATORS_V4 = struct.Struct("<BIB4HiB" + "HH" + "4H")   # + VBAT and VDDA lows, servo targets
ACTUATORS_V5 = struct.Struct("<BIB4HiB" + "HH" + "4H" + "BB")   # + inputs, solenoids asked for
LC_CALIBRATION = struct.Struct("<if")
LC2 = struct.Struct("<i")
RADIO = struct.Struct("<6IbbBBB")
RADIO_V5 = struct.Struct("<6IbbBBB" + "BI")    # + the STATE's count and when it came
HEALTH = struct.Struct("<IIHH16sIII")
NAME_SIZE = 12
CHANNELS = ["pt1", "pt2", "tc1", "tc2", "tc3", "tc4", "lc1", "lc2",
            "sol1", "sol2", "sol3", "sol4", "srv1", "srv2", "srv3", "srv4"]

RECORD_FILE_START, RECORD_PT, RECORD_LC, RECORD_TC, RECORD_ACTUATORS, RECORD_LC_CALIBRATION, \
    RECORD_TEXT, RECORD_NAMES, RECORD_LC2, RECORD_LC2_CALIBRATION, RECORD_RADIO, RECORD_HEALTH, \
    RECORD_CONSOLE = range(1, 14)

# The payload sizes each record may have. Two where version 1 and 2 differ.
SIZES = {
    RECORD_FILE_START: (FILE_START_V1.size, FILE_START.size),
    RECORD_PT: (PT.size,),
    RECORD_LC: (LC.size,),
    RECORD_TC: (TC.size,),
    RECORD_ACTUATORS: (ACTUATORS.size, ACTUATORS_V4.size, ACTUATORS_V5.size),
    RECORD_LC_CALIBRATION: (LC_CALIBRATION.size,),
    RECORD_NAMES: (15 * NAME_SIZE, 16 * NAME_SIZE),
    RECORD_LC2: (LC2.size,),
    RECORD_LC2_CALIBRATION: (LC_CALIBRATION.size,),
    RECORD_RADIO: (RADIO.size, RADIO_V5.size),
    RECORD_HEALTH: (HEALTH.size,),
}

FORMAT_VERSIONS = (1, 2, 3, 4, 5)   # 1: one load cell and 15 names. 2: two and 16. 3: radio records.
                                    # 4: rail lows, servo targets. 5: health, console, inputs, STATE count
INTERLOCK_NAMES = ["RECOVERING", "CABLE_FAULT", "ENABLED"]
SAMPLES_PER_BLOCK = 10
ADS1220_FULL_SCALE = 2 ** 23
ADS1220_GAIN = 128
PT_MIN_SUPPLY_COUNTS = 65535 // 5           # as src/app/pt_convert.c

# RECORD_RADIO's flags, and the launch box's (app/radio_link.h).
RADIO_LISTENING, RADIO_LINKED, RADIO_ABORTED, RADIO_STATE_HEARD = (1 << n for n in range(4))
STATE_FLAGS = ["armed", "abort", "igniter_1", "igniter_2", "fire_sequence"]
VALVES = ["detach", "supply", "tank", "vent", "line_vent", "mov", "dump"]


def records(data):
    """Yields (type, time_ms, payload). Stops at a record cut short by a power loss."""
    offset = 0
    while offset + HEADER.size <= len(data):
        kind, length, time_ms = HEADER.unpack_from(data, offset)
        offset += HEADER.size
        if offset + length > len(data):
            print(f"  last record cut short at byte {offset - HEADER.size}, ignored")
            return
        if kind in SIZES and length not in SIZES[kind]:
            raise ValueError(f"record type {kind} at byte {offset - HEADER.size} has "
                             f"{length} bytes, expected {' or '.join(map(str, SIZES[kind]))}")
        yield kind, time_ms, data[offset:offset + length]
        offset += length


def sample_times(time_ms):
    """The block's samples were 1 ms apart and the last was taken just before time_ms."""
    return [(time_ms - (SAMPLES_PER_BLOCK - 1 - n)) / 1000 for n in range(SAMPLES_PER_BLOCK)]


def blank_or(value, digits):
    return "" if value is None else f"{value:.{digits}f}"


def state_text(valves, flags):
    """A STATE as the launch box and `radio` print it: valves 0001010, armed."""
    text = "valves " + "".join(str((valves >> v) & 1) for v in range(len(VALVES)))
    text += "".join(f", {name.replace('_', ' ')}" for n, name in enumerate(STATE_FLAGS) if flags & (1 << n))
    return text if flags & 0b11 else text + ", key off"


def unpack_names(payload):
    """One name per entry of CHANNELS. A version 1 log has 15: no second load cell."""
    names = [payload[n:n + NAME_SIZE].split(b"\0")[0].decode("ascii", "replace")
             for n in range(0, len(payload), NAME_SIZE)]
    return names if len(names) == len(CHANNELS) else names[:7] + [""] + names[7:]


def first_names(data):
    """The names the log started with. They follow the file start; none in an old log."""
    for n, (kind, _, payload) in enumerate(records(data)):
        if kind == RECORD_NAMES:
            return unpack_names(payload)
        if n >= 10:
            break
    return [""] * len(CHANNELS)


def decode(path, out_dir):
    data = path.read_bytes()
    out = out_dir / path.stem
    out.mkdir(parents=True, exist_ok=True)

    # A named channel's columns carry the name: tc2_nozzle_c instead of tc2_c.
    names = first_names(data)
    labels = [f"{channel}_{name}" if name else channel for channel, name in zip(CHANNELS, names)]
    pt_labels, tc_labels, sol_labels, srv_labels = labels[0:2], labels[2:6], labels[8:12], labels[12:16]
    lc_prefix = f"{names[6]}_" if names[6] else ""
    lc2_prefix = f"{names[7]}_" if names[7] else ""

    files = {name: open(out / f"{name}.csv", "w", newline="")
             for name in ("pt", "lc", "lc2", "tc", "actuators", "radio", "health", "console", "events")}
    writers = {name: csv.writer(f) for name, f in files.items()}
    writers["pt"].writerow(["time_s"] + [f"{p}_{c}" for p in pt_labels
                                         for c in ("supply_counts", "data_counts", "ratio", "psi")])
    writers["lc"].writerow(["time_s"] + [f"{lc_prefix}{c}" for c in ("code", "mv_per_v", "newtons")])
    writers["lc2"].writerow(["time_s"] + [f"{lc2_prefix}{c}" for c in ("code", "mv_per_v", "newtons")])
    writers["tc"].writerow(["time_s"] + [f"{t}_{c}" for t in tc_labels
                                         for c in ("c", "cold_junction_c", "faults")])
    writers["actuators"].writerow(["time_s", "interlock", "vbat_v"] + sol_labels
                                  + [f"{s}_us" for s in srv_labels]
                                  + ["stepper_position", "stepper_awake", "vbat_min_v", "vdda_min_v"]
                                  + [f"{s}_target_us" for s in srv_labels]
                                  + [f"{s}_wanted" for s in sol_labels] + ["stepper_fault", "limit_switch"])
    writers["radio"].writerow(["time_s", "listening", "linked", "aborted", "received", "damaged",
                               "other_version", "not_this_links", "sent", "restarts", "box_dbm", "box_snr_db"]
                              + [f"state_{f}" for f in STATE_FLAGS] + [f"state_valve_{v}" for v in VALVES]
                              + ["state_count", "state_heard_s"])
    writers["health"].writerow(["time_s", "dropped", "buffer_peak_bytes", "write_max_ms", "stack_min_bytes",
                                "stack_min_task", "lc_misses", "tc_setups_again", "spi4_errors"])
    writers["console"].writerow(["time_s", "line"])
    writers["events"].writerow(["time_s", "event"])

    pt_calibrations = None
    lc_zero, lc_newtons_per_code = 0, 0.0
    lc2_zero, lc2_newtons_per_code = 0, 0.0
    counts = {}
    radio_flags, radio_state = 0, None   # the last radio record's, for events on a change

    for kind, time_ms, payload in records(data):
        counts[kind] = counts.get(kind, 0) + 1
        time_s = time_ms / 1000

        if pt_calibrations is None and kind != RECORD_FILE_START:
            raise ValueError("file doesn't start with a file-start record")

        if kind == RECORD_FILE_START:
            layout = FILE_START if len(payload) == FILE_START.size else FILE_START_V1
            fields = layout.unpack(payload)
            if fields[0] != b"HATL" or fields[1] not in FORMAT_VERSIONS:
                raise ValueError(f"not a HAT log of version {', '.join(map(str, FORMAT_VERSIONS))}")
            pt_calibrations = [fields[2:5], fields[5:8]]
            lc_zero, lc_newtons_per_code = fields[8], fields[9]
            if layout is FILE_START:
                lc2_zero, lc2_newtons_per_code = fields[10], fields[11]

        elif kind == RECORD_PT:
            values = PT.unpack(payload)
            for n, t in enumerate(sample_times(time_ms)):
                row = [f"{t:.3f}"]
                for p in range(2):
                    supply, output = values[n * 4 + p * 2], values[n * 4 + p * 2 + 1]
                    ratio = output / supply if supply >= PT_MIN_SUPPLY_COUNTS else None
                    at_zero, at_full, full_scale = pt_calibrations[p]
                    psi = None if ratio is None else (ratio - at_zero) / (at_full - at_zero) * full_scale
                    row += [supply, output, blank_or(ratio, 5), blank_or(psi, 2)]
                writers["pt"].writerow(row)

        elif kind == RECORD_LC:
            for code, t in zip(LC.unpack(payload), sample_times(time_ms)):
                mv_per_v = code / ADS1220_FULL_SCALE / ADS1220_GAIN * 1000
                newtons = (code - lc_zero) * lc_newtons_per_code if lc_newtons_per_code else None
                writers["lc"].writerow([f"{t:.3f}", code, f"{mv_per_v:.6f}", blank_or(newtons, 2)])

        elif kind == RECORD_LC2:
            code, = LC2.unpack(payload)
            mv_per_v = code / ADS1220_FULL_SCALE / ADS1220_GAIN * 1000
            newtons = (code - lc2_zero) * lc2_newtons_per_code if lc2_newtons_per_code else None
            writers["lc2"].writerow([f"{time_s:.3f}", code, f"{mv_per_v:.6f}", blank_or(newtons, 2)])

        elif kind == RECORD_TC:
            values = TC.unpack(payload)
            row = [f"{time_s:.3f}"]
            for t in range(4):
                present, faults, temp_c, cold_c = values[t * 4:t * 4 + 4]
                row += [f"{temp_c:.2f}", f"{cold_c:.2f}", faults] if present else ["", "", ""]
            writers["tc"].writerow(row)

        elif kind == RECORD_ACTUATORS:
            extra = [""] * 12   # columns an older format doesn't have
            if len(payload) == ACTUATORS.size:
                interlock, vbat_mv, solenoids, *rest = ACTUATORS.unpack(payload)
            else:
                layout = ACTUATORS_V5 if len(payload) == ACTUATORS_V5.size else ACTUATORS_V4
                interlock, vbat_mv, solenoids, *rest = layout.unpack(payload)
                vbat_min_mv, vdda_min_mv = rest[6:8]
                extra[0:6] = [f"{vbat_min_mv / 1000:.3f}", f"{vdda_min_mv / 1000:.3f}"] + rest[8:12]
                if layout is ACTUATORS_V5:
                    inputs, wanted = rest[12:14]
                    extra[6:12] = [(wanted >> n) & 1 for n in range(4)] + [inputs & 1, (inputs >> 1) & 1]
            servos, (position, awake) = rest[:4], rest[4:6]
            writers["actuators"].writerow(
                [f"{time_s:.3f}", INTERLOCK_NAMES[interlock], f"{vbat_mv / 1000:.3f}"]
                + [(solenoids >> n) & 1 for n in range(4)] + servos + [position, awake] + extra)

        elif kind == RECORD_LC_CALIBRATION:
            lc_zero, lc_newtons_per_code = LC_CALIBRATION.unpack(payload)
            writers["events"].writerow([f"{time_s:.3f}", f"lc1 calibration: zero {lc_zero}, "
                                                         f"{lc_newtons_per_code:.6g} N per code"])

        elif kind == RECORD_LC2_CALIBRATION:
            lc2_zero, lc2_newtons_per_code = LC_CALIBRATION.unpack(payload)
            writers["events"].writerow([f"{time_s:.3f}", f"lc2 calibration: zero {lc2_zero}, "
                                                         f"{lc2_newtons_per_code:.6g} N per code"])

        elif kind == RECORD_TEXT:
            writers["events"].writerow([f"{time_s:.3f}", payload.decode("ascii", "replace")])

        elif kind == RECORD_RADIO:
            state_count = state_heard = ""
            if len(payload) == RADIO_V5.size:
                *counters, rssi, snr, flags, valves, state_flags, state_count, state_heard_ms = RADIO_V5.unpack(payload)
                state_heard = f"{state_heard_ms / 1000:.3f}"
            else:
                *counters, rssi, snr, flags, valves, state_flags = RADIO.unpack(payload)
            heard = flags & RADIO_STATE_HEARD
            writers["radio"].writerow(
                [f"{time_s:.3f}"] + [int(bool(flags & bit)) for bit in (RADIO_LISTENING, RADIO_LINKED, RADIO_ABORTED)]
                + counters + [rssi if counters[0] else "", snr if counters[0] else ""]
                + [(state_flags >> n) & 1 if heard else "" for n in range(len(STATE_FLAGS))]
                + [(valves >> v) & 1 if heard else "" for v in range(len(VALVES))]
                + ([state_count, state_heard] if heard else ["", ""]))
            # Each change is an event, starting from link down and no STATE.
            for bit, on, off in ((RADIO_LINKED, "radio: link up", "radio: link down"),
                                 (RADIO_ABORTED, "radio: went safe by itself", "radio: following the launch box again")):
                if (flags ^ radio_flags) & bit:
                    writers["events"].writerow([f"{time_s:.3f}", on if flags & bit else off])
            if heard and (valves, state_flags) != radio_state:
                radio_state = (valves, state_flags)
                writers["events"].writerow([f"{time_s:.3f}", f"launch box STATE: {state_text(*radio_state)}"])
            radio_flags = flags

        elif kind == RECORD_HEALTH:
            dropped, peak, write_ms, stack_bytes, task, lc_misses, tc_setups, spi4_errors = HEALTH.unpack(payload)
            writers["health"].writerow([f"{time_s:.3f}", dropped, peak, write_ms, stack_bytes,
                                        task.split(b"\0")[0].decode("ascii", "replace"),
                                        lc_misses, tc_setups, spi4_errors])

        elif kind == RECORD_CONSOLE:
            # One printf's worth: whole lines, usually one.
            for line in payload.decode("ascii", "replace").replace("\r", "").split("\n"):
                if line:
                    writers["console"].writerow([f"{time_s:.3f}", line])

        elif kind == RECORD_NAMES:
            # The column headers keep the names the log started with.
            named = ", ".join(f"{label} = {name}" for label, name in
                              zip(CHANNELS, unpack_names(payload)) if name)
            writers["events"].writerow([f"{time_s:.3f}", f"names: {named or 'none'}"])

        else:
            print(f"  unknown record type {kind} at {time_s:.3f} s, skipped")

    for f in files.values():
        f.close()
    blocks = ", ".join(f"{n} {name}" for name, n in (
        ("PT", counts.get(RECORD_PT, 0)), ("LC", counts.get(RECORD_LC, 0)),
        ("LC2", counts.get(RECORD_LC2, 0)),
        ("TC", counts.get(RECORD_TC, 0)), ("actuator", counts.get(RECORD_ACTUATORS, 0)),
        ("radio", counts.get(RECORD_RADIO, 0)), ("health", counts.get(RECORD_HEALTH, 0))))
    print(f"{path.name}: {blocks} records -> {out}/")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, default=Path("."), help="where to put the folders")
    args = parser.parse_args()
    for path in args.logs:
        decode(path, args.out)


if __name__ == "__main__":
    main()
