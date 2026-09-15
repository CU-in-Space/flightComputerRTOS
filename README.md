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

You need four things on `PATH`: `arm-none-eabi-gcc`, `cmake` (≥3.22), `ninja`,
and `openocd`. Plus `arm-none-eabi-gdb` if you wanst to debug.

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

Install the **toolchain from Arm directly** rather than `apt install
gcc-arm-none-eabi` — the distro package is usually years old and on some
releases ships without `libnosys`, which breaks the link step:

```bash
curl -LO "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz"
sudo tar xJf arm-gnu-toolchain-*.tar.xz -C /opt
echo 'export PATH="/opt/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin:$PATH"' >> ~/.bashrc
```

Two Linux-specific notes:
- Debian/Ubuntu ship **`gdb-multiarch`**, not `arm-none-eabi-gdb`. Point
  cortex-debug at it in your VS Code *User* settings:
  `"cortex-debug.gdbPath": "/usr/bin/gdb-multiarch"`
- Flashing as a non-root user needs a udev rule for the ST-LINK:
  ```bash
  sudo cp /usr/share/openocd/contrib/60-openocd.rules /etc/udev/rules.d/
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```
  Without it OpenOCD reports `LIBUSB_ERROR_ACCESS`. Unplug and replug the board.
</details>

<details>
<summary><b>Windows</b></summary>

```powershell
winget install Kitware.CMake Ninja-build.Ninja
winget install Arm.GnuArmEmbeddedToolchain
```

OpenOCD has no winget package; use [xPack OpenOCD](https://github.com/xpack-dev-tools/openocd-xpack/releases)
or `scoop install openocd`, then add its `bin` to `PATH`.

Windows-specific notes:
- Install the **ST-LINK USB driver** ([ST's STSW-LINK009](https://www.st.com/en/development-tools/stsw-link009.html)),
  or OpenOCD will not see the board at all.
- Verify with `where arm-none-eabi-gcc` before building. The Arm installer has a
  "Add path to environment variable" checkbox that is **off** by default.
- Use PowerShell rather than `cmd`. The commands below are identical either way,
  but the quoting in the flash command only works in PowerShell and POSIX shells.
</details>

VS Code extensions are recommended automatically from `.vscode/extensions.json`:
`cmake-tools`, `clangd`, `cortex-debug`, and the `mcu-debug` views.

---

## Build

```bash
git clone --recurse-submodules <repo-url>
cd stm32h7-freertos
cmake --preset debug
cmake --build --preset debug
```

Already cloned without submodules? `git submodule update --init --recursive`.

| Command | Result |
|---|---|
| `cmake --preset debug` | Configure, `-Og -g3` |
| `cmake --preset release` | Configure, `-Os` |
| `cmake --build --preset debug` | Build → `build/debug/cuis-fc.{elf,hex,bin}` |
| `cmake --build --preset release` | Build → `build/release/…` |
| `cmake -E rm -rf build` | Clean everything |

Identical on all three platforms — the presets pin Ninja and the toolchain file,
so there is no `make` vs `nmake` vs `msbuild` divergence. In VS Code, <kbd>⇧⌘B</kbd>
(<kbd>Ctrl+Shift+B</kbd>) runs the debug build.

Current footprint: **~21 KB flash**, ~67 KB RAM (64 KB of which is the FreeRTOS heap).

## Flash and debug

```bash
cmake --build --preset debug --target flash
```

This prefers **STM32CubeProgrammer** (found automatically if you have STM32CubeIDE
or STM32CubeCLT installed) and falls back to OpenOCD otherwise. Prefer it over
calling OpenOCD directly: OpenOCD 0.12.0 cannot reliably flash an STM32H7 through
an ST-LINK **V3** — see Troubleshooting. Debugging over OpenOCD is unaffected.

A third option needs no tools at all: the NUCLEO mounts as a USB drive
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

## Decisions worth knowing

**Clock: 480 MHz.** 8 MHz HSE (bypass, from the ST-LINK MCO — the NUCLEO has no
crystal at X3) → PLL1 `/4 ×480 /2` → 480 MHz SYSCLK, 240 MHz HCLK, 120 MHz on
APB1–4. This needs VOS0, which needs the LDO supply — fine on this board, but it
would be unreachable on an SMPS-supplied `-Q` variant. Regenerate from
`cubemx/fc.ioc`; don't hand-edit `src/system_clock.c`.

**RAM is AXI SRAM, not DTCM.** `.data`/`.bss`/heap/stack live in `RAM_D1`
(0x24000000, 512 KB) rather than DTCMRAM as ST's stock script does. **The DMA
controllers cannot reach DTCM** — the classic H7 trap where DMA silently
transfers nothing. Starting in AXI SRAM keeps every sensor buffer valid by
default.

**FreeRTOS owns SysTick; HAL borrows it.** The kernel's `SVC`/`PendSV`/`SysTick`
handlers are aliased onto the CMSIS vector names at the bottom of
`FreeRTOSConfig.h`, so `HAL_IncTick()` is driven from `vApplicationTickHook()`.
That is why `configTICK_RATE_HZ` is 1000 and must stay there. **Never define
those three handlers in `stm32h7xx_it.c`** — including by pasting CubeMX's
version of that file, which contains all three.

**Interrupt priorities.** 4 priority bits. Any ISR calling a `...FromISR()` API
must use a priority number **≥ 5** (`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`).
Lower numbers are more urgent and invisible to the kernel — use them only for
ISRs that touch no FreeRTOS API. `configASSERT` traps violations.

**FreeRTOS port is `GCC_ARM_CM4F`, not CM7.** The STM32H753 is a Cortex-M7
**r1p1**, and FreeRTOS's own `portable/GCC/ARM_CM7/ReadMe.txt` recommends the
CM4F port for every revision except r0p1.

**Caches.** I-cache on. D-cache deliberately off until there is discipline
around `SCB_CleanDCache_by_Addr` / `SCB_InvalidateDCache_by_Addr` for DMA buffers.

**Startup ordering in `main()` is load-bearing:** `MPU_Config()` →
`SCB_EnableICache()` → `HAL_Init()` → `SystemClock_Config()`. `HAL_Init()` calls
`HAL_MspInit()`, which enables the SYSCFG clock, which `SystemClock_Config()`
needs before it can set the overdrive bit for VOS0.

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

**`jtag status contains invalid mode value - communication failure`** during
programming — the SWD link dropped mid-write, usually because firmware already
on the chip changed the clocks or entered low power while OpenOCD was attaching.
`openocd/nucleo-h753zi.cfg` handles this with `connect_assert_srst` and a
1.8 MHz adapter speed. If it still fails, hold the black **RESET** button while
the flash command starts, or drop `adapter speed` to `480`.

**`LIBUSB_ERROR_ACCESS` (Linux)** — missing udev rule; see the Linux section.

**OpenOCD sees no board (Windows)** — missing ST-LINK driver (STSW-LINK009).

**Only 1 MB of flash visible** — you are using `target/stm32h7x.cfg` instead of
the dual-bank config. Use `openocd/nucleo-h753zi.cfg`.

**Duplicate symbol `SysTick_Handler`** — you pasted CubeMX's `stm32h7xx_it.c`
wholesale. Take only the peripheral IRQ handlers from it.

**`implicit declaration of HAL_...`** — the CubeMX firmware package is newer than
the pinned `stm32h7xx_hal_driver`. Bump the submodule tag.

---

## Before this flies

- [x] Silicon revision confirmed **rev V** on the club NUCLEO (2026-09-14), so 480 MHz
      is in spec. Check any new board: STM32CubeProgrammer prints `Revision ID` on
      connect. Rev Y caps at 400 MHz — set `PLLN` to 400 and voltage scale 1.
- [ ] Drop `STM32H753.svd` into `debug/` and uncomment `svdFile` in
      `.vscode/launch.json` for named peripheral registers while debugging.
- [ ] Revisit `configTOTAL_HEAP_SIZE` (currently 64 KB) once the task set is real.
- [ ] Retarget `_write()` to USART3 if you want `printf` over the ST-LINK VCP
      (115200 8N1 on PD8/PD9).
