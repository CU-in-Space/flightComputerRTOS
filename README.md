# cuis-fc — STM32H753ZI + FreeRTOS skeleton

Build scaffold for the CU InSpace flight computer (STM32H753ZIT6 on a NUCLEO-H753ZI).
It compiles, links, and starts the FreeRTOS scheduler with one idle placeholder
task. **There is no application logic yet** — that is deliberate.

Fully free toolchain: GNU Arm Embedded + CMake + Ninja + OpenOCD. No
STM32CubeIDE, no STM32CubeCLT, no ST account required.

## Prerequisites (macOS)

```bash
brew install --cask gcc-arm-embedded && brew install cmake ninja open-ocd
```

Your machine already has a working set (`gcc-arm-none-eabi` 9.3.1, CMake 3.31,
Ninja 1.13, OpenOCD 0.12), which is what this was verified against. The
`gcc-arm-embedded` cask above is the newer 15.x toolchain — optional, but worth
moving to eventually since 9.3.1 is from 2020.

VS Code extensions (VS Code will prompt from `.vscode/extensions.json`):
`ms-vscode.cpptools`, `ms-vscode.cmake-tools`, `marus25.cortex-debug`.

## Build

```bash
git clone --recurse-submodules <repo-url> && cd stm32h7-freertos
cmake --preset debug && cmake --build --preset debug
```

If you already cloned without submodules: `git submodule update --init --recursive`.

In VS Code, ⇧⌘B runs the same thing. Presets are `debug` (`-Og -g3`) and
`release` (`-Os`). Artifacts land in `build/<preset>/cuis-fc.{elf,hex,bin}`.

Current footprint: ~9 KB flash, ~67 KB RAM (64 KB of that is the FreeRTOS heap).

## Flash and debug

```bash
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg -c 'program build/debug/cuis-fc.elf verify reset exit'
```

Or press F5 in VS Code — `launch.json` starts OpenOCD, flashes, breaks at
`main`, and has `"rtos": "FreeRTOS"` set so the debugger shows a task list
rather than one bare thread.

## Layout

```
CMakeLists.txt                 build definition
CMakePresets.json              debug / release presets
cmake/arm-none-eabi-gcc.cmake  toolchain file (CPU flags live here)
include/FreeRTOSConfig.h       kernel configuration
include/stm32h7xx_hal_conf.h   HAL module enables + oscillator values
linker/STM32H753ZI_FLASH.ld    memory map
src/main.c                     scheduler startup + FreeRTOS hooks
src/stm32h7xx_it.c             core fault handlers
lib/                           pinned vendor submodules
```

Dependencies are pinned submodules, not vendored copies:

| Submodule | Version |
|---|---|
| `FreeRTOS-Kernel` | V11.3.1 |
| `cmsis_core` | v5.9.0_20250520 |
| `cmsis_device_h7` | v1.10.7 |
| `stm32h7xx_hal_driver` | v1.11.6 |

## Decisions worth knowing

**RAM is AXI SRAM, not DTCM.** `.data`/`.bss`/heap/stack sit in `RAM_D1`
(0x24000000, 512 KB) rather than DTCMRAM as ST's stock linker script does. DTCM
is faster but **the DMA controllers cannot reach it** — the classic H7 trap
where DMA silently transfers nothing. Starting in AXI SRAM keeps every sensor
DMA buffer valid by default. Move hot CPU-only data to DTCM later if profiling
justifies it.

**FreeRTOS owns SysTick; HAL borrows it.** The kernel's `SVC`/`PendSV`/`SysTick`
handlers are aliased onto the CMSIS vector names at the bottom of
`FreeRTOSConfig.h`. Because FreeRTOS takes SysTick, `HAL_IncTick()` is driven
from `vApplicationTickHook()` in `main.c` — that is why `configTICK_RATE_HZ` is
1000 and must stay there. Do not define those three handlers in
`stm32h7xx_it.c`; it would be a duplicate-symbol error.

**Interrupt priorities.** 4 priority bits. Any ISR calling a `...FromISR()` API
must use a priority number ≥ 5 (`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`).
Lower numbers are more urgent and are invisible to the kernel — use them only
for ISRs that touch no FreeRTOS API. `configASSERT` will trap violations.

**Caches.** I-cache is on. D-cache is deliberately off until there is
discipline around `SCB_CleanDCache_by_Addr` / `SCB_InvalidateDCache_by_Addr`
for DMA buffers.

## Before this flies

- [ ] **Clock config.** The core currently runs from the 64 MHz HSI, not the
      480 MHz PLL. Generate `SystemClock_Config()` with STM32CubeMX and call it
      from `main()` where the TODO is.
- [ ] **`HSE_VALUE`** is set to 8000000 for the NUCLEO's ST-LINK MCO.
      Change it if you move to a board with its own crystal.
- [ ] Drop `STM32H753.svd` (from ST's website) into `debug/` and uncomment
      `svdFile` in `launch.json` for named peripheral registers while debugging.
- [ ] Revisit `configTOTAL_HEAP_SIZE` (currently 64 KB) once the task set is real.
