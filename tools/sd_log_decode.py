#!/usr/bin/env python3
"""Decode FLIGHT.LOG from the flight computer's SD card into CSV.

    python3 tools/sd_log_decode.py FLIGHT.LOG > flight.csv

Writes one row per logged value (session, t_ms, channel, value) to stdout and
a per-session summary to stderr. Standard library only.

The format is defined in src/tasks/sd_card_task.c; keep the two in sync.
Sectors are written strictly in order, so decoding stops at the first sector
that isn't a valid part of this file: that is where the data ends.
"""
import argparse
import csv
import struct
import sys
import zlib

SECTOR = 512
MAGIC = 0x474C4346  # "FCLG"
VERSION = 1
SECTOR_FILE, SECTOR_CHANNELS, SECTOR_DATA = 1, 2, 3
HEADER = struct.Struct("<IIIHBBI")  # magic, file_id, index, session, type, count, dropped
RECORD = struct.Struct("<IHHI")     # t_ms, channel, reserved, value
CHANNEL = struct.Struct("<16sB3x")  # name, type
TYPES = {1: "<f", 2: "<i"}          # SD_LOG_FLOAT, SD_LOG_INT32


def parse(sector, file_id, index):
    """Header fields of a valid sector, or None."""
    if len(sector) != SECTOR:
        return None
    magic, fid, idx, session, kind, count, dropped = HEADER.unpack_from(sector)
    crc, = struct.unpack_from("<I", sector, SECTOR - 4)
    if magic != MAGIC or zlib.crc32(sector[:-4]) != crc:
        return None
    if file_id is not None and (fid != file_id or idx != index):
        return None
    return fid, session, kind, count, dropped


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", help="FLIGHT.LOG copied from the SD card")
    args = ap.parse_args()

    out = csv.writer(sys.stdout, lineterminator="\n")
    out.writerow(["session", "t_ms", "channel", "value"])

    with open(args.log, "rb") as f:
        first = f.read(SECTOR)
        head = parse(first, None, 0)
        if head is None or head[2] != SECTOR_FILE:
            sys.exit("not a FLIGHT.LOG file (bad header sector)")
        file_id = head[0]
        version, = struct.unpack_from("<I", first, HEADER.size)
        if version != VERSION:
            sys.exit(f"format version {version}, this decoder reads {VERSION}")

        sessions = {}  # session -> summary
        names, types = [], []
        index = 1
        while True:
            sector = f.read(SECTOR)
            h = parse(sector, file_id, index)
            if h is None:
                break
            _, session, kind, count, dropped = h
            s = sessions.setdefault(session, {"records": 0, "dropped": 0, "first": None, "last": None})
            s["dropped"] = dropped
            if kind == SECTOR_CHANNELS:
                names, types = [], []
                for i in range(count):
                    raw, typ = CHANNEL.unpack_from(sector, HEADER.size + i * CHANNEL.size)
                    names.append(raw.split(b"\0", 1)[0].decode("ascii", "replace"))
                    types.append(typ)
                s["channels"] = names
            elif kind == SECTOR_DATA:
                for i in range(count):
                    t_ms, ch, _, raw = RECORD.unpack_from(sector, HEADER.size + i * RECORD.size)
                    if ch < len(names):
                        value, = struct.unpack(TYPES.get(types[ch], "<I"), struct.pack("<I", raw))
                        name = names[ch]
                    else:
                        value, name = raw, f"channel_{ch}"
                    out.writerow([session, t_ms, name, value])
                    s["records"] += 1
                    s["first"] = t_ms if s["first"] is None else s["first"]
                    s["last"] = t_ms
            index += 1

    print(f"{index - 1} sectors of data, {len(sessions)} session(s)", file=sys.stderr)
    for n, s in sessions.items():
        span = "" if s["first"] is None else f", t = {s['first']}..{s['last']} ms"
        print(f"  session {n}: {s['records']} records, {s['dropped']} dropped{span}, "
              f"channels {s.get('channels', [])}", file=sys.stderr)


if __name__ == "__main__":
    main()
