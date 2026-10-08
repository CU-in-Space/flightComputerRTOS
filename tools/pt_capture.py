#!/usr/bin/env python3
"""Record the PT board's serial stream to a CSV file on this computer.

Shows every line from the board as it arrives and writes each sample to the
CSV with the computer's clock added in front:

    host_time,host_ms,board_ms,data_counts,supply_counts,data_mv,supply_mv,ratio,pressure_psi,status

  host_time  wall-clock time the line arrived (local, ms resolution)
  host_ms    computer's monotonic clock when the line arrived, in ms since
             the first sample. Can't jump if the system clock is adjusted.
  board_ms   the board's own clock (FreeRTOS tick) when it took the sample

Comparing host_ms with board_ms shows whether the timing is off: dropped or
late samples show as gaps on both clocks, a board that stalls shows long board
intervals, and a serial backlog shows host_ms falling behind board_ms. The
computer's clock includes USB delivery, which adds a few ms of jitter.

Gaps are flagged live while recording. Stop with Ctrl-C to get a timing
summary; with --plot it also draws the run next to the CSV.

Usage:
    python3 tools/pt_capture.py [--port PORT] [--out FILE.csv] [--plot]

Needs pyserial and matplotlib (pip install pyserial matplotlib). Close tio or
screen first; only one program can hold the port.
"""
import argparse
import csv
import statistics
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pt_log  # noqa: E402  (needs the path above)

STMICRO_USB_VENDOR_ID = 0x0483  # the NUCLEO's ST-LINK

# Live gap check: compare each interval against the median of recent ones.
RECENT_INTERVAL_COUNT = 50
MINIMUM_INTERVALS_BEFORE_CHECKING = 10


def find_stlink_port():
    ports = [port.device for port in list_ports.comports()
             if port.vid == STMICRO_USB_VENDOR_ID]
    # macOS lists both /dev/cu.* and /dev/tty.*; cu.* is the one for this.
    ports = sorted(ports, key=lambda device: "/tty." in device)
    if not ports:
        sys.exit("No ST-LINK serial port found. Is the board plugged in? "
                 "Pass --port to choose one.")
    return ports[0]


class GapWatcher:
    """Warns as soon as a sample arrives noticeably later than usual, on
    either clock. 'Usual' is the median of recent intervals, so nothing is
    assumed about the board's sample period."""

    def __init__(self):
        self.recent_board_intervals = deque(maxlen=RECENT_INTERVAL_COUNT)
        self.recent_host_intervals = deque(maxlen=RECENT_INTERVAL_COUNT)
        self.previous_row = None

    def check(self, row, sample_number):
        previous_row, self.previous_row = self.previous_row, row
        if previous_row is None:
            return
        board_interval = row["board_ms"] - previous_row["board_ms"]
        host_interval = row["host_ms"] - previous_row["host_ms"]
        if board_interval < 0:
            print(f"!! sample {sample_number}: board clock went backwards "
                  f"(board reset?)", file=sys.stderr)
            self.recent_board_intervals.clear()
            return

        warnings = []
        for name, interval, recent in (
                ("board", board_interval, self.recent_board_intervals),
                ("computer", host_interval, self.recent_host_intervals)):
            if len(recent) >= MINIMUM_INTERVALS_BEFORE_CHECKING:
                typical = statistics.median(recent)
                if interval > typical * pt_log.GAP_FACTOR:
                    warnings.append(f"{name} {interval:.0f} ms (usually {typical:.0f})")
            recent.append(interval)
        if warnings:
            print(f"!! sample {sample_number}: late sample: " + ", ".join(warnings),
                  file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="serial port (default: the ST-LINK's)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", type=Path,
                        help="CSV to write (default: data/pt_bench/pt_<date>_<time>.csv)")
    parser.add_argument("--plot", action="store_true", help="plot the run when stopped")
    arguments = parser.parse_args()

    port_name = arguments.port or find_stlink_port()
    csv_path = arguments.out or Path("data/pt_bench") / datetime.now().strftime(
        "pt_%Y%m%d_%H%M%S.csv")
    csv_path.parent.mkdir(parents=True, exist_ok=True)

    columns = pt_log.HOST_COLUMNS + pt_log.BOARD_COLUMNS
    rows = []
    first_sample_monotonic = None
    gap_watcher = GapWatcher()

    with serial.Serial(port_name, arguments.baud, timeout=1) as port, \
            csv_path.open("w", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(columns)
        print(f"Recording {port_name} -> {csv_path}  (Ctrl-C to stop)", file=sys.stderr)

        try:
            while True:
                raw_line = port.readline()
                # Stamp first, before any processing, so it's the arrival time.
                arrival_monotonic = time.monotonic()
                arrival_wall_clock = datetime.now()
                if not raw_line:
                    continue
                text_line = raw_line.decode(errors="replace").strip()
                if not text_line:
                    continue
                print(text_line)

                row = pt_log.parse_line(text_line)
                if row is None or row["board_ms"] is None:
                    continue  # comments, the header, partial or old-format lines
                if first_sample_monotonic is None:
                    first_sample_monotonic = arrival_monotonic
                row = {
                    "host_time": arrival_wall_clock.isoformat(timespec="milliseconds"),
                    "host_ms": round((arrival_monotonic - first_sample_monotonic) * 1000, 1),
                    **row,
                }
                writer.writerow(["" if row[column] is None else row[column]
                                 for column in columns])
                csv_file.flush()  # keep the file complete if this gets killed
                rows.append(row)
                gap_watcher.check(row, len(rows) - 1)
        except KeyboardInterrupt:
            pass

    print(f"\n{len(rows)} samples -> {csv_path}", file=sys.stderr)
    if rows:
        print("Timing:\n" + pt_log.timing_report(rows), file=sys.stderr)
    if arguments.plot and rows:
        png_path = csv_path.with_suffix(".png")
        pt_log.plot(rows, csv_path.stem, png_path)
        print(f"plot -> {png_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
