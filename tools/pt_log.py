#!/usr/bin/env python3
"""Convert PT board logs to CSV, plot them, and check their timing.

Accepts any of:
  - a CSV from tools/pt_capture.py              -> plotted only
  - a raw serial capture (tio, screen, or pasted into TextEdit and saved as
    .rtf) of the current firmware, which prints CSV rows itself
  - a capture of the older firmware, which printed
    "A0= 5931 A1=59496  sens= 497 mV  supply=4993 mV  ratio=0.0997  P=-0.20 psi"

and writes <name>.csv and <name>.png into the output directory.

Two clocks, never assumed:
  host_ms   the computer's clock when the line arrived. From pt_capture.py, or
            from tio's per-line timestamps (run tio with -t).
  board_ms  the board's own clock, printed by the current firmware.
A log that has neither is plotted against sample number and says so.

Usage:
    python3 tools/pt_log.py LOG [LOG ...] [--out DIR]

The pressure column is whatever the firmware computed with the calibration it
was flashed with at the time; the ratio column does not depend on it.
"""
import argparse
import csv
import re
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # write files only, never open a window
import matplotlib.pyplot as plt

# What the firmware prints per sample (src/tasks/pt_task.c).
BOARD_COLUMNS = [
    "board_ms", "data_counts", "supply_counts", "data_mv", "supply_mv",
    "ratio", "pressure_psi", "status",
]
# What pt_capture.py puts in front of them.
HOST_COLUMNS = ["host_time", "host_ms"]

INTEGER_COLUMNS = {"board_ms", "data_counts", "supply_counts", "data_mv", "supply_mv"}
FLOAT_COLUMNS = {"host_ms", "ratio", "pressure_psi"}

# Column names in CSVs written by earlier versions, mapped to the current ones.
OLD_COLUMN_NAMES = {
    "time_ms": "board_ms",
    "a0_code": "data_counts",
    "a1_code": "supply_counts",
    "sens_mv": "data_mv",
}

OLD_FORMAT_PATTERN = re.compile(
    r"A0=\s*(?P<data_counts>\d+)\s+A1=\s*(?P<supply_counts>\d+)\s+"
    r"sens=\s*(?P<data_mv>\d+)\s*mV\s+supply=\s*(?P<supply_mv>\d+)\s*mV\s+"
    r"(?:ratio=(?P<ratio>-?[\d.]+)\s+P=(?P<pressure_psi>-?[\d.]+)\s*psi"
    r"|(?P<no_supply>no sensor supply))"
)
NEW_FORMAT_PATTERN = re.compile(r"^\d+,[\d,.\-]*,[a-z_]+$")

# tio -t puts "[15:06:05.138] " in front of every line.
TIO_TIMESTAMP_PATTERN = re.compile(
    r"^\[(?P<hours>\d{2}):(?P<minutes>\d{2}):(?P<seconds>\d{2})\.(?P<millis>\d{3})\]\s*")
MS_PER_DAY = 24 * 60 * 60 * 1000

# An interval this many times the typical one counts as a gap (missed samples
# or a stall). The typical interval is measured from the log, not assumed.
GAP_FACTOR = 1.5


def convert_field(column, text):
    if text is None or text == "":
        return None
    if column in INTEGER_COLUMNS:
        return int(text)
    if column in FLOAT_COLUMNS:
        return float(text)
    return text


def split_tio_timestamp(text_line):
    """Returns (milliseconds since midnight or None, rest of the line)."""
    match = TIO_TIMESTAMP_PATTERN.match(text_line)
    if match is None:
        return None, text_line
    ms_of_day = (((int(match["hours"]) * 60 + int(match["minutes"])) * 60
                  + int(match["seconds"])) * 1000 + int(match["millis"]))
    return ms_of_day, text_line[match.end():]


def parse_line(text_line):
    """One line of serial output -> dict keyed by BOARD_COLUMNS, or None for
    anything that isn't a sample (comments, the header, partial lines, tio
    banners, RTF markup). Old-format rows have board_ms = None."""
    # RTF ends every line with a backslash.
    text_line = text_line.strip().rstrip("\\").strip()

    if NEW_FORMAT_PATTERN.match(text_line):
        fields = text_line.split(",")
        if len(fields) == len(BOARD_COLUMNS):
            return {column: convert_field(column, field)
                    for column, field in zip(BOARD_COLUMNS, fields)}
        return None

    match = OLD_FORMAT_PATTERN.search(text_line)
    if match is None:
        return None
    has_supply = match["no_supply"] is None
    return {
        "board_ms": None,
        "data_counts": int(match["data_counts"]),
        "supply_counts": int(match["supply_counts"]),
        "data_mv": int(match["data_mv"]),
        "supply_mv": int(match["supply_mv"]),
        "ratio": float(match["ratio"]) if has_supply else None,
        "pressure_psi": float(match["pressure_psi"]) if has_supply else None,
        "status": "ok" if has_supply else "no_supply",
    }


def load(path):
    """Rows from a CSV written by this tool or pt_capture.py, or from a raw
    serial capture in either firmware format. Every row has host_ms and
    board_ms keys; either may be None if the log didn't record it."""
    if path.suffix.lower() == ".csv":
        with path.open(newline="") as csv_file:
            rows = []
            for row in csv.DictReader(csv_file):
                renamed = {OLD_COLUMN_NAMES.get(column, column): value
                           for column, value in row.items()}
                converted = {column: convert_field(column, value)
                             for column, value in renamed.items()}
                converted.setdefault("host_ms", None)
                rows.append(converted)
            return rows

    rows = []
    first_ms_of_day = None
    days_passed = 0
    previous_ms_of_day = None
    for text_line in path.read_text(errors="replace").splitlines():
        ms_of_day, text_line = split_tio_timestamp(text_line.strip())
        row = parse_line(text_line)
        if row is None:
            continue
        host_ms = None
        if ms_of_day is not None:
            if previous_ms_of_day is not None and ms_of_day < previous_ms_of_day:
                days_passed += 1  # the capture ran past midnight
            previous_ms_of_day = ms_of_day
            ms_of_day += days_passed * MS_PER_DAY
            if first_ms_of_day is None:
                first_ms_of_day = ms_of_day
            host_ms = float(ms_of_day - first_ms_of_day)
        rows.append({"host_ms": host_ms, **row})
    return rows


def write_csv(rows, csv_path):
    columns = [column for column in HOST_COLUMNS if column in rows[0]]
    columns += BOARD_COLUMNS
    with csv_path.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({key: ("" if value is None else value) for key, value in row.items()})


def has_clock(rows, column):
    return all(row.get(column) is not None for row in rows)


def intervals(rows, column):
    """Time between consecutive samples on one clock, in ms. A drop in the
    board clock means the board rebooted; that interval is left out."""
    differences = []
    for previous, current in zip(rows, rows[1:]):
        difference = current[column] - previous[column]
        differences.append(difference if difference >= 0 else None)
    return differences


def describe_intervals(name, differences):
    values = [difference for difference in differences if difference is not None]
    if not values:
        return [f"  {name}: not enough samples"]
    typical = statistics.median(values)
    gaps = [value for value in values if value > typical * GAP_FACTOR]
    lines = [
        f"  {name}: median {typical:.1f} ms, mean {statistics.fmean(values):.2f} ms, "
        f"min {min(values):.1f}, max {max(values):.1f}, "
        f"std dev {statistics.pstdev(values):.2f} ms",
        f"    gaps over {GAP_FACTOR:g}x median: {len(gaps)}"
        + (f" (longest {max(gaps):.0f} ms)" if gaps else ""),
    ]
    resets = differences.count(None)
    if resets:
        lines.append(f"    clock went backwards {resets}x (board reset?)")
    return lines


def timing_report(rows):
    """Plain-text summary of how regular the samples were, on each clock."""
    lines = []
    has_host = has_clock(rows, "host_ms")
    has_board = has_clock(rows, "board_ms")
    if has_host:
        lines += describe_intervals("computer clock", intervals(rows, "host_ms"))
    if has_board:
        lines += describe_intervals("board clock   ", intervals(rows, "board_ms"))
    if has_host and has_board and len(rows) > 1:
        host_span = rows[-1]["host_ms"] - rows[0]["host_ms"]
        board_span = rows[-1]["board_ms"] - rows[0]["board_ms"]
        if host_span > 0 and board_span > 0:
            drift_ppm = (board_span - host_span) / host_span * 1e6
            lines.append(f"  over {host_span / 1000:.1f} s the board clock ran "
                         f"{board_span - host_span:+.0f} ms vs the computer "
                         f"({drift_ppm:+.0f} ppm; USB adds a few ms of jitter to "
                         f"the computer clock, so this only means much over minutes)")
    if not lines:
        lines.append("  no timestamps in this log: timing can't be checked")
    return "\n".join(lines)


def plot(rows, title, png_path):
    """Pressure, ratio, connector voltages and sample-to-sample interval.
    The x axis is the computer's clock if the log has it, else the board's,
    else plain sample number. Missing values leave gaps."""
    if has_clock(rows, "host_ms"):
        x_column, x_label = "host_ms", "Computer time since first sample (s)"
    elif has_clock(rows, "board_ms"):
        x_column, x_label = "board_ms", "Board time since first sample (s)"
    else:
        x_column, x_label = None, "Sample number (no timestamps in this log)"

    if x_column is None:
        x_values = list(range(len(rows)))
    else:
        first = rows[0][x_column]
        x_values = [(row[x_column] - first) / 1000 for row in rows]

    has_timing = has_clock(rows, "host_ms") or has_clock(rows, "board_ms")
    panel_count = 4 if has_timing else 3
    figure, axes = plt.subplots(panel_count, 1, sharex=True,
                                figsize=(11, 2.6 * panel_count), constrained_layout=True)
    figure.suptitle(title)
    pressure_axis, ratio_axis, voltage_axis = axes[:3]

    pressure_axis.plot(x_values, [row["pressure_psi"] for row in rows],
                       color="tab:blue", linewidth=1)
    pressure_axis.set_ylabel("Pressure (psi)\nas computed on board")

    ratio_axis.plot(x_values, [row["ratio"] for row in rows], color="tab:purple", linewidth=1)
    ratio_axis.set_ylabel("Ratio\nVout / Vsupply")

    def volts(row, column):
        return None if row[column] is None else row[column] / 1000

    voltage_axis.plot(x_values, [volts(row, "supply_mv") for row in rows],
                      label="supply (A1)", color="tab:orange", linewidth=1)
    voltage_axis.plot(x_values, [volts(row, "data_mv") for row in rows],
                      label="sensor out (A0)", color="tab:green", linewidth=1)
    voltage_axis.set_ylabel("At connector (V)")
    voltage_axis.legend(loc="upper right")

    if has_timing:
        interval_axis = axes[3]
        # Steps, not a slope: each interval spans the gap between its two
        # samples, so it's drawn flat across that gap.
        if has_clock(rows, "host_ms"):
            interval_axis.plot(x_values[1:], intervals(rows, "host_ms"),
                               label="computer clock", color="tab:gray", linewidth=1,
                               drawstyle="steps-pre")
        if has_clock(rows, "board_ms"):
            interval_axis.plot(x_values[1:], intervals(rows, "board_ms"),
                               label="board clock", color="tab:red", linewidth=1,
                               drawstyle="steps-pre")
        interval_axis.set_ylabel("Time since previous\nsample (ms)")
        interval_axis.legend(loc="upper right")

    for axis in axes:
        axis.grid(True, alpha=0.3)
    axes[-1].set_xlabel(x_label)

    figure.savefig(png_path, dpi=150)
    plt.close(figure)


def file_name_for(path):
    return re.sub(r"[^a-z0-9]+", "_", path.stem.lower()).strip("_")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, default=Path("data/pt_bench"))
    arguments = parser.parse_args()

    arguments.out.mkdir(parents=True, exist_ok=True)
    for log_path in arguments.logs:
        rows = load(log_path)
        if not rows:
            print(f"{log_path}: no PT samples found, skipped")
            continue

        name = file_name_for(log_path)
        png_path = arguments.out / f"{name}.png"
        outputs = [png_path]
        if log_path.suffix.lower() != ".csv":
            csv_path = arguments.out / f"{name}.csv"
            write_csv(rows, csv_path)
            outputs.insert(0, csv_path)
        plot(rows, log_path.stem.strip(), png_path)

        valid = sum(1 for row in rows if row["status"] == "ok")
        print(f"{log_path.name}: {len(rows)} samples ({valid} ok) -> "
              + ", ".join(str(output) for output in outputs))
        print(timing_report(rows))


if __name__ == "__main__":
    main()
