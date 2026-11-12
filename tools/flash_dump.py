#!/usr/bin/env python3
"""Copy a run out of the Sensor HAT's backup flash into a log file.

Sends `flash dump` to the console, collects the checksummed frames that come
back and writes RUNnnnn.BIN, the same format as an SD log, so hat_log.py
decodes it. The frame and block layouts are in src/app/log_records.h; change
both together.

The HAT needs power (battery connected, interlock ENABLED). At 115200 baud a
5-minute run takes about 7 minutes. Ctrl-C stops the board too.

Usage:
    python3 tools/flash_dump.py [--port PORT] [--run N] [--out FILE]

Needs pyserial (pip install pyserial). Close tio or screen first; only one
program can hold the port.
"""
import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hat_log import HEADER, RECORD_TEXT, SIZES  # noqa: E402  (needs the path above)

SYNC = bytes([0xA5, 0x5A])
FRAME = struct.Struct("<2sBI116sI")         # sync, length, offset, data, crc
BLOCK_HEADER = struct.Struct("<4sII")       # magic, run, sequence
BLOCK_SIZE = 65536
PAGE_SIZE = 256
BAUD = 115200
SILENCE_LIMIT_S = 10


def frames(stream, show_text):
    """Yields (offset, data, finished) for each good frame in a growing byte buffer.

    stream is a bytearray; frames are removed from it as they're found. Anything
    between frames is console text, passed to show_text.
    """
    while True:
        at = stream.find(SYNC)
        if at < 0:
            # The last byte may be the first half of a sync.
            keep = 1 if stream[-1:] == SYNC[:1] else 0
            text = len(stream) - keep
            if text:
                show_text(bytes(stream[:text]))
                del stream[:text]
            return
        if at:
            show_text(bytes(stream[:at]))
            del stream[:at]
        if len(stream) < FRAME.size:
            return

        _, length, offset, data, crc = FRAME.unpack_from(stream)
        if zlib.crc32(bytes(stream[len(SYNC):FRAME.size - 4])) != crc:
            # A sync pattern that wasn't one, or a damaged frame.
            del stream[0]
            continue
        del stream[:FRAME.size]
        yield offset, data[:length], length == 0


def assemble(chunks, show_text):
    """Builds the dump from an iterable of received byte chunks."""
    stream = bytearray()
    dump = bytearray()
    for chunk in chunks:
        stream += chunk
        for offset, data, finished in frames(stream, show_text):
            if offset != len(dump):
                sys.exit(f"\nlost data at byte {len(dump)} (next frame was {offset}). Run it again.")
            if finished:
                return dump
            dump += data
    sys.exit("\nthe dump never finished. Run it again.")


def records_of(block):
    """The records in one block, without its header or the blank end."""
    magic, _, _ = BLOCK_HEADER.unpack_from(block)
    if magic != b"HATB":
        sys.exit("a block in the dump has no header")

    start = offset = BLOCK_HEADER.size
    while offset + HEADER.size <= len(block):
        kind, length, _ = HEADER.unpack_from(block, offset)
        end = offset + HEADER.size + length
        # Blank flash, or the page a power cut interrupted.
        if end > len(block) or not (SIZES.get(kind) == length or kind == RECORD_TEXT):
            break
        # Runs into a page that was never written: a power cut lost the rest of it.
        # To the end of the block, since a whole record can hold FF bytes.
        page_end = (offset | (PAGE_SIZE - 1)) + 1
        if end > page_end and not block[page_end:].strip(b"\xff"):
            break
        offset = end
    return block[start:offset]


def to_log(dump):
    """A run's raw blocks, as the record stream of one log file."""
    return b"".join(records_of(dump[at:at + BLOCK_SIZE]) for at in range(0, len(dump), BLOCK_SIZE))


def received(link):
    """Yields what the port receives, until it goes silent."""
    last = time.monotonic()
    while time.monotonic() - last < SILENCE_LIMIT_S:
        chunk = link.read(link.in_waiting or 1)
        if chunk:
            last = time.monotonic()
            yield chunk


def find_stlink_port():
    from serial.tools import list_ports
    ports = [port.device for port in list_ports.comports() if port.vid == 0x0483]
    # macOS lists both /dev/cu.* and /dev/tty.*; cu.* is the one for this.
    ports = sorted(ports, key=lambda device: "/tty." in device)
    if not ports:
        sys.exit("No ST-LINK serial port found. Is the board plugged in? Pass --port to choose one.")
    return ports[0]


def main():
    import serial

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="serial port (default: the ST-LINK's)")
    parser.add_argument("--run", type=int, help="which run (default: the newest)")
    parser.add_argument("--out", type=Path, help="output file (default: RUNnnnn.BIN)")
    args = parser.parse_args()

    command = "flash dump" + (f" {args.run}" if args.run else "") + "\r"

    def show_text(text):
        print(text.decode("ascii", "replace"), end="", flush=True)

    with serial.Serial(args.port or find_stlink_port(), BAUD, timeout=0.2) as link:
        link.reset_input_buffer()
        link.write(command.encode())
        try:
            dump = assemble(received(link), show_text)
        except KeyboardInterrupt:
            link.write(b" ")
            sys.exit("\nstopped")

    _, run, _ = BLOCK_HEADER.unpack_from(dump)
    log = to_log(dump)
    out = args.out or Path(f"RUN{run:04d}.BIN")
    out.write_bytes(log)
    print(f"\nrun {run}: {len(dump) // BLOCK_SIZE} blocks, {len(log)} bytes of records -> {out}")
    print(f"decode it with: python3 tools/hat_log.py {out}")


if __name__ == "__main__":
    main()
