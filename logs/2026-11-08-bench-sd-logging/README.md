# 2026-11-08: SD card logging bench test

**What it is:** the first real `FLIGHT.LOG` from the SD card logger, decoded to
[`flight.csv`](flight.csv) with `tools/sd_log_decode.py`. It was recorded on the
bench (NUCLEO-H753ZI, microSD breakout on CN8, SD bus at 25 MHz), not in flight.
No sensors were connected: the data comes from a temporary test task that
logged two made-up channels so the logger could be checked.

| Channel | Type | Rate | Content |
|---|---|---|---|
| `test_count` | int32 | 1000 Hz | Counter, +1 every ms. Any jump means values were lost. |
| `test_wave` | float | 100 Hz | Triangle wave from 0 to 1 and back, 2 s period |

Logger code: commit `6d25d5d` (`src/tasks/sd_card_task.c`). The test task was
never committed.

## What it shows

The file has 3 sessions (one per power-up):

- **Session 1** (0 – 267 s): the logging test. 234,000 values logged,
  60,097 dropped. Logged + dropped matches what the test task produced.
- **Sessions 2 and 3:** later boots of the final firmware, which registers no
  channels yet, so they hold no data.

Every gap in `test_count` lines up with something done on purpose:

| Time | Values missing | Cause |
|---|---|---|
| 2.8 – 3.2 s | ~230, in small bursts | First boot with a new card: `FLIGHT.LOG` was being created while logging had already started |
| 167.7 – 175.8 s | 8,117 | A data wire pulled and put back within ~5 s. Logging resumed by itself. |
| 191.2 – 237.5 s | 46,288 | A data wire pulled and left out. Logging resumed once it was reconnected. |

Outside those gaps the counter is continuous: everything logged while the card
was connected reached the file. The logger also kept the rest of the firmware
running throughout, including with the card disconnected.

A dropout lasts a few seconds longer than the wire is out: after an error the
logger waits 1 s between retries, and the card has to be re-initialised.

## Columns

`session, t_ms, channel, value`. `t_ms` is milliseconds since that session's
boot. There is one row per logged value.
