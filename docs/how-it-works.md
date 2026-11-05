# How cuis-fc works

A from-the-ground-up tour of this project: how C source becomes bytes in the
chip's flash, how those bytes get onto the board, what happens between power-on
and the blinking LED, and where everything lives. Sizes and code addresses change
with every build, so they're left out: the build prints flash and RAM usage at the
end of every link, and `build/debug/cuis-fc.map` has the details.

1. [The build chain](#1-the-build-chain)
2. [The flash chain](#2-the-flash-chain)
3. [From power-on to a blinking LED](#3-from-power-on-to-a-blinking-led)
4. [Code layout](#4-code-layout)
5. [Where to learn more](#5-where-to-learn-more)
6. [Glossary](#glossary)

---

## 1. The build chain

```
  src/*.c        lib/**/*.c        startup_stm32h753xx.s
     │               │                     │
     └──────── ① compile (arm-none-eabi-gcc) ──────┐
                                                   ▼
                                     .obj files   machine code, addresses not yet known
                                                   │
                                  ② archive        HAL → libstm32cube.a, kernel → libfreertos_kernel.a
                                                   │
       linker/STM32H753ZI_FLASH.ld ───▶ ③ link ◀───┘
                                                   ▼
                                             cuis-fc.elf   code + addresses + debug info
                                                   │
                                  ④ objcopy        ▼
                                             cuis-fc.bin / .hex   just the bytes for flash
```

### Cross-compiling

Your computer (Mac, Linux or Windows) runs a full operating system on its own
CPU. The STM32H753 is an Arm **Cortex-M7**: a
different instruction set, and **no operating system at all**. So we need a
*cross-compiler* — one that runs on your computer but produces code for the chip:
`arm-none-eabi-gcc`.

| Part of the name | Meaning |
|---|---|
| `arm` | produces Arm machine code |
| `none` | no OS underneath — no files, no processes, nothing to ask for memory |
| `eabi` | the embedded calling convention: how functions pass arguments, how structs are laid out |

The toolchain file [`cmake/arm-none-eabi-gcc.cmake`](../cmake/arm-none-eabi-gcc.cmake)
adds four flags to every compile that describe the exact CPU:

| Flag | Tells the compiler |
|---|---|
| `-mcpu=cortex-m7` | which instructions exist |
| `-mthumb` | use the compact Thumb-2 encoding — the only one a Cortex-M can run |
| `-mfpu=fpv5-d16` | there is a double-precision floating-point unit (FPU) |
| `-mfloat-abi=hard` | pass floats in FPU registers. Faster, but the FPU must be switched on at boot ([§3](#reset_handler-making-c-possible)) |

### CMake and Ninja

Two tools with two jobs:

- **CMake** reads [`CMakeLists.txt`](../CMakeLists.txt), a *description* of the
  build (which files, which flags, what links to what), and writes out a plan.
- **Ninja** carries out the plan: runs the compiler on each file in parallel, and
  on later builds re-runs only what changed.

```bash
cmake --preset debug           # configure: CMake writes build/debug/build.ninja (rarely needed)
cmake --build --preset debug   # build: Ninja runs the plan (every time)
```

[`CMakePresets.json`](../CMakePresets.json) pins the choices — Ninja, the
toolchain file, debug flags — so every machine builds the same way.

### ① Compile: one `.c` file becomes one `.obj`

Each source file is compiled **on its own, knowing nothing about the others**.
`main.c` calls `HAL_GPIO_TogglePin()`, but the compiler has only seen its
*declaration* in a header, so it leaves a note: "put that function's address
here later".

Two `-D` defines steer ST's headers: `STM32H753xx` selects this exact chip's
register map, and `USE_HAL_DRIVER` enables the HAL.

Every file becomes one object: ST's HAL and CMSIS, the FreeRTOS kernel, and ours
(every `.c` file under `src/` plus the startup assembly).

`-ffunction-sections -fdata-sections` puts every function and variable in its own
named section. That pays off in step ③.

### ② Archive: bundle objects into libraries

HAL objects are bundled into `libstm32cube.a`, and kernel objects into
`libfreertos_kernel.a`. An archive is basically a zip of `.obj` files, which lets
the linker pull out only the pieces something actually uses.

### ③ Link: stitch it together, and decide where everything goes

The linker has two jobs.

**Resolve names.** Every "address goes here later" note from step ① gets filled
in. A name that's never defined anywhere gives `undefined reference`; one defined
twice gives `multiple definition`.

**Place everything in memory.** A desktop program gets loaded wherever the OS
chooses. A microcontroller has fixed physical memory, so *you* choose, in
[`linker/STM32H753ZI_FLASH.ld`](../linker/STM32H753ZI_FLASH.ld):

```
0x0800_0000   FLASH    2 MB     keeps its contents without power — code lives here
0x2400_0000   RAM_D1   512 KB   AXI SRAM, wiped at power-off — variables live here
```

The chip has other, smaller RAM regions we don't use yet. The comment at the top
of the linker script explains why not DTCM: the DMA controllers can't reach it.

The compiler sorts everything into **sections**. The linker script says which
memory each section goes into:

| Section | Contains | Goes to |
|---|---|---|
| `.isr_vector` | the vector table ([§3](#reset-the-cpu-reads-two-numbers)) | FLASH, **first** |
| `.text` | machine code | FLASH |
| `.rodata` | constants, string literals | FLASH |
| `.data` | variables with a starting value | RAM — **but the starting values are stored in FLASH** |
| `.bss` | variables that start at zero | RAM |

`.data` is the subtle one. Take `uint32_t SystemCoreClock = 64000000;`. RAM is
blank at power-on, so that `64000000` has to be stored in flash, and something has
to copy it into RAM before `main()` runs. That something is the startup code in
[§3](#reset_handler-making-c-possible).

`.bss` is mostly `ucHeap`, the pool FreeRTOS hands memory out from. Its size is
set by `configTOTAL_HEAP_SIZE`.

**Garbage collection.** `-Wl,--gc-sections` discards every section nothing
references. That's why compiling all of ST's HAL costs nothing: only the HAL
functions we actually call survive into the final program.

### ④ The output files

| File | What it is | Used by |
|---|---|---|
| `cuis-fc.elf` | code + addresses + **debug info** (names, line numbers, types) | the flasher and the debugger |
| `cuis-fc.bin` | exactly the bytes that go into flash, nothing else | drag-and-drop onto the `NOD_H753ZI` drive |
| `cuis-fc.hex` | the same bytes written as text, with addresses | some programming tools |
| `cuis-fc.map` | the linker's report of where everything landed | you, when asking "why is my binary so big?" |

The `.elf` is many times larger than the `.bin`. Almost all of that is debug info,
and none of it reaches the chip.

---

## 2. The flash chain

```
 computer ──USB──▶ ST-LINK V3 ──SWD, 2 wires──▶ STM32H753 debug port ──▶ flash controller ──▶ FLASH @ 0x0800_0000
                   a second MCU
                   on the NUCLEO
```

**The ST-LINK** is a *second* microcontroller on the NUCLEO board. Its only job is
translating USB commands from your computer into SWD.

**SWD** (Serial Wire Debug) is a two-wire interface, `SWDIO` and `SWCLK`, into a
debug port built into every Cortex-M. Through it a debugger can halt the CPU, step
it, and **read or write any address**. That's how we checked the firmware without
adding any code: reading `SystemCoreClock` from RAM returned `480000000`, and
reading the GPIOB output register at `0x58020414` showed the LED pin toggling.

**Flash is not RAM.** You can't simply write bytes to `0x08000000`. The chip's
flash controller has to be unlocked, the sectors erased (every bit becomes 1),
then programmed (bits set to 0), then relocked. Programming tools know this
sequence for each chip.

What `cmake --build --preset debug --target flash` does, using OpenOCD:

1. **Reset and halt**: pulse the H753's reset line and stop the CPU before it runs
   anything, so whatever firmware is already on the chip can't interfere
2. **Erase** the flash sectors the new image needs
3. **Program** the parts of the `.elf` that belong in flash
4. **Verify**: read everything back and compare
5. **Reset**: release the chip, which boots the new firmware

**Debugging uses the same two wires.** Pressing F5 in VS Code starts OpenOCD,
which talks SWD through the ST-LINK and runs a GDB server on port 3333. VS Code's
cortex-debug extension talks to that server. The `.elf`'s debug info is what lets
it show you `vHeartbeatTask` instead of a bare hex address.

Every link in this chain failed at least once during bring-up: a USB cable that
couldn't sustain long transfers, ST-LINK firmware stuck in its bootloader, and a
leftover `stlink-server` process holding the probe. The bad cable was the most
misleading: its errors looked exactly like an OpenOCD bug, and ST's own tool
failed the same way. If flashing fails, swap the cable first. See README →
Troubleshooting.

---

## 3. From power-on to a blinking LED

### Reset: the CPU reads two numbers

A Cortex-M doesn't "start at `main`". On reset, the CPU reads the first two 32-bit
values in flash. These are the start of the **vector table**, which is why
`.isr_vector` must be placed first:

| Address | Value | Meaning |
|---|---|---|
| `0x0800_0000` | `0x2408_0000` | initial stack pointer: the top of AXI SRAM (`_estack` in the linker script) |
| `0x0800_0004` | `Reset_Handler` + 1 | wherever `Reset_Handler` landed in flash. The low bit is set to mean "Thumb code" |

The rest of the table holds the address of every interrupt handler — SysTick,
USART3, DMA, and so on.

### `Reset_Handler`: making C possible

At this point RAM holds garbage, the FPU is off, and C can't run yet. So
`Reset_Handler` is written in assembly, in
`lib/cmsis_device_h7/Source/Templates/gcc/startup_stm32h753xx.s`:

1. **Set the stack pointer** to `_estack`
2. **`ExitRun0Mode()`** (H7-specific): select the LDO power supply and wait for the
   core voltage to settle
3. **`SystemInit()`**: **turn the FPU on** — without this, the first
   floating-point instruction crashes, because we compile with hard float — reset
   clocks to the internal 64 MHz oscillator, and point the CPU at the vector table
4. **Copy `.data`** from flash to RAM. This is where `SystemCoreClock` gets its
   `64000000`
5. **Zero `.bss`**. C guarantees uninitialised globals start at 0, and this is what
   keeps that promise
6. **`__libc_init_array()`**: C library setup
7. **`main()`**

### `main()`: bring up the hardware — order matters

In [`src/main.c`](../src/main.c):

| Call | What it does | Why it's here |
|---|---|---|
| `MPU_Config()` | marks unmapped address ranges as no-access | the M7 reads ahead speculatively and would fault on them |
| `SCB_EnableICache()` | turns on the instruction cache | free speed |
| `HAL_Init()` | HAL setup, then calls `HAL_MspInit()`, which clocks the SYSCFG peripheral | `SystemClock_Config()` needs SYSCFG |
| `MX_GPIO_Init()` | clocks every GPIO port; drives LD1, the HAT LED and every power-board actuator line low | before anything else can go wrong |
| `crash_init()` | unlocks the RTC backup registers that hold the crash record | |
| `SystemClock_Config()` | raises the core voltage, starts the PLL, switches to **480 MHz**; picks the ADCs' clock | overwrites `SystemCoreClock` with `480000000` |
| `MX_USART3_UART_Init()` | sets up the serial port to the ST-LINK's virtual COM port | |
| `vbat_adc_init()` | sets up and calibrates ADC3 for the battery voltage | all hardware setup happens here, before any task runs |
| `solenoid_pwm_init()` | starts TIM1's 20 kHz PWM at 0 %, then hands SOL1–4 to it | |
| `libc_locks_init()` | creates the mutexes that make the C library safe to share between tasks | must exist before any task runs |
| `log_init()`, `printf(…)`, `crash_report()` | creates the debug-log queue; prints a boot line, the reset reason and any saved crash | from here on any task can `printf` |
| `actuator_init()`, `console_init()` | create the actuator command queue; turn on the serial port's receive interrupt | |
| `xTaskCreate(vInterlockTask, …)` | checks the battery voltage every 10 ms: the cable interlock | highest application priority |
| `xTaskCreate(vActuatorTask, …)` | owns the solenoids; applies commands while the interlock is ENABLED | |
| `xTaskCreate(vConsoleTask, …)` | reads typed commands from the serial port | |
| `xTaskCreate(vLogTask, …)` | the only task that writes to USART3 | |
| `xTaskCreate(vHeartbeatTask, …)` | registers the heartbeat task (it doesn't run yet) | |
| `xTaskCreate(vStateEstimationTask, …)`, `xTaskCreate(vAirbrakeTask, …)` | registers the two flight tasks in `src/tasks/`. For now they're stubs that just sleep | |
| `xTaskCreate(vPtTask, …)` | samples the PT at 10 Hz and streams CSV through the log | |
| `vTaskStartScheduler()` | **hands the CPU to FreeRTOS. Never returns.** | |

### The scheduler: how the LED actually blinks

FreeRTOS takes over the Cortex-M **SysTick** timer and has it interrupt **1,000
times per second**. On every tick:

- the kernel checks whether any waiting task is due, and switches to it

HAL keeps its own millisecond clock (`HAL_GetTick()`) on a separate timer, TIM6,
at an interrupt priority FreeRTOS never masks, so it runs even before the scheduler
starts.

The heartbeat task is a loop:

```c
for (;;) {
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);   // flip PB0
    vTaskDelay(pdMS_TO_TICKS(500));                 // sleep for 500 ticks
}
```

`vTaskDelay` is the core idea of an RTOS: **it doesn't spin in a loop**. It tells
the kernel "wake me in 500 ticks" and gives up the CPU. The two stub tasks do the
same, so most of the time every task is asleep and the kernel runs its built-in
idle task. After 500 ticks the heartbeat is ready
again, flips the pin, and sleeps again.

Once the state estimation task does real work, *it* gets the CPU during those
500 ms instead of the idle task. That's how multiple jobs share one CPU.

---

## 4. Code layout

### The layers

Here's what the one line `HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin)` looks like
at each level:

```
 your code      HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin)       src/main.c
                          │  LED1_* are CubeMX's names for GPIOB pin 0 (include/main.h)
 HAL            odr = GPIOB->ODR;  GPIOB->BSRR = …                  lib/stm32h7xx_hal_driver
                          │  GPIOB is a struct laid over the hardware's registers
 CMSIS device   GPIOB = 0x4000_0000 + 0x1802_0000 + 0x400           lib/cmsis_device_h7
                          │  ODR is 0x14 past that
 silicon        output register at 0x5802_0414                      the address we read over SWD
```

Alongside the HAL sits **FreeRTOS** (`lib/FreeRTOS-Kernel`). Beneath both sits
**CMSIS core** (`lib/cmsis_core`): Arm's definitions for the parts that are
identical on every Cortex-M, such as SysTick, the interrupt controller (NVIC), the
caches, and the MPU.

### Who owns each file

The most useful way to read the tree is by **who's allowed to change each file**.

**You write these**
```
src/main.c                    startup order, tasks, FreeRTOS hooks, Error_Handler
src/stm32h7xx_it.c            fault handlers; peripheral interrupt handlers live with the code that owns the peripheral
src/libc/locks.c              makes the C library thread-safe: newlib's locks on FreeRTOS mutexes
src/libc/syscalls.c           low-level functions the C library needs, e.g. growing malloc's heap
src/tasks/                    one file per task: interlock, actuators, console, PT, log; state estimation and airbrake are stubs
src/app/                      plain-C logic with no hardware, tested on your laptop (test/)
src/drivers/                  one peripheral each: ADCs, solenoid PWM
src/board/                    the pin map
src/gpio.c                    MX_GPIO_Init: every port clock, every plain output, all low at boot
src/system_clock.c            SystemClock_Config (480 MHz, ADC clock) + MPU_Config
include/FreeRTOSConfig.h      kernel settings: tick rate, heap size, interrupt priorities
```

**CubeMX generates these; you copy them in. Don't hand-edit.**
```
cubemx/fc.ioc                 ← the source of truth. Change pins and clocks here,
                                regenerate, and copy the files below back in.
src/stm32h7xx_hal_msp.c       each peripheral's clock and pin setup ("MCU Support Package")
src/usart.c                   MX_USART3_UART_Init
include/main.h, gpio.h, usart.h
```

**Build and flash configuration**
```
CMakeLists.txt                what to compile, how to link, the `flash` target
CMakePresets.json             debug, release and test presets
cmake/arm-none-eabi-gcc.cmake the cross-compiler and CPU flags
linker/STM32H753ZI_FLASH.ld   the memory map
openocd/nucleo-h753zi.cfg     debugger settings
include/stm32h7xx_hal_conf.h  which HAL modules are compiled, and the 8 MHz HSE value
.vscode/                      build, flash and debug buttons for VS Code
```

**Vendor code — pinned git submodules. Don't edit.**
```
lib/FreeRTOS-Kernel           the RTOS
lib/stm32h7xx_hal_driver      ST's HAL
lib/cmsis_device_h7           the H7 register map, startup assembly, SystemInit
lib/cmsis_core                Cortex-M core definitions
```

**Generated output — never commit**
```
build/                        everything from §1
cubemx/ (except fc.ioc)       CubeMX's scratch project
```

### Where new things go

| You want to… | Do this |
|---|---|
| add a task | write it in `src/` (every `.c` file under `src/`, subfolders included, is compiled automatically) and `xTaskCreate` it in `main()` |
| use a new peripheral | write a driver in `src/drivers/`: it sets up the peripheral and its pins, and `main()` calls its init before the scheduler starts |
| handle an interrupt | put its handler in the file that owns the peripheral (e.g. `USART3_IRQHandler` in `src/tasks/console_task.c`). **Never** add `SysTick_Handler`, `PendSV_Handler` or `SVC_Handler` — FreeRTOS owns those |
| change heap size or tick rate | edit `include/FreeRTOSConfig.h` |

### How to lay out code as it grows

Today there are tasks, drivers, `app/` logic and the board pin map. As sensors, logging
and airbrake control arrive, a common, proven shape is three kinds of module, each
in its own folder under `src/`. Subfolders compile automatically, and `#include "drivers/imu.h"` works.

```
src/
  main.c                     creates the tasks, nothing else
  libc/                      glue that makes the C library work under FreeRTOS (already exists)
  drivers/  imu.c  baro.c    one chip each, spoken to over SPI/I²C via the HAL. No FreeRTOS.
  tasks/    state_estimation_task.c  airbrake_task.c  log_task.c
                             one job each: wait on a queue, do the work, pass results on
                             (the first two exist as stubs)
  app/      flight_state.c  airbrake_control.c
                             pure logic: no HAL, no FreeRTOS
```

Data then moves between tasks like this:

```
 IMU interrupt ─notify─▶ state_estimation_task ──queue──▶ airbrake_task ─▶ PWM to servo
                                       └────────queue──▶ log_task ─▶ storage
```

Three rules make it work:

1. **Dependencies point one way: down.** Tasks use drivers and app logic; drivers
   use the HAL. A driver never calls a task, and app logic never touches the HAL.
2. **Tasks share data through queues, not global variables.** One task writing a
   global while another reads it is the classic RTOS bug. It passes on the bench and
   fails at random. The one exception is a single word that one task writes and
   others only read, like the interlock state: a `static volatile` variable behind
   a getter.
3. **Keep `app/` free of HAL and FreeRTOS.** Then the flight logic — the code that
   matters most — also compiles on your computer, where you can test it against
   recorded flight data without a board. Small logic that belongs to one task
   and its hardware, like the interlock or peak-and-hold, stays in that task's
   file, where it's easier to follow than split across two.

Treat this as a starting point, not a law. *Making Embedded Systems* and Miro
Samek's course (below) explain the reasoning and the alternatives.

---

## 5. Where to learn more

**Start here, in this order**

1. DigiKey's *Getting Started with STM32 and Nucleo*, parts 1–2: the HAL on a real board
2. DigiKey's *Introduction to RTOS*: FreeRTOS ideas, one short video at a time
3. *From Zero to main()*: the startup and linker details from §1–3, in depth

**STM32 and the HAL**

- [Getting Started with STM32 and Nucleo](https://www.digikey.com/en/maker/videos/shawn-hymel/getting-started-with-stm32-and-nucleo-part-1-introduction-to-stm32cubeide-and-blinky)
  (DigiKey, Shawn Hymel): free videos on blinky, I²C, SPI, ADC + DMA and timers.
  Uses CubeIDE and a different Nucleo, but the HAL calls are the same. Skip part 3:
  it uses the CMSIS-RTOS wrapper, not the native FreeRTOS API this project uses.
- [STM32CubeH7 examples for NUCLEO-H743ZI](https://github.com/STMicroelectronics/STM32CubeH7/tree/master/Projects/NUCLEO-H743ZI/Examples):
  ST's working projects for every peripheral, on the same board family. When you add
  a peripheral, find its example and copy the setup pattern.
- [UM2217](https://www.st.com/resource/en/user_manual/um2217-description-of-stm32h7-hal-and-lowlayer-drivers-stmicroelectronics.pdf):
  the reference for every HAL function.
- [RM0433](https://www.st.com/resource/en/reference_manual/rm0433-stm32h742-stm32h743753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf):
  the chip's register-level manual. Look things up; don't read it cover to cover.

**FreeRTOS**

- [Introduction to RTOS](https://www.youtube.com/playlist?list=PLEBQazB0HUyQ4hAPU1cJED6t3DU0h34bz)
  (DigiKey, Shawn Hymel): 12 short videos on tasks, scheduling, queues, mutexes and
  interrupts, with [example code](https://github.com/ShawnHymel/introduction-to-rtos).
  It runs on an ESP32, but the API is nearly identical — just ignore the
  ESP32-only `…PinnedToCore` calls.
- [Mastering the FreeRTOS Real Time Kernel](https://www.freertos.org/Documentation/02-Kernel/07-Books-and-manual/01-RTOS_book):
  the official free book. Chapters 3, 4 and 7 cover tasks, queues and interrupts.

**Structuring firmware**

- [Making Embedded Systems, 2nd ed.](https://www.oreilly.com/library/view/making-embedded-systems/9781098151539/)
  (Elecia White, 2024): the standard book on firmware design — modules, layering,
  state machines. Not free, but university libraries often have O'Reilly access.
- [Modern Embedded Systems Programming](https://www.state-machine.com/video-course)
  (Miro Samek): a free video course from bare-metal C up to RTOSes, state machines
  and event-driven design. A good fit for flight-state logic.
- [BARR-C:2018 Embedded C Coding Standard](https://barrgroup.com/embedded-c-coding-standard):
  a free, short set of widely used rules for safer embedded C.

**Under the hood (§1–3 in depth)**

- [From Zero to main(): Bare metal C](https://interrupt.memfault.com/blog/zero-to-main-1):
  everything between reset and `main()`.
- [Demystifying Firmware Linker Scripts](https://interrupt.memfault.com/blog/how-to-write-linker-scripts-for-firmware):
  sections, memory regions and the linker script.
- [Bootstrapping libc with Newlib](https://interrupt.memfault.com/blog/bootstrapping-libc-with-newlib):
  where the `_write is not implemented` build warnings come from, and how `printf`
  gets connected to real hardware.
- [Interrupt](https://interrupt.memfault.com): Memfault's blog of practical firmware
  engineering articles, worth browsing.

---

## Glossary

| Term | Meaning |
|---|---|
| **Cross-compiler** | runs on one kind of CPU, produces code for another |
| **Section** | a named chunk of compiled output (`.text`, `.data`…) that the linker places in memory |
| **Linker script** | describes the chip's memory and which section goes where |
| **ELF** | the linker's output: code, addresses, and debug info |
| **Vector table** | the list of addresses at the start of flash: initial stack pointer, reset handler, interrupt handlers |
| **SWD** | the two-wire debug interface built into Cortex-M chips |
| **ST-LINK** | the USB-to-SWD adapter built onto the NUCLEO |
| **HAL** | ST's Hardware Abstraction Layer — functions like `HAL_GPIO_TogglePin` |
| **CMSIS** | Arm's standard definitions: *core* (same on every Cortex-M) and *device* (this chip's registers) |
| **MSP** | MCU Support Package: the HAL hook that enables a peripheral's clock and pins |
| **Tick** | one SysTick interrupt — 1 ms in this project |
| **Queue** | FreeRTOS's safe way to pass data from one task (or interrupt) to another |
| **Reentrant** | safe to run from several tasks at once, because each keeps its own state |
