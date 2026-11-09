# cuis-fc — STM32H753ZI + FreeRTOS

Firmware scaffold for the CU InSpace flight computer, targeting an
**STM32H753ZIT6** on a **NUCLEO-H753ZI**. It builds, flashes, and runs the
FreeRTOS scheduler at 480 MHz. A **heartbeat task** blinks LD1 (green) at 1 Hz:
a steady blink means the scheduler is alive, a frozen LED means a hang or fault.
The Sensor HAT and power-board firmware is being added in stages on top.

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
- Debian/Ubuntu ship **`gdb-multiarch`**, not `arm-none-eabi-gdb`. The
  workspace settings already point VS Code's debugger at it on Linux.
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

Install each tool with `winget` from PowerShell:

```powershell
winget install Kitware.CMake                  # CMake: generates the build
winget install Ninja-build.Ninja              # Ninja: runs the build
winget install Arm.GnuArmEmbeddedToolchain    # arm-none-eabi-gcc + gdb
winget install xpack-dev-tools.openocd-xpack  # OpenOCD: flashing and debugging
```

CMake installs for all users, so Windows asks for administrator approval, and it
adds itself to `PATH`. Prefer a manual install? Download the Windows x64
installer (`.msi`) from [cmake.org/download](https://cmake.org/download/) and
pick **"Add CMake to the system PATH"** during setup. Any CMake 3.22 or newer
works.

Windows-specific notes:
- **No USB driver needed** on Windows 10/11. The NUCLEO-H753ZI's STLINK-V3
  carries its own WinUSB descriptors, so Windows sets it up on first plug-in.
  (ST's STSW-LINK009 driver is only required for older ST-LINK/V2 probes or
  Windows 7/8.)
- Open a **new** terminal after installing, and fully quit and reopen VS Code:
  `PATH` changes only reach programs started afterwards. Then check that all four
  are found:
  ```powershell
  cmake --version
  ninja --version
  arm-none-eabi-gcc --version
  openocd --version
  ```
  If one isn't found, add its `bin` folder to `PATH` by hand. The usual culprit is
  the Arm toolchain: its installer has an "Add path to environment variable"
  checkbox that is **off** by default.
</details>

VS Code extensions are recommended automatically from `.vscode/extensions.json`:
`cmake-tools`, `clangd`, `cortex-debug`, and the `mcu-debug` views. If you also
have ST's STM32 extensions installed, the workspace settings switch off their
duplicate clangd and automatic IntelliSense setup for this project.

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
| `cmake --preset test && cmake --build --preset test && ctest --preset test` | Host unit tests for `src/app` (needs a normal C compiler) |
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

Or press <kbd>F5</kbd> in VS Code — that builds, flashes and breaks at `main`.
While debugging:

- **XPERIPHERALS** shows every peripheral register by name (`ADC1 → CR → ADEN`…),
  from [`debug/STM32H753.svd`](debug/README.md). The quickest way to check what a
  HAL call actually did to the hardware.
- **RTOS** lists every FreeRTOS task with its state, priority, and stack size and
  peak use.
- **LIVE WATCH** keeps updating expressions while the chip runs (right-click a
  variable → *Add to Live Watch*), so you can watch a sensor reading without
  halting.

The other everyday actions are VS Code tasks (*Terminal → Run Task*): **Flash
(debug)**, **Flash (release)**, **Reset board** and **Clean**. Snippets for the
repeated patterns — `halok`, `rtostask`, `xtaskcreate`, `mspinit` — live in
`.vscode/stm32.code-snippets`.

All probe settings live in [`openocd/nucleo-h753zi.cfg`](openocd/nucleo-h753zi.cfg)
so the command above is the same on every platform.

---

## Layout

```
CMakeLists.txt                  build definition
CMakePresets.json               debug / release / test presets
cmake/arm-none-eabi-gcc.cmake   toolchain file (CPU flags live here)
openocd/nucleo-h753zi.cfg       probe + target settings
debug/STM32H753.svd             register map for the debugger's peripheral view
include/FreeRTOSConfig.h        kernel configuration
include/stm32h7xx_hal_conf.h    HAL module enables, HSE_VALUE
include/ffconf.h                FatFs options
linker/STM32H753ZI_FLASH.ld     memory map
src/main.c                      startup ordering, heartbeat task, FreeRTOS hooks
src/tasks/                      one file per task (interlock, actuators, console, PT, TC, load cell, SD data log, debug log, flight stubs)
src/app/                        plain-C logic, unit-tested on the host (test/)
src/drivers/                    one peripheral or chip each (ADCs, solenoid PWM, servo PWM, stepper, SPI4, MAX31856, W25Q128, ADS1220, SD card)
src/board/                      HAT/power-board pin map
src/system_clock.c              SystemClock_Config + MPU_Config  (from CubeMX, + ADC clock)
src/stm32h7xx_hal_msp.c         peripheral MSP init              (from CubeMX)
src/gpio.c                      every port clock, plain outputs  (hand-written)
src/usart.c                     peripheral init                  (from CubeMX)
src/stm32h7xx_it.c              core fault handlers              (hand-written)
cubemx/fc.ioc                   CubeMX source of truth for the above
lib/                            pinned vendor submodules
tools/hat_log.py                decodes an SD-card log (LOGnnnn.BIN) into CSV files
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

**`arm-none-eabi-gcc was not found on PATH or in the usual install folders`** —
CMake can't see the compiler. Run `arm-none-eabi-gcc --version` in a new terminal:
if that fails too, the toolchain isn't on `PATH` (see your platform's install
notes). If it works there but not in VS Code, quit VS Code completely and reopen
it. Then delete `build/` and configure again. As a last resort, point CMake at the
compiler directly: `cmake --preset debug -DARM_GCC=<full path to arm-none-eabi-gcc>`.

**`The CMAKE_C_COMPILER: … is not a full path to an existing compiler tool`
(Windows)** — an older version of `cmake/arm-none-eabi-gcc.cmake` dropped the
`.exe` from the compiler's path. Pull the latest code, delete `build/`, and
configure again.

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

