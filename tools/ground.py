#!/usr/bin/env python3
"""Record the stands' telemetry on a laptop and send them commands, through the ground link.

The ground link is a Nano with an RFM95 running tools/ground_link/ground_link.ino,
plugged into this computer. It hears every packet on the channel. This script
decodes them and writes, into a new folder named after the time it started:

  stand1.csv, stand2.csv  one row per telemetry packet from that stand
  runs.csv                one row per data log run a stand made while this ran:
                          its SD file, when it started and stopped
  box.csv                 one row per change in what the launch box sends
  link.csv                one row a second: how many packets asked each stand for an
                          answer, how many it answered and how fast, damaged packets
  events.csv              commands, names, log starts and stops, restarts

It records from start to quit, whatever the stands are doing. A row's log_file
(LOG0007) is the SD file the stand was writing, and its stand_time_s is the
time_s that hat_log.py gives the same moment in that file.

Type:
  1 log start     any console line, to stand 1 (or 2)
  poll 2          ask stand 2 for one telemetry packet
  stop            log stop to every stand
  show            a line for every packet, on or off
  rate            a line a second, as link.csv gets it, on or off
  (Enter)         the latest reading from each stand
  quit            or Ctrl-C: close the files

The packet layout is src/app/radio_link.h; change both together.

Usage:
    python3 tools/ground.py [--port PORT] [--out DIR] [--power DBM] [--poll [STAND ...]]

Needs pyserial (pip install pyserial).
"""
import argparse
import csv
import queue
import struct
import sys
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path

BAUD = 115200
NETWORK_ID = 0x4355
PROTOCOL_VERSION = 5
NODE_BOX = 0
STAND_MAX = 2
POLL, COMMAND, SAFE, TELEMETRY, STATE = range(1, 6)

ADDRESS = struct.Struct("<BBBB")                # to, from, sequence, spare
TAG = struct.Struct("<HBB")                     # network, version, type
STATE_PACKET = struct.Struct("<HBB BB")         # tag, valves, flags
TELEMETRY_PACKET = struct.Struct("<HBB I H BBBB 4H H 2f 2f 2i 4f I H bb B 12s")
assert STATE_PACKET.size == 6 and TELEMETRY_PACKET.size == 85

VALID_PT1, VALID_LC1, VALID_LC1_CALIBRATED, VALID_TC1 = 1 << 0, 1 << 2, 1 << 3, 1 << 4
VALID_LC2, VALID_LC2_CALIBRATED = 1 << 8, 1 << 9
FLAG_LOGGING, FLAG_ABORTED = 1 << 0, 1 << 1
STATE_ARMED, STATE_ABORT, STATE_IGNITER_1, STATE_IGNITER_2, STATE_FIRE_SEQUENCE = (1 << n for n in range(5))

VALVES = ["detach", "supply", "tank", "vent", "line_vent", "mov", "dump"]
STATE_FLAGS = ["armed", "abort", "igniter 1", "igniter 2", "fire sequence"]
KINDS = {POLL: "POLL", COMMAND: "COMMAND", SAFE: "SAFE", TELEMETRY: "TELEMETRY", STATE: "STATE"}
STANDS = range(1, STAND_MAX + 1)
# The name channels, in the order of RADIO_NAME_CHANNELS.
CHANNELS = ["pt1", "pt2", "tc1", "tc2", "tc3", "tc4", "lc1", "lc2",
            "sol1", "sol2", "sol3", "sol4", "srv1", "srv2", "srv3", "srv4"]

BOX_QUIET_S = 1.0       # no STATE for this long: the launch box is off the air
STAND_QUIET_S = 3.0     # with the launch box on the air, a stand this quiet is reported
RESTART_MS = 1000       # a stand's clock going back more than this: it restarted
VERSION_REPORT_S = 30   # how often to say a sender is on another protocol
REQUEST_WAIT_S = 5      # the ground link answers every line well within this
POLL_PERIOD_S = 0.25    # --poll: one stand polled this often, in turn
READY_WAIT_S = 4        # the Nano restarts when the port opens
ANSWER_WAIT_S = 0.3     # an ask with no answer by now went unanswered
RECENT_S = 10           # the snapshot's link line covers this long

RESULTS = {
    "ok": "took it",
    "failed": "didn't take it: no acknowledgement",
    "busy": "not sent: the launch box is on the air, and sending would collide with it",
    "toolong": "not sent: longer than 60 characters",
    "usage": "not sent: the ground link didn't understand it",
    "noanswer": "no answer from the ground link",
}


def log_name(number):
    return f"LOG{number:04d}" if number else ""


def blank_or(value, digits):
    return "" if value is None else f"{value:.{digits}f}"


def blank_or_int(value):
    return "" if value is None else value


def mean(values):
    return sum(values) / len(values) if values else None


def node(number):
    return "box" if number == NODE_BOX else "all" if number == 0xFF else str(number)


def state_text(valves, flags):
    """A STATE as the launch box and the stand's `radio` print it: valves 0001010, armed."""
    text = "valves " + "".join(str((valves >> v) & 1) for v in range(len(VALVES)))
    text += "".join(f", {name}" for n, name in enumerate(STATE_FLAGS) if flags & (1 << n))
    return text if flags & (STATE_ARMED | STATE_ABORT) else text + ", key off"


def detail(kind, sequence, data):
    """What a packet from node 0 carries, as text."""
    if kind == STATE and len(data) == STATE_PACKET.size:
        _, _, _, valves, flags = STATE_PACKET.unpack(data)
        return state_text(valves, flags)
    if kind == COMMAND:
        return f"#{sequence} \"{data[TAG.size:].decode('ascii', 'replace')}\""
    if kind == SAFE:
        return f"#{sequence}"
    return ""


class Second:
    """What was heard in one second: a row of link.csv."""

    def __init__(self):
        self.box = 0                  # packets heard from the launch box (node 0)
        self.laptop = 0               # sent by this ground link
        self.asked = {s: 0 for s in STANDS}
        self.answered = {s: 0 for s in STANDS}
        self.answer_ms = {s: [] for s in STANDS}
        self.dbm = {s: [] for s in STANDS}
        self.unasked = 0              # telemetry with no ask heard before it
        self.damaged = 0
        self.other_network = 0
        self.other_version = 0


LINK_HEADER = (["pc_time", "session_s", "box_packets", "laptop_packets"]
               + [f"stand{s}_{c}" for s in STANDS for c in ("asked", "answered", "answer_ms", "dbm")]
               + ["unasked", "damaged", "other_network", "other_version"])


class Telemetry:
    """One telemetry packet, decoded."""

    def __init__(self, data):
        (_, _, _, self.time_ms, self.valid, self.flags, self.solenoids, self.valves_mapped,
         self.valves, *rest) = TELEMETRY_PACKET.unpack(data)
        self.servo_us, self.vbat_mv = rest[0:4], rest[4]
        self.pt_psi, self.lc_newtons, self.lc_code, self.tc_c = rest[5:7], rest[7:9], rest[9:11], rest[11:15]
        (self.log_bytes, self.log_file, self.rssi_dbm, self.snr_db,
         self.name_channel, name) = rest[15:]
        self.name = name.split(b"\0")[0].decode("ascii", "replace")
        self.logging = bool(self.flags & FLAG_LOGGING)
        self.aborted = bool(self.flags & FLAG_ABORTED)

    def pt(self, i):
        return self.pt_psi[i] if self.valid & (VALID_PT1 << i) else None

    def tc(self, i):
        return self.tc_c[i] if self.valid & (VALID_TC1 << i) else None

    def lc(self, i):
        return self.lc_code[i] if self.valid & (VALID_LC1, VALID_LC2)[i] else None

    def newtons(self, i):
        return self.lc_newtons[i] if self.valid & (VALID_LC1_CALIBRATED, VALID_LC2_CALIBRATED)[i] else None

    def valve(self, v):
        """1 open, 0 closed, None if this stand doesn't drive it."""
        return (self.valves >> v) & 1 if (self.valves_mapped >> v) & 1 else None

    def run(self):
        """The run's name: the SD file's."""
        return log_name(self.log_file)


class Run:
    """One data log run of one stand, as heard from here."""

    def __init__(self, stand, t, pc_time):
        self.stand = stand.number
        self.boot = stand.boot
        self.log_file = t.log_file
        self.started = pc_time
        self.stopped = ""
        self.first_s = self.last_s = t.time_ms / 1000
        self.rows = 0
        self.log_bytes = 0
        self.ended = "still recording"

    def same(self, t):
        return t.log_file == self.log_file

    def name(self):
        return log_name(self.log_file)

    def row(self):
        return [self.stand, log_name(self.log_file), self.started, self.stopped,
                self.boot, f"{self.first_s:.3f}", f"{self.last_s:.3f}", self.rows, self.log_bytes, self.ended]


RUNS_HEADER = ["stand", "log_file", "started", "stopped", "boot", "first_stand_time_s",
               "last_stand_time_s", "telemetry_rows", "log_bytes", "ended"]


class Stand:
    def __init__(self, number, folder):
        self.number = number
        self.path = folder / f"stand{number}.csv"
        self.file = self.writer = None   # opened once all the names are in
        self.names = [None] * len(CHANNELS)
        self.held = []                   # rows waiting for the names
        self.last = None                 # the last Telemetry
        self.heard = 0.0                 # when, time.monotonic()
        self.rssi = 0
        self.quiet = False
        self.boot = 1                    # restarts seen, plus one
        self.run = None
        self.rows = 0

    def label(self, channel):
        name = self.names[CHANNELS.index(channel)]
        return f"{channel}_{name}" if name else channel

    def header(self):
        label = self.label
        return (["pc_time", "session_s", "boot", "stand_time_s", "log_file", "log_bytes",
                 "logging", "aborted", "rssi_dbm", "snr_db", "stand_heard_dbm", "stand_heard_snr_db", "vbat_v"]
                + [f"{label(c)}_psi" for c in ("pt1", "pt2")]
                + [f"{label(c)}_c" for c in ("tc1", "tc2", "tc3", "tc4")]
                + [f"{label(c)}_{unit}" for c in ("lc1", "lc2") for unit in ("n", "code")]
                + [label(c) for c in ("sol1", "sol2", "sol3", "sol4")]
                + [f"{label(c)}_us" for c in ("srv1", "srv2", "srv3", "srv4")]
                + [f"valve_{v}" for v in VALVES])

    def row(self, t, rssi, snr, pc_time, session_s):
        return ([pc_time, f"{session_s:.3f}", self.boot, f"{t.time_ms / 1000:.3f}",
                 log_name(t.log_file), t.log_bytes if t.logging else "",
                 int(t.logging), int(t.aborted), rssi, snr, t.rssi_dbm, t.snr_db, f"{t.vbat_mv / 1000:.3f}"]
                + [blank_or(t.pt(i), 2) for i in range(2)]
                + [blank_or(t.tc(i), 2) for i in range(4)]
                + [x for i in range(2) for x in (blank_or(t.newtons(i), 2), blank_or_int(t.lc(i)))]
                + [(t.solenoids >> i) & 1 for i in range(4)]
                + list(t.servo_us)
                + [blank_or_int(t.valve(v)) for v in range(len(VALVES))])

    def write(self, row):
        if self.writer is None:
            self.held.append(row)
        else:
            self.writer.writerow(row)
        self.rows += 1

    def open(self):
        """Writes the header with the names as they are, and the rows held for it."""
        self.file = open(self.path, "w", newline="", buffering=1)
        self.writer = csv.writer(self.file)
        self.writer.writerow(self.header())
        self.writer.writerows(self.held)
        self.held = []


def open_csv(path, header):
    f = open(path, "w", newline="", buffering=1)
    writer = csv.writer(f)
    writer.writerow(header)
    return f, writer


class Session:
    """Everything heard since the script started, and the files it goes into."""

    def __init__(self, folder):
        self.folder = folder
        self.start = time.monotonic()
        self.lock = threading.RLock()
        self.stands = {}
        self.runs = []
        self.box_state = None        # (valves, flags) last written to box.csv
        self.box_heard = 0.0
        self.box_on_air = False
        self.version_reported = {}   # sender -> when
        self.events_file, self.events = open_csv(folder / "events.csv",
                                                 ["pc_time", "session_s", "stand", "stand_time_s", "event"])
        self.box_file, self.box = open_csv(folder / "box.csv",
                                           ["pc_time", "session_s", "armed", "abort", "igniter_1", "igniter_2",
                                            "fire_sequence"] + [f"valve_{v}" for v in VALVES])
        self.link_file, self.link = open_csv(folder / "link.csv", LINK_HEADER)
        self.write_runs()
        self.showing = False         # a line for every packet
        self.rating = False          # a line a second
        self.second = Second()
        self.second_start = self.start
        self.recent = deque(maxlen=RECENT_S)
        self.pending = {}            # stand -> (ground link ms, monotonic) of the ask it hasn't answered yet
        self.damaged_total = 0       # from the ground link's B lines
        self.damaged_counted = 0

    def times(self):
        now = datetime.now()
        return now.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3], time.monotonic() - self.start, now

    def event(self, stand, t, text):
        with self.lock:
            pc_time, session_s, now = self.times()
            stand_time = f"{t.time_ms / 1000:.3f}" if t else ""
            self.events.writerow([pc_time, f"{session_s:.3f}", stand or "", stand_time, text])
            print(f"{now:%H:%M:%S}  " + (f"stand {stand}: " if stand else "") + text, flush=True)

    def write_runs(self):
        with open(self.folder / "runs.csv", "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(RUNS_HEADER)
            writer.writerows(run.row() for run in self.runs)

    def end_run(self, stand, t, why):
        run = stand.run
        run.stopped, _, _ = self.times()
        run.ended = why
        stand.run = None
        self.write_runs()
        self.event(stand.number, t, f"{run.name()} {why}, {run.log_bytes} bytes on the card")

    def print_line(self, text):
        print(f"{datetime.now():%H:%M:%S.%f}"[:-3] + "  " + text, flush=True)

    def show_packet(self, who, kind, text, rssi=None, snr=None):
        if self.showing:
            signal = f"   {rssi} dBm, SNR {snr} dB" if rssi is not None else ""
            self.print_line(f"{who:<12} {kind:<9}  {text}{signal}")

    def take(self, rssi, snr, raw, ms):
        """One packet heard, the 4 address bytes first. ms: the ground link's clock when it came."""
        with self.lock:
            if len(raw) < ADDRESS.size + TAG.size:
                self.second.other_network += 1
                self.show_packet("?", "", f"{len(raw)} bytes, too short to be this link's", rssi, snr)
                return
            to, sender, sequence, _ = ADDRESS.unpack_from(raw)
            data = raw[ADDRESS.size:]
            network, version, kind = TAG.unpack_from(data)
            who = f"{node(sender)} -> {node(to)}"
            if network != NETWORK_ID:
                self.second.other_network += 1
                self.show_packet("?", "", f"another network (0x{network:04X}), {len(raw)} bytes", rssi, snr)
                return
            if version != PROTOCOL_VERSION:
                self.second.other_version += 1
                self.show_packet(who, "", f"protocol {version}, this is {PROTOCOL_VERSION}", rssi, snr)
                self.wrong_version(sender, version)
                return

            if sender == NODE_BOX:
                self.second.box += 1
                self.ask(to, ms)
                self.show_packet(who, KINDS.get(kind, "?"), detail(kind, sequence, data), rssi, snr)
            if kind == TELEMETRY and 1 <= sender <= STAND_MAX and len(data) == TELEMETRY_PACKET.size:
                t = Telemetry(data)
                text = self.answer(sender, ms, rssi) + f", last command #{sequence}"
                text += f", logging {t.run()}" if t.logging else ""
                text += ", WENT SAFE BY ITSELF" if t.aborted else ""
                self.show_packet(who, "TELEMETRY", text, rssi, snr)
                self.take_telemetry(sender, t, rssi, snr)
            elif kind == STATE and sender == NODE_BOX and len(data) == STATE_PACKET.size:
                _, _, _, valves, flags = STATE_PACKET.unpack(data)
                self.take_state(valves, flags)
            elif kind == COMMAND and sender == NODE_BOX:
                # Not this ground link's: it doesn't hear itself.
                text = data[TAG.size:].decode("ascii", "replace")
                self.event(None, None, f"heard someone else send stand {to}: {text}")
            elif kind == SAFE and sender == NODE_BOX:
                self.event(None, None, f"heard SAFE sent to {'every stand' if to == 0xFF else f'stand {to}'}")
            elif sender != NODE_BOX:
                self.show_packet(who, KINDS.get(kind, "?"), "not from the launch box", rssi, snr)

    def take_sent(self, raw, ms):
        """One packet this ground link sent, the 4 address bytes first."""
        with self.lock:
            to, _, sequence, _ = ADDRESS.unpack_from(raw)
            data = raw[ADDRESS.size:]
            _, _, kind = TAG.unpack_from(data)
            self.second.laptop += 1
            self.ask(to, ms)
            self.show_packet(f"laptop -> {node(to)}", KINDS.get(kind, "?"), detail(kind, sequence, data))

    def ask(self, to, ms):
        """A packet from node 0. Whatever it was sent to answers it; any ask before it is over."""
        self.expire_asks(0)
        if to in STANDS:
            self.second.asked[to] += 1
            self.pending[to] = (ms, time.monotonic())

    def expire_asks(self, older_than_s):
        now = time.monotonic()
        for stand, (_, asked) in list(self.pending.items()):
            if now - asked >= older_than_s:
                del self.pending[stand]
                if self.showing:
                    self.print_line(f"{'':<12} {'':<9}  stand {stand} didn't answer")

    def answer(self, stand, ms, rssi):
        """Telemetry from a stand: the answer to the ask waiting for it, if one is."""
        self.second.dbm[stand].append(rssi)
        ask = self.pending.pop(stand, None)
        if ask is None:
            self.second.unasked += 1
            return "no ask heard before it"
        self.second.answered[stand] += 1
        self.second.answer_ms[stand].append(ms - ask[0])
        return f"answered in {ms - ask[0]:.0f} ms"

    def take_damaged(self, total):
        with self.lock:
            self.damaged_total = total

    def wrong_version(self, sender, version):
        now = time.monotonic()
        if now - self.version_reported.get(sender, -VERSION_REPORT_S) >= VERSION_REPORT_S:
            self.version_reported[sender] = now
            who = "the launch box" if sender == NODE_BOX else f"stand {sender}"
            self.event(None, None, f"{who} is on protocol {version}, this is {PROTOCOL_VERSION}: "
                                   f"its packets can't be read until both are the same")

    def take_telemetry(self, number, t, rssi, snr):
        pc_time, session_s, _ = self.times()
        stand = self.stands.get(number)
        if stand is None:
            stand = self.stands[number] = Stand(number, self.folder)
            self.event(number, t, f"heard at {rssi} dBm")
        last = stand.last

        if last and t.time_ms + RESTART_MS < last.time_ms:
            stand.boot += 1
            self.event(number, t, f"restarted: its clock went back from {last.time_ms / 1000:.1f} s")
            if stand.run:
                self.end_run(stand, t, "ended by the restart")
            last = None

        if stand.quiet:
            stand.quiet = False
            self.event(number, t, "heard again")

        # One name per packet, each channel in turn.
        if t.name_channel < len(CHANNELS):
            old = stand.names[t.name_channel]
            stand.names[t.name_channel] = t.name
            if stand.writer and old != t.name:
                self.event(number, t, f"{CHANNELS[t.name_channel]} is now named \"{t.name}\" "
                                      f"(the columns keep the first names)")
            if stand.writer is None and None not in stand.names:
                named = ", ".join(f"{c} = {n}" for c, n in zip(CHANNELS, stand.names) if n)
                self.event(number, t, f"names: {named or 'none set'}")
                stand.open()

        if t.logging:
            if stand.run and not stand.run.same(t):
                self.end_run(stand, t, "closed (the stand went on to a new file)")
            if stand.run is None:
                stand.run = Run(stand, t, pc_time)
                self.runs.append(stand.run)
                self.write_runs()
                self.event(number, t, f"started logging: {log_name(t.log_file)}")
            stand.run.last_s = t.time_ms / 1000
            stand.run.rows += 1
            stand.run.log_bytes = max(stand.run.log_bytes, t.log_bytes)
        elif stand.run:
            self.end_run(stand, t, "stopped")

        if t.aborted and not (last and last.aborted):
            self.event(number, t, "WENT SAFE BY ITSELF: it lost the launch box")
        elif last and last.aborted and not t.aborted:
            self.event(number, t, "following the launch box again")

        stand.last, stand.heard, stand.rssi = t, time.monotonic(), rssi
        stand.write(stand.row(t, rssi, snr, pc_time, session_s))

    def take_state(self, valves, flags):
        self.box_heard = time.monotonic()
        if not self.box_on_air:
            self.box_on_air = True
            self.event(None, None, "launch box on the air")
        if (valves, flags) == self.box_state:
            return
        old_flags = self.box_state[1] if self.box_state else 0
        self.box_state = (valves, flags)
        pc_time, session_s, _ = self.times()
        self.box.writerow([pc_time, f"{session_s:.3f}"]
                          + [int(bool(flags & bit)) for bit in (STATE_ARMED, STATE_ABORT, STATE_IGNITER_1,
                                                                STATE_IGNITER_2, STATE_FIRE_SEQUENCE)]
                          + [(valves >> v) & 1 for v in range(len(VALVES))])
        for bit, on, off in ((STATE_ARMED, "launch box armed", "launch box no longer armed"),
                             (STATE_ABORT, "launch box ABORT", "launch box abort cleared")):
            if (flags ^ old_flags) & bit:
                self.event(None, None, on if flags & bit else off)

    def tick(self):
        """Called a few times a second: notices what has gone quiet, and closes each second."""
        with self.lock:
            now = time.monotonic()
            self.expire_asks(ANSWER_WAIT_S)
            if now - self.second_start >= 1:
                self.close_second(now)
            if self.box_on_air and now - self.box_heard > BOX_QUIET_S:
                self.box_on_air = False
                self.event(None, None, "launch box off the air")
            # Without the launch box the stands only speak when polled, so quiet means nothing.
            for stand in self.stands.values():
                if self.box_on_air and not stand.quiet and now - stand.heard > STAND_QUIET_S:
                    stand.quiet = True
                    self.event(stand.number, None, f"not heard for {STAND_QUIET_S:.0f} s")

    def close_second(self, now):
        second, self.second = self.second, Second()
        self.second_start = now
        second.damaged = self.damaged_total - self.damaged_counted
        self.damaged_counted = self.damaged_total
        self.recent.append(second)
        pc_time, session_s, _ = self.times()
        row = [pc_time, f"{session_s:.3f}", second.box, second.laptop]
        for stand in STANDS:
            answer_ms, dbm = mean(second.answer_ms[stand]), mean(second.dbm[stand])
            row += [second.asked[stand], second.answered[stand], blank_or(answer_ms, 1), blank_or(dbm, 0)]
        self.link.writerow(row + [second.unasked, second.damaged, second.other_network, second.other_version])
        if self.rating:
            self.print_line(self.link_text([second]))

    def link_text(self, seconds):
        """Asks, answers and damaged packets over these seconds, as one line."""
        box, laptop = sum(s.box for s in seconds), sum(s.laptop for s in seconds)
        parts = [f"box sent {box}"] if box else []
        parts += [f"laptop sent {laptop}"] if laptop else []
        for stand in STANDS:
            asked, answered = sum(s.asked[stand] for s in seconds), sum(s.answered[stand] for s in seconds)
            answer_ms = mean([ms for s in seconds for ms in s.answer_ms[stand]])
            dbm = mean([d for s in seconds for d in s.dbm[stand]])
            if asked or dbm is not None:
                text = f"stand {stand} answered {answered} of {asked}"
                text += f" in {answer_ms:.0f} ms" if answer_ms is not None else ""
                text += f" at {dbm:.0f} dBm" if dbm is not None else ""
                parts.append(text)
        for count, what in ((sum(s.unasked for s in seconds), "telemetry with no ask heard"),
                            (sum(s.damaged for s in seconds), "damaged"),
                            (sum(s.other_network for s in seconds), "from another network"),
                            (sum(s.other_version for s in seconds), "on another protocol version")):
            if count:
                parts.append(f"{count} {what}")
        return " | ".join(parts) or "nothing heard"

    def show(self):
        with self.lock:
            if not self.stands:
                print("no stand heard yet")
            now = time.monotonic()
            for number, stand in sorted(self.stands.items()):
                t, label = stand.last, stand.label
                log = f"logging {t.run()}, {t.log_bytes} bytes on the card" if t.logging else "not logging"
                print(f"stand {number}: heard {now - stand.heard:.1f} s ago at {stand.rssi} dBm, "
                      f"battery {t.vbat_mv / 1000:.2f} V, {log}" + (", WENT SAFE BY ITSELF" if t.aborted else ""))
                print("  " + "   ".join(f"{label(f'pt{i + 1}')} {blank_or(t.pt(i), 1) or '-'} psi" for i in range(2)))
                print("  " + "   ".join(f"{label(f'tc{i + 1}')} {blank_or(t.tc(i), 1) or '-'} C" for i in range(4)))
                print("  " + "   ".join(f"{label(f'lc{i + 1}')} " + (f"{t.newtons(i):.1f} N" if t.newtons(i) is not None
                                                                      else f"{t.lc(i)} raw" if t.lc(i) is not None else "-")
                                        for i in range(2)))
                outputs = [f"{label(f'sol{i + 1}')} {'on' if (t.solenoids >> i) & 1 else 'off'}" for i in range(4)]
                outputs += [f"{label(f'srv{i + 1}')} {t.servo_us[i]} us" for i in range(4)]
                print("  " + "   ".join(outputs))
                mapped = [v for v in range(len(VALVES)) if t.valve(v) is not None]
                if mapped:
                    print("  valves: " + ", ".join(f"{VALVES[v]} {'open' if t.valve(v) else 'closed'}" for v in mapped))
            if self.recent:
                print(f"last {len(self.recent)} s: {self.link_text(self.recent)}")
            if self.box_state:
                valves, flags = self.box_state
                state = "ARMED" if flags & STATE_ARMED else "ABORT" if flags & STATE_ABORT else "key off"
                opened = [VALVES[v] for v in range(len(VALVES)) if (valves >> v) & 1]
                print(f"launch box: {'on the air' if self.box_on_air else 'off the air'}, {state}, "
                      f"asking for open: {', '.join(opened) or 'nothing'}")

    def close(self):
        with self.lock:
            for stand in self.stands.values():
                if stand.run:
                    stand.run.ended = "still recording when ground.py stopped"
                if stand.writer is None:
                    stand.open()
                stand.file.close()
            self.write_runs()
            self.events_file.close()
            self.box_file.close()
            self.link_file.close()
            for number, stand in sorted(self.stands.items()):
                print(f"stand {number}: {stand.rows} rows")
            print(f"{len(self.runs)} log runs, written to {self.folder}/")


class Link:
    """The ground link on its serial port. One request at a time."""

    def __init__(self, port, session):
        import serial
        self.serial = serial.Serial(port, BAUD, timeout=0.2)   # opening it restarts the Nano
        self.session = session
        self.results = queue.Queue()
        self.request_lock = threading.Lock()
        self.ready = threading.Event()
        self.stopping = threading.Event()
        self.lost = False
        self.old_sketch = False
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        buffer = b""
        while not self.stopping.is_set():
            try:
                buffer += self.serial.read(self.serial.in_waiting or 1)
            except Exception as error:   # unplugged
                self.lost = True
                self.session.event(None, None, f"lost the ground link ({error}). Type quit")
                return
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                self.take_line(raw.decode("ascii", "replace").strip())
            self.session.tick()

    def take_line(self, text):
        kind, _, rest = text.partition(" ")
        fields = rest.split(" ")
        try:
            if kind == "P" and len(fields) == 4:
                ms, rssi, snr, hex_bytes = fields
                self.session.take(int(rssi), int(snr), bytes.fromhex(hex_bytes), int(ms))
            elif kind == "P" and len(fields) == 3:
                # A ground link with the sketch from before packet times: the laptop's clock instead.
                if not self.old_sketch:
                    self.old_sketch = True
                    self.session.event(None, None, "the ground link has an older sketch: no packet times, "
                                                   "sent packets or damaged count. Upload tools/ground_link")
                rssi, snr, hex_bytes = fields
                self.session.take(int(rssi), int(snr), bytes.fromhex(hex_bytes), time.monotonic() * 1000)
            elif kind == "T" and len(fields) == 2:
                self.session.take_sent(bytes.fromhex(fields[1]), int(fields[0]))
            elif kind == "B":
                self.session.take_damaged(int(rest))
        except (ValueError, struct.error):
            pass   # a line garbled as the port opened
        if kind == "C":
            self.results.put(rest.split(" ")[-1])
        elif kind == "I":
            if rest.startswith("ground_link"):
                self.ready.set()
            self.session.event(None, None, f"ground link: {rest}")

    def request(self, line):
        """Sends one line and returns the ground link's result for it."""
        with self.request_lock:
            while not self.results.empty():
                self.results.get_nowait()
            self.serial.write((line + "\n").encode())
            try:
                return self.results.get(timeout=REQUEST_WAIT_S)
            except queue.Empty:
                return "noanswer"

    def close(self):
        self.stopping.set()
        self.reader.join()
        self.serial.close()


def find_link_port():
    from serial.tools import list_ports
    # CH340 (the Nano clones), FTDI, Arduino.
    ports = [port.device for port in list_ports.comports() if port.vid in (0x1A86, 0x0403, 0x2341, 0x2A03)]
    # macOS lists both /dev/cu.* and /dev/tty.*; cu.* is the one for this.
    ports = sorted(ports, key=lambda device: "/tty." in device)
    if not ports:
        sys.exit("No ground link found. Is the Nano plugged in? Pass --port to choose one.")
    return ports[0]


def send(session, link, stand, text):
    result = link.request(f"cmd {stand} {text}")
    session.event(stand, None, f"\"{text}\": {RESULTS.get(result, result)}")


def poll(session, link, stand):
    result = link.request(f"poll {stand}")
    messages = dict(RESULTS, ok="answered", failed="no answer")
    session.event(stand, None, f"poll: {messages.get(result, result)}")


def keep_polling(link, stands, stopping):
    """--poll: on the bench, with no launch box asking, poll the stands in turn."""
    paused = False
    while True:
        for stand in stands:
            if stopping.wait(POLL_PERIOD_S):
                return
            result = link.request(f"poll {stand}")
            if result == "busy" and not paused:
                print("launch box on the air: --poll waits until it's off", flush=True)
            paused = result == "busy"


HELP = ("type: 1 <console line>, 2 <console line>, poll <stand>, stop, show (every packet), "
        "rate (a line a second), Enter for the latest readings, quit")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="the ground link's serial port (default: the first Arduino-like one)")
    parser.add_argument("--out", type=Path, default=Path("."), help="where to make the session folder")
    parser.add_argument("--power", type=int, help="the ground link's transmit power, 2 to 20 dBm")
    parser.add_argument("--poll", type=int, nargs="*", metavar="STAND",
                        help="poll these stands (default 1) when the launch box is off the air")
    args = parser.parse_args()

    port = args.port or find_link_port()
    folder = args.out / f"ground-{datetime.now():%Y-%m-%d_%H%M%S}"
    folder.mkdir(parents=True)
    session = Session(folder)
    session.event(None, None, f"recording to {folder}/ from {port}")
    link = Link(port, session)
    if not link.ready.wait(READY_WAIT_S):
        print("nothing from the ground link yet: is ground_link.ino on it?", flush=True)
    if args.power is not None:
        result = link.request(f"power {args.power}")
        if result != "ok":
            session.event(None, None, f"power {args.power}: {RESULTS.get(result, result)}")

    poller = None
    stop_polling = threading.Event()
    if args.poll is not None:
        stands = args.poll or [1]
        poller = threading.Thread(target=keep_polling, args=(link, stands, stop_polling), daemon=True)
        poller.start()

    print(HELP, flush=True)
    try:
        while not link.lost:
            text = input().strip()
            words = text.split(" ", 1)
            if not text:
                session.show()
            elif text in ("quit", "exit"):
                break
            elif text == "show":
                session.showing = not session.showing
                print(f"show: {'on' if session.showing else 'off'}", flush=True)
            elif text == "rate":
                session.rating = not session.rating
                print(f"rate: {'on' if session.rating else 'off'}", flush=True)
            elif text == "stop":
                for stand in sorted(session.stands) or range(1, STAND_MAX + 1):
                    send(session, link, stand, "log stop")
            elif words[0] == "poll" and len(words) == 2 and words[1].isdigit():
                poll(session, link, int(words[1]))
            elif len(words) == 2 and words[0].isdigit() and 1 <= int(words[0]) <= STAND_MAX:
                send(session, link, int(words[0]), words[1].strip())
            else:
                print(HELP, flush=True)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        # The poller first: it may be waiting for the ground link's answer.
        stop_polling.set()
        if poller:
            poller.join()
        link.close()
        session.close()


if __name__ == "__main__":
    main()
