# cuis-fc — STM32H753ZI + FreeRTOS

Firmware scaffold for the CU InSpace flight computer, targeting an
**STM32H753ZIT6** on a **NUCLEO-H753ZI**. It builds, flashes, and runs the
FreeRTOS scheduler at 480 MHz with a single **heartbeat task** that blinks LD1
(green) at 1 Hz. A steady blink means the scheduler is alive; a frozen LED means
a hang or fault.

**New to this?** Read [docs/how-it-works.md](docs/how-it-works.md) first: the build
chain, flash chain, boot sequence and code layout, explained from fundamentals.

Free toolchain throughout: GNU Arm Embedded + CMake + Ninja + OpenOCD. No
STM32CubeIDE, no STM32CubeCLT, and no ST account needed to build or flash.
CubeMX is used only to regenerate clock and pin configuration.

---

## Prerequisites

**Hardware:** a NUCLEO-H753ZI board and a USB data cable (the onboard ST-LINK
handles both power and flashing — no external programmer needed).

**Software:** four things on `PATH`: `arm-none-eabi-gcc`, `cmake` (≥3.22),
`ninja`, and `openocd`. Plus `arm-none-eabi-gdb` if you want to debug.

<details open>
<summary><b>macOS</b></summary>

```bash
brew install --cask gcc-arm-embedded
brew install cmake ninja open-ocd
```
</details>

<details>
<summary><b>Linux (Debian / Ubuntu)</b></summary>

```bash
sudo apt update
sudo apt install cmake ninja-build openocd gdb-multiarch
```

Install the **toolchain from Arm directly** 
```bash
ARCH=$(uname -m)   # x86_64 on a PC, aarch64 on a Raspberry Pi / Jetson
curl -LO "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-$ARCH-arm-none-eabi.tar.xz"
sudo tar xJf arm-gnu-toolchain-14.2.rel1-$ARCH-arm-none-eabi.tar.xz -C /opt
echo "export PATH=\"/opt/arm-gnu-toolchain-14.2.rel1-$ARCH-arm-none-eabi/bin:\$PATH\"" >> ~/.bashrc
```

Linux-specific notes:
- Debian/Ubuntu ship **`gdb-multiarch`**, not `arm-none-eabi-gdb`. Point
  cortex-debug at it in your VS Code *User* settings:
  `"cortex-debug.gdbPath": "/usr/bin/gdb-multiarch"`
- The `openocd` package already installs a udev rule
  (`/lib/udev/rules.d/60-openocd.rules`) that gives the `plugdev` group access to
  the ST-LINK. Make sure you're in it, and in `dialout` for the ST-LINK's serial
  port (`/dev/ttyACM0`):
  ```bash
  sudo usermod -aG plugdev,dialout $USER
  ```
  Log out and back in, then unplug and replug the board. Without this OpenOCD
  reports `LIBUSB_ERROR_ACCESS`.
</details>

<details>
<summary><b>Windows</b></summary>

```powershell
winget install Kitware.CMake Ninja-build.Ninja xpack-dev-tools.openocd-xpack
winget install Arm.GnuArmEmbeddedToolchain
```

Windows-specific notes:
- **No USB driver needed** on Windows 10/11. The NUCLEO-H753ZI's STLINK-V3
  carries its own WinUSB descriptors, so Windows sets it up on first plug-in.
  (ST's STSW-LINK009 driver is only required for older ST-LINK/V2 probes or
  Windows 7/8.)
- Open a **new** terminal after installing, then check that `where
  arm-none-eabi-gcc` and `where openocd` both find something. The Arm installer
  has an "Add path to environment variable" checkbox that is **off** by default;
  if `where` comes up empty, add its `bin` folder to `PATH` by hand.
</details>

VS Code extensions are recommended automatically from `.vscode/extensions.json`:
`cmake-tools`, `clangd`, `cortex-debug`, and the `mcu-debug` views.

---

## Build

```bash
git clone --recurse-submodules git@github.com:CU-in-Space/flightComputerRTOS.git
cd flightComputerRTOS
cmake --preset debug
cmake --build --preset debug
```

Already cloned without submodules? `git submodule update --init --recursive`.

| Command | Result |
|---|---|
| `cmake --preset debug` | Configure, `-Og -g3` |
| `cmake --preset release` | Configure, `-Os -g` |
| `cmake --build --preset debug` | Build → `build/debug/cuis-fc.{elf,hex,bin}` |
| `cmake --build --preset release` | Build → `build/release/…` |
| `cmake -E rm -rf build` | Clean everything |

Identical on all three platforms — the presets pin Ninja and the toolchain file,
so there is no `make` vs `nmake` vs `msbuild` divergence. In VS Code, <kbd>⇧⌘B</kbd>
(<kbd>Ctrl+Shift+B</kbd>) runs the debug build.

Every build ends by printing how much flash and RAM it uses. Most of the RAM is
the FreeRTOS heap (`configTOTAL_HEAP_SIZE`).

---

## Flash and debug

```bash
cmake --build --preset debug --target flash
```

This runs OpenOCD, which erases, programs, verifies (`** Verified OK **`) and
resets the board. It is the same on every platform, and the same OpenOCD config
is used for debugging.

Another option needs no tools at all: the NUCLEO mounts as a USB drive
(`NOD_H753ZI`), and copying `build/debug/cuis-fc.bin` onto it flashes the board.
Handy for getting a build onto someone's board quickly; no verification, no debug.

Or press <kbd>F5</kbd> in VS Code — that builds, flashes, breaks at `main`, and
enables the FreeRTOS task view.

All probe settings live in [`openocd/nucleo-h753zi.cfg`](openocd/nucleo-h753zi.cfg)
so the command above is the same on every platform.

---

## Layout

```
CMakeLists.txt                  build definition
CMakePresets.json               debug / release presets
cmake/arm-none-eabi-gcc.cmake   toolchain file (CPU flags live here)
openocd/nucleo-h753zi.cfg       probe + target settings
include/FreeRTOSConfig.h        kernel configuration
include/stm32h7xx_hal_conf.h    HAL module enables, HSE_VALUE
linker/STM32H753ZI_FLASH.ld     memory map
src/main.c                      startup ordering, heartbeat task, FreeRTOS hooks
src/system_clock.c              SystemClock_Config + MPU_Config  (from CubeMX)
src/stm32h7xx_hal_msp.c         peripheral MSP init              (from CubeMX)
src/gpio.c, src/usart.c         peripheral init                  (from CubeMX)
src/stm32h7xx_it.c              core fault handlers              (hand-written)
cubemx/fc.ioc                   CubeMX source of truth for the above
lib/                            pinned vendor submodules
```

Every `.c` file under `src/`, including subfolders, is compiled automatically,
so new modules and anything copied out of CubeMX need no build-file edits.

| Submodule | Version |
|---|---|
| `FreeRTOS-Kernel` | V11.3.1 |
| `cmsis_core` | v5.9.0_20250520 |
| `cmsis_device_h7` | v1.10.7 |
| `stm32h7xx_hal_driver` | v1.11.6 |

---

## Troubleshooting

**`ST-LINK error (DEV_CONNECT_ERR)` / `USBDeviceOpen: unknown error (0xe00002d8)`**
— something else holds the probe, or the probe is wedged. In order:
1. Kill ST's shared-access daemon, which **CubeMX spawns and leaves running**:
   `pkill -f stlink-server`. It will grab the probe again next time you open the
   `.ioc`, so expect to repeat this after CubeMX sessions.
2. Close any VS Code debug session or stray `openocd` (`pkill openocd`).
3. Unplug the USB cable, wait 5 s, replug. A failed flash can leave the ST-LINK
   enumerated but non-functional -- the giveaway is that the `NOD_H753ZI` drive
   stops appearing under `/Volumes` (macOS) or in your file manager.

**`mode (transport) not supported by device`, `init mode failed (unable to connect
to the target)`, `LIBUSB_ERROR_OTHER`, or the `NOD_H753ZI` drive vanishing
mid-copy** — **try a different USB cable first.** A marginal cable produced every
one of these during bring-up, on both OpenOCD and STM32CubeProgrammer, and they
disappeared with a new cable. Other telltales: the probe shows up with a missing
or garbled serial number (`system_profiler SPUSBHostDataType` on macOS, `lsusb -v`
on Linux), or the `NOD_H753ZI` drive never mounts. Plug straight into the
computer, not through a hub or dongle.

**`jtag status contains invalid mode value - communication failure`** during
programming — the SWD link dropped mid-write. Check the cable first (above).
Otherwise firmware already on the chip may have changed the clocks or entered low
power while OpenOCD was attaching: hold the black **RESET** button while the flash
command starts, or drop `adapter speed` in `openocd/nucleo-h753zi.cfg` to `480`.

**`LIBUSB_ERROR_ACCESS` (Linux)** — you're not in the `plugdev` group, or haven't
logged out and back in since joining it; see the Linux section.

**OpenOCD sees no board (Windows)** — check that the probe shows up in Device
Manager with no warning icon. If not, try another cable (above). If a tool such as
Zadig has replaced the ST-LINK's driver, uninstall that device in Device Manager
(tick "delete the driver") and replug to get the built-in WinUSB driver back.

**Only 1 MB of flash visible** — you are using `target/stm32h7x.cfg` instead of
the dual-bank config. Use `openocd/nucleo-h753zi.cfg`.

**Duplicate symbol `SysTick_Handler`** — you pasted CubeMX's `stm32h7xx_it.c`
wholesale. Take only the peripheral IRQ handlers from it.

**`implicit declaration of HAL_...`** — the CubeMX firmware package is newer than
the pinned `stm32h7xx_hal_driver`. Bump the submodule tag.

---

