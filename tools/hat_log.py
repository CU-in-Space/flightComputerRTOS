#!/usr/bin/env python3
"""Decode a Sensor HAT data log (LOGnnnn.BIN from the SD card) into CSV files.

Writes, into a folder named after the log:
  pt.csv         every PT sample (1 kHz): raw counts, ratio and psi
  lc.csv         every load-cell sample (1 kHz): raw code, mV/V and newtons
  tc.csv         the four thermocouples, every 100 ms
  actuators.csv  interlock, VBAT and every output, every 10 ms
  events.csv     console commands and load-cell calibrations

The format is src/app/log_records.h; change both together. Times are seconds
since the board booted. Psi and newtons use the calibration recorded in the file,
so a file decodes the same however the firmware has changed since.

Usage:
    python3 tools/hat_log.py LOG0001.BIN [LOG0002.BIN ...] [--out DIR]
"""
import argparse
import csv
import struct
from pathlib import Path

HEADER = struct.Struct("<BBI")              # type, payload length, time_ms
FILE_START = struct.Struct("<4sH" + "fff" * 2 + "if")
PT = struct.Struct("<" + "H" * 40)          # 10 samples x 2 PTs x (supply, output)
LC = struct.Struct("<" + "i" * 10)
TC = struct.Struct("<" + "BBff" * 4)
ACTUATORS = struct.Struct("<BIB4HiB")
LC_CALIBRATION = struct.Struct("<if")

RECORD_FILE_START, RECORD_PT, RECORD_LC, RECORD_TC, RECORD_ACTUATORS, \
    RECORD_LC_CALIBRATION, RECORD_TEXT = range(1, 8)

SIZES = {
    RECORD_FILE_START: FILE_START.size,
    RECORD_PT: PT.size,
    RECORD_LC: LC.size,
    RECORD_TC: TC.size,
    RECORD_ACTUATORS: ACTUATORS.size,
    RECORD_LC_CALIBRATION: LC_CALIBRATION.size,
}

FORMAT_VERSION = 1
INTERLOCK_NAMES = ["RECOVERING", "CABLE_FAULT", "ENABLED"]
SAMPLES_PER_BLOCK = 10
ADS1220_FULL_SCALE = 2 ** 23
ADS1220_GAIN = 128
PT_MIN_SUPPLY_COUNTS = 65535 // 5           # as src/app/pt_convert.c


def records(data):
    """Yields (type, time_ms, payload). Stops at a record cut short by a power loss."""
    offset = 0
    while offset + HEADER.size <= len(data):
        kind, length, time_ms = HEADER.unpack_from(data, offset)
        offset += HEADER.size
        if offset + length > len(data):
            print(f"  last record cut short at byte {offset - HEADER.size}, ignored")
            return
        if kind in SIZES and SIZES[kind] != length:
            raise ValueError(f"record type {kind} at byte {offset - HEADER.size} has "
                             f"{length} bytes, expected {SIZES[kind]}")
        yield kind, time_ms, data[offset:offset + length]
        offset += length


def sample_times(time_ms):
    """The block's samples were 1 ms apart and the last was taken just before time_ms."""
    return [(time_ms - (SAMPLES_PER_BLOCK - 1 - n)) / 1000 for n in range(SAMPLES_PER_BLOCK)]


def blank_or(value, digits):
    return "" if value is None else f"{value:.{digits}f}"


def decode(path, out_dir):
    data = path.read_bytes()
    out = out_dir / path.stem
    out.mkdir(parents=True, exist_ok=True)

    files = {name: open(out / f"{name}.csv", "w", newline="")
             for name in ("pt", "lc", "tc", "actuators", "events")}
    writers = {name: csv.writer(f) for name, f in files.items()}
    writers["pt"].writerow(["time_s"] + [f"pt{p}_{c}" for p in (1, 2)
                                         for c in ("supply_counts", "data_counts", "ratio", "psi")])
    writers["lc"].writerow(["time_s", "code", "mv_per_v", "newtons"])
    writers["tc"].writerow(["time_s"] + [f"tc{t}_{c}" for t in range(1, 5)
                                         for c in ("c", "cold_junction_c", "faults")])
    writers["actuators"].writerow(["time_s", "interlock", "vbat_v"]
                                  + [f"sol{n}" for n in range(1, 5)]
                                  + [f"srv{n}_us" for n in range(1, 5)]
                                  + ["stepper_position", "stepper_awake"])
    writers["events"].writerow(["time_s", "event"])

    pt_calibrations = None
    lc_zero, lc_newtons_per_code = 0, 0.0
    counts = {}

    for kind, time_ms, payload in records(data):
        counts[kind] = counts.get(kind, 0) + 1
        time_s = time_ms / 1000

        if pt_calibrations is None and kind != RECORD_FILE_START:
            raise ValueError("file doesn't start with a file-start record")

        if kind == RECORD_FILE_START:
            fields = FILE_START.unpack(payload)
            if fields[0] != b"HATL" or fields[1] != FORMAT_VERSION:
                raise ValueError(f"not a version {FORMAT_VERSION} HAT log")
            pt_calibrations = [fields[2:5], fields[5:8]]
            lc_zero, lc_newtons_per_code = fields[8], fields[9]

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

        elif kind == RECORD_TC:
            values = TC.unpack(payload)
            row = [f"{time_s:.3f}"]
            for t in range(4):
                present, faults, temp_c, cold_c = values[t * 4:t * 4 + 4]
                row += [f"{temp_c:.2f}", f"{cold_c:.2f}", faults] if present else ["", "", ""]
            writers["tc"].writerow(row)

        elif kind == RECORD_ACTUATORS:
            interlock, vbat_mv, solenoids, *rest = ACTUATORS.unpack(payload)
            servos, (position, awake) = rest[:4], rest[4:]
            writers["actuators"].writerow(
                [f"{time_s:.3f}", INTERLOCK_NAMES[interlock], f"{vbat_mv / 1000:.3f}"]
                + [(solenoids >> n) & 1 for n in range(4)] + servos + [position, awake])

        elif kind == RECORD_LC_CALIBRATION:
            lc_zero, lc_newtons_per_code = LC_CALIBRATION.unpack(payload)
            writers["events"].writerow([f"{time_s:.3f}", f"lc calibration: zero {lc_zero}, "
                                                         f"{lc_newtons_per_code:.6g} N per code"])

        elif kind == RECORD_TEXT:
            writers["events"].writerow([f"{time_s:.3f}", payload.decode("ascii", "replace")])

        else:
            print(f"  unknown record type {kind} at {time_s:.3f} s, skipped")

    for f in files.values():
        f.close()
    blocks = ", ".join(f"{n} {name}" for name, n in (
        ("PT", counts.get(RECORD_PT, 0)), ("LC", counts.get(RECORD_LC, 0)),
        ("TC", counts.get(RECORD_TC, 0)), ("actuator", counts.get(RECORD_ACTUATORS, 0))))
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
