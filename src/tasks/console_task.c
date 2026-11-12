#include "tasks/console_task.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "stream_buffer.h"
#include "task.h"

#include "usart.h"
#include "app/radio_link.h"
#include "app/command.h"
#include "app/crc32.h"
#include "app/flash_log.h"
#include "app/stand.h"
#include "tasks/actuator_task.h"
#include "tasks/datalog_task.h"
#include "drivers/max31856.h"
#include "drivers/e22.h"
#include "drivers/pt_adc.h"
#include "drivers/w25q128.h"
#include "tasks/interlock_task.h"
#include "tasks/lc_task.h"
#include "tasks/log_task.h"
#include "tasks/pt_task.h"
#include "tasks/radio_task.h"
#include "tasks/tc_task.h"

#define RX_BUFFER_SIZE  64U
#define LINE_MAX        80U   // longest command line, including its arguments
#define ARGS_MAX        8     // most words in one command
#define SEND_TIMEOUT_MS 100U
#define STREAM_PERIOD_MS 100U
#define FLASH_RUNS_LISTED 16U // the newest ones

typedef struct {
    const char *name;
    const char *usage;   // shown by "help"
    void (*run)(int argc, char *argv[]);
} console_command_t;

static bool at_serial_console(const char *command);
static void print_table_error(void);
static void cmd_help(int argc, char *argv[]);
static void cmd_status(int argc, char *argv[]);
static void cmd_radio(int argc, char *argv[]);
static void cmd_stand(int argc, char *argv[]);
static void cmd_off(int argc, char *argv[]);
static void cmd_safe(int argc, char *argv[]);
static void cmd_sol(int argc, char *argv[]);
static void cmd_srv(int argc, char *argv[]);
static void cmd_stp(int argc, char *argv[]);
static void cmd_pt(int argc, char *argv[]);
static void cmd_tc(int argc, char *argv[]);
static void cmd_lc(int argc, char *argv[]);
static void cmd_log(int argc, char *argv[]);
static void cmd_flash(int argc, char *argv[]);

// To add a command: write its handler below, declare it above, add a row here.
static const console_command_t commands[] = {
    { "help",   "list commands",                               cmd_help   },
    { "status", "interlock, VBAT and every actuator",           cmd_status },
    { "radio",  "[send]: the link; send transmits one packet",  cmd_radio  },
    { "off",    "every actuator off",                           cmd_off    },
    { "safe",   "every actuator to its safe state",             cmd_safe   },
    { "sol",    "<1-4 or name> <on|off>: switch a solenoid",    cmd_sol    },
    { "srv",    "<1-4 or name> <us|off>: set a servo's pulse",  cmd_srv    },
    { "stp",    "home | zero | stop | go <step> | jog <steps>", cmd_stp    },
    { "pt",     "[stream]: pressures; stream runs until a key", cmd_pt     },
    { "tc",     "[stream]: thermocouples",                      cmd_tc     },
    { "lc",     "[stream | tare <cell> | cal <cell> <N>]: load cells 1 and 2", cmd_lc },
    { "log",    "[start | stop]: record to SD and flash",       cmd_log    },
    { "flash",  "[dump [run]]: flash runs; dump is for tools/flash_dump.py", cmd_flash },
    { "stand",  "[1|2]: which stand this is, and what is wired to it", cmd_stand },
};
static const size_t command_count = sizeof commands / sizeof commands[0];

static StreamBufferHandle_t rx_buffer;
static TaskHandle_t console_task_handle;

void console_init(void)
{
    rx_buffer = xStreamBufferCreate(RX_BUFFER_SIZE, 1);
    if (rx_buffer == NULL) {
        Error_Handler();
    }

    // Priority 6 is below the FreeRTOS line, so the handler may call FreeRTOS.
    HAL_NVIC_SetPriority(USART3_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXFNE);
}

void USART3_IRQHandler(void)
{
    // An overrun raises this interrupt too, and keeps raising it until cleared.
    __HAL_UART_CLEAR_FLAG(&huart3, UART_CLEAR_OREF);

    // Empty the FIFO. If this handler ran late, several bytes can be waiting.
    BaseType_t woken = pdFALSE;
    while (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXFNE)) {
        uint8_t byte = (uint8_t)huart3.Instance->RDR;
        xStreamBufferSendFromISR(rx_buffer, &byte, 1, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

// Splits the line into words and runs the matching command. Two tasks call it,
// so a command keeps what it's working on in local variables.
void console_run_line(char *line)
{
    char *argv[ARGS_MAX];
    int argc = 0;
    char *rest;   // where strtok_r picks up next time

    // Into the data log as it came, before it's cut up.
    size_t length = strlen(line);
    if (length > 0) {
        datalog_write(RECORD_TEXT, line, length);
    }

    // A line sent over the radio may still end in the sender's CR or LF.
    for (char *word = strtok_r(line, " \r\n", &rest); word != NULL && argc < ARGS_MAX;
         word = strtok_r(NULL, " \r\n", &rest)) {
        argv[argc++] = word;
    }
    if (argc == 0) {
        return;
    }

    for (size_t i = 0; i < command_count; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].run(argc, argv);
            return;
        }
    }
    printf("unknown command '%s', try 'help'\r\n", argv[0]);
}

void vConsoleTask(void *pvParameters)
{
    (void)pvParameters;

    console_task_handle = xTaskGetCurrentTaskHandle();
    char line[LINE_MAX];
    size_t length = 0;
    char previous = 0;

    print_table_error();
    printf("Type 'help' for commands.\r\n");
    log_write("> ", 2);

    for (;;) {
        char key;
        xStreamBufferReceive(rx_buffer, &key, 1, portMAX_DELAY);

        // Terminals send CR, LF or CR+LF for Enter. In a CR+LF the CR has
        // already run the line, so the LF is skipped.
        bool lf_after_cr = key == '\n' && previous == '\r';
        previous = key;
        if (lf_after_cr) {
            continue;
        }

        if (key == '\r' || key == '\n') {
            log_write("\r\n", 2);
            line[length] = '\0';
            console_run_line(line);
            length = 0;
            log_write("> ", 2);
        } else if (key == '\b' || key == 0x7F) {
            // Backspace (macOS sends 0x7F): step back, blank the character, step back.
            if (length > 0) {
                length--;
                log_write("\b \b", 3);
            }
        } else if (key >= ' ' && key <= '~' && length < LINE_MAX - 1) {
            // Printable characters are kept and echoed; arrow keys and the like are ignored.
            line[length++] = key;
            log_write(&key, 1);
        }
    }
}

// ------------------------------------------------------------------ commands

// Waits until the actuator task has taken the command, so its reply prints
// before the next prompt. It takes commands every 10 ms.
static void send_command(const command_t *command)
{
    if (xQueueSend(actuator_queue, command, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) != pdPASS) {
        printf("not sent: actuator queue full\r\n");
        return;
    }
    while (uxQueueMessagesWaiting(actuator_queue) > 0) {
        vTaskDelay(1);
    }
}

// True if the whole word is a decimal number, with an optional sign.
static bool read_number(const char *text, long *value)
{
    char *end;
    *value = strtol(text, &end, 10);
    return *end == '\0';
}

static void cmd_help(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < command_count; i++) {
        printf("  %-7s %s\r\n", commands[i].name, commands[i].usage);
    }
}

static void cmd_status(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    command_t command = { .type = COMMAND_STATUS };
    send_command(&command);
}

static void cmd_off(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    command_t command = { .type = COMMAND_ALL_OFF };
    send_command(&command);
}

// The same thing the launch box's SAFE and a lost radio link do.
static void cmd_safe(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    command_t command = { .type = COMMAND_SAFE };
    send_command(&command);
}

static void cmd_radio(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "send") == 0) {
        printf(radio_send_test() ? "radio: sending one telemetry packet\r\n"
                                 : "radio: not running\r\n");
        return;
    }
    if (argc != 1) {
        printf("usage: radio [send]\r\n");
        return;
    }

    interlock_state_t interlock = interlock_current_state();
    if (interlock != INTERLOCK_ENABLED) {
        printf("radio: HAT has no power (interlock %s)\r\n", interlock_state_names[interlock]);
        return;
    }

    radio_status_t status;
    radio_status(&status);
    if (!status.started) {
        printf("radio: SX1262 not responding\r\n");
        return;
    }
    printf("radio: stand %u, listening on %lu kHz, sending at %d dBm from the chip, link %s%s\r\n",
           stand_number(), (unsigned long)(RADIO_FREQUENCY_HZ / 1000UL), E22_TX_POWER_DBM,
           status.linked ? "up" : "down",
           status.aborted ? ", went safe by itself: turn the launch box's key off to clear" : "");
    printf("radio: %lu received (last %d dBm, SNR %d dB), %lu rejected, %lu sent, %lu restarts\r\n",
           (unsigned long)status.received, status.rssi_dbm, status.snr_db,
           (unsigned long)status.rejected, (unsigned long)status.sent,
           (unsigned long)status.restarts);
}

// One line of the "stand" listing: a channel's name, and for an output what it
// drives and how.
static void print_channel(channel_kind_t kind, unsigned index)
{
    const char *name = stand_name(kind, index);
    printf("%s %u %s", stand_kind_text(kind), index + 1, name[0] != '\0' ? name : "-");

    const output_t *row = NULL;
    if (kind == KIND_SOL || kind == KIND_SRV) {
        row = stand_output((kind == KIND_SOL ? 0 : SOLENOID_COUNT) + index);
    }
    if (row == NULL || row->drives == NOT_A_VALVE) {
        printf("\r\n");
    } else if (kind == KIND_SOL) {
        printf(": %s, open is %s\r\n", stand_drives_text(row->drives), row->inverted ? "off" : "on");
    } else {
        printf(": %s, closed %u us, open %u us\r\n", stand_drives_text(row->drives), row->closed_us,
               row->open_us);
    }
}

// Says so if the table in app/stand_table.c has a mistake in it.
static void print_table_error(void)
{
    char why[96];
    if (!stand_check(stand_table, RADIO_STAND_MAX, why, sizeof why)) {
        printf("stand table: %s\r\n", why);
    }
}

// "stand" says which stand this is and lists what's wired to it: the names and
// valves of its row in app/stand_table.c. "stand 2" changes the number, the one
// thing a stand saves.
static void cmd_stand(int argc, char *argv[])
{
    long stand;
    if (argc == 2 && read_number(argv[1], &stand) && stand >= 1 && stand <= (long)RADIO_STAND_MAX) {
        // Not over the radio: the launch box is talking to the old number.
        if (!at_serial_console("stand")) {
            return;
        }
        stand_select((uint8_t)stand);
        if (!stand_save()) {
            printf("stand: not saved to flash, kept until power off\r\n");
        }
    } else if (argc != 1) {
        printf("usage: stand [1|2]\r\n");
        return;
    }

    printf("stand %u\r\n", stand_number());
    for (unsigned kind = 0; kind < KIND_COUNT; kind++) {
        for (unsigned i = 0; i < stand_kind_count(kind); i++) {
            print_channel(kind, i);
        }
    }
    print_table_error();
}

static void cmd_sol(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: sol <1-4 or name> <on|off>\r\n");
        return;
    }

    int index = stand_index(KIND_SOL, argv[1]);
    bool on = strcmp(argv[2], "on") == 0;
    if (index < 0 || (!on && strcmp(argv[2], "off") != 0)) {
        printf("usage: sol <1-4 or name> <on|off>\r\n");
        return;
    }

    command_t command = { .type = COMMAND_SOLENOID, .channel = (uint8_t)index, .on = on };
    send_command(&command);
}

// The actuator task checks the pulse against that servo's limits.
static void cmd_srv(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: srv <1-4 or name> <us|off>\r\n");
        return;
    }

    int index = stand_index(KIND_SRV, argv[1]);
    long pulse_us = 0;   // "off"
    bool off = strcmp(argv[2], "off") == 0;
    if (index < 0
        || (!off && (!read_number(argv[2], &pulse_us) || pulse_us < 1 || pulse_us > UINT16_MAX))) {
        printf("usage: srv <1-4 or name> <us|off>\r\n");
        return;
    }

    command_t command = { .type = COMMAND_SERVO, .channel = (uint8_t)index,
                          .pulse_us = (uint16_t)pulse_us };
    send_command(&command);
}

// go is a position from home, jog a distance from here (negative toward the switch).
// The actuator task checks the interlock, home and travel.
static void cmd_stp(int argc, char *argv[])
{
    command_t command = {0};
    long steps = 0;

    if (argc == 2 && strcmp(argv[1], "home") == 0) {
        command.type = COMMAND_STEPPER_HOME;
    } else if (argc == 2 && strcmp(argv[1], "zero") == 0) {
        command.type = COMMAND_STEPPER_ZERO;
    } else if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        command.type = COMMAND_STEPPER_STOP;
    } else if (argc == 3 && strcmp(argv[1], "go") == 0 && read_number(argv[2], &steps)) {
        command.type = COMMAND_STEPPER_GO;
    } else if (argc == 3 && strcmp(argv[1], "jog") == 0 && read_number(argv[2], &steps)) {
        command.type = COMMAND_STEPPER_JOG;
    } else {
        printf("usage: stp home | zero | stop | go <step> | jog <steps>\r\n");
        return;
    }

    command.steps = (int32_t)steps;
    send_command(&command);
}

// newlib-nano's printf has no %f, so this prints value * 10^decimals as two
// integers either side of the point. The sign is done by hand so -0.4 isn't "0.4".
static void format_fixed(char *text, size_t size, float value, unsigned decimals)
{
    long scale = 1;
    for (unsigned i = 0; i < decimals; i++) {
        scale *= 10;
    }
    long scaled = (long)(value * (float)scale + (value < 0.0f ? -0.5f : 0.5f));
    unsigned long magnitude = (unsigned long)(scaled < 0 ? -scaled : scaled);
    snprintf(text, size, "%s%lu.%0*lu", scaled < 0 ? "-" : "", magnitude / (unsigned long)scale,
             (int)decimals, magnitude % (unsigned long)scale);
}

static void print_pts(void)
{
    for (unsigned i = 0; i < PT_COUNT; i++) {
        pt_reading_t reading;
        char name[NAME_SIZE + 1];
        pt_latest(i, &reading);
        stand_spaced_name(KIND_PT, i, name);
        if (!reading.powered) {
            printf("pt%u%s no supply (%lu mV)\r\n", i + 1, name, (unsigned long)reading.supply_mv);
            continue;
        }

        char psi[16];
        char ratio[16];
        format_fixed(psi, sizeof psi, reading.psi, 1);
        format_fixed(ratio, sizeof ratio, reading.ratio, 4);
        printf("pt%u%s %s psi  ratio %s  out %lu mV  supply %lu mV\r\n", i + 1, name, psi, ratio,
               (unsigned long)reading.data_mv, (unsigned long)reading.supply_mv);
    }
}

// Drops anything that arrives in the next 10 ms: the rest of a key press, since
// Enter can be CR then LF.
static void drain_input(void)
{
    char key;
    while (xStreamBufferReceive(rx_buffer, &key, 1, pdMS_TO_TICKS(10)) > 0) {
    }
}

// True in the console task. A command that only a key at the serial port can end
// uses it to refuse a line that came over the radio.
static bool at_serial_console(const char *command)
{
    if (xTaskGetCurrentTaskHandle() == console_task_handle) {
        return true;
    }
    printf("%s: at the serial console only\r\n", command);
    return false;
}

// Calls print every 100 ms until a key comes in.
static void stream(void (*print)(void))
{
    if (!at_serial_console("stream")) {
        return;
    }

    // The LF of the Enter that ran this command may still be on its way.
    drain_input();
    printf("press any key to stop\r\n");
    char key;
    do {
        print();
    } while (xStreamBufferReceive(rx_buffer, &key, 1, pdMS_TO_TICKS(STREAM_PERIOD_MS)) == 0);
    drain_input();
}

static void cmd_pt(int argc, char *argv[])
{
    if (argc == 1) {
        print_pts();
    } else if (argc == 2 && strcmp(argv[1], "stream") == 0) {
        stream(print_pts);
    } else {
        printf("usage: pt [stream]\r\n");
    }
}

static void print_tcs(void)
{
    interlock_state_t interlock = interlock_current_state();
    if (interlock != INTERLOCK_ENABLED) {
        printf("tc: HAT has no power (interlock %s)\r\n", interlock_state_names[interlock]);
        return;
    }

    for (unsigned i = 0; i < TC_COUNT; i++) {
        tc_reading_t reading;
        char name[NAME_SIZE + 1];
        tc_latest(i, &reading);
        stand_spaced_name(KIND_TC, i, name);
        // Not set up yet, not answering, or the last read failed.
        if (!reading.present) {
            printf("tc%u%s no reading\r\n", i + 1, name);
            continue;
        }

        char temp[16];
        char cold_junction[16];
        format_fixed(temp, sizeof temp, reading.value.temp_c, 1);
        format_fixed(cold_junction, sizeof cold_junction, reading.value.cold_junction_c, 1);

        // One name per fault bit that's set.
        char faults[128] = "";
        size_t used = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            if (reading.value.faults & (1U << bit)) {
                used += (size_t)snprintf(faults + used, sizeof faults - used, "  %s",
                                         max31856_fault_names[bit]);
            }
        }
        printf("tc%u%s %s C  cold junction %s C%s\r\n", i + 1, name, temp, cold_junction, faults);
    }
}

static void cmd_tc(int argc, char *argv[])
{
    if (argc == 1) {
        print_tcs();
    } else if (argc == 2 && strcmp(argv[1], "stream") == 0) {
        stream(print_tcs);
    } else {
        printf("usage: tc [stream]\r\n");
    }
}

static void print_lc(void)
{
    interlock_state_t interlock = interlock_current_state();
    if (interlock != INTERLOCK_ENABLED) {
        printf("lc: HAT has no power (interlock %s)\r\n", interlock_state_names[interlock]);
        return;
    }

    for (unsigned i = 0; i < LC_COUNT; i++) {
        lc_reading_t reading;
        char name[NAME_SIZE + 1];
        lc_latest(i, &reading);
        stand_spaced_name(KIND_LC, i, name);
        if (!reading.present) {
            printf("lc%u%s no reading\r\n", i + 1, name);
            continue;
        }

        char mv_per_v[24];
        format_fixed(mv_per_v, sizeof mv_per_v, reading.mv_per_v, 5);
        if (!reading.calibrated) {
            printf("lc%u%s %s mV/V  uncalibrated\r\n", i + 1, name, mv_per_v);
            continue;
        }
        char newtons[16];
        format_fixed(newtons, sizeof newtons, reading.newtons, 1);
        printf("lc%u%s %s N  %s mV/V\r\n", i + 1, name, newtons, mv_per_v);
    }
}

// "lc tare 1" with cell 1 unloaded, then "lc cal 1 <N>" with a known load on it.
// The cell is always given, by number or name, so "lc cal 2" can't be read as
// "cell 1, 2 newtons".
static void cmd_lc(int argc, char *argv[])
{
    static const char usage[] = "usage: lc [stream | tare <1-2 or name> | cal <1-2 or name> <N>]\r\n";
    int cell = argc >= 3 ? stand_index(KIND_LC, argv[2]) : -1;

    if (argc == 1) {
        print_lc();
    } else if (argc == 2 && strcmp(argv[1], "stream") == 0) {
        stream(print_lc);
    } else if (argc == 3 && cell >= 0 && strcmp(argv[1], "tare") == 0) {
        printf("taring, keep the cell still for a second\r\n");
        printf(lc_tare((unsigned)cell) ? "lc %d zeroed\r\n" : "lc %d tare failed: no reading\r\n", cell + 1);
    } else if (argc == 4 && cell >= 0 && strcmp(argv[1], "cal") == 0) {
        char *end;
        float newtons = strtof(argv[3], &end);
        if (*end != '\0' || newtons == 0.0f) {
            printf(usage);
            return;
        }
        printf("calibrating, keep the load still for a second\r\n");
        printf(lc_calibrate((unsigned)cell, newtons) ? "lc %d calibrated\r\n" : "lc %d cal failed: no reading\r\n",
               cell + 1);
    } else {
        printf(usage);
    }
}

static void cmd_log(int argc, char *argv[])
{
    if (argc == 1) {
        datalog_status_t status;
        datalog_status(&status);
        if (status.file_name[0] == '\0' && status.flash_run == 0) {
            printf("log: not recording\r\n");
            return;
        }
        const char *state = status.recording ? "writing" : "stopped, last";
        if (status.file_name[0] != '\0') {
            printf("log: %s file %s, %lu bytes\r\n", state, status.file_name,
                   (unsigned long)status.bytes_written);
        }
        if (status.flash_run != 0) {
            printf("log: %s flash run %lu\r\n", state, (unsigned long)status.flash_run);
        }
        printf("log: %lu records dropped\r\n", (unsigned long)status.dropped);
    } else if (argc == 2 && strcmp(argv[1], "start") == 0) {
        datalog_start();
    } else if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        datalog_stop();
    } else {
        printf("usage: log [start | stop]\r\n");
    }
}

// Sends a whole run for tools/flash_dump.py. Every frame goes through the debug
// log's queue, so anything printed meanwhile lands between frames and the tool
// skips it. At 115200 a 64 KB block takes about 6 s.
static void flash_dump(const flash_log_run_t *run)
{
    _Static_assert(sizeof(flash_dump_frame_t) <= LOG_MESSAGE_MAX, "a frame must fit one message");
    static flash_dump_frame_t frame = { .sync = { DUMP_SYNC_0, DUMP_SYNC_1 } };
    const size_t checked = sizeof frame - sizeof frame.sync - sizeof frame.crc;
    uint32_t total = run->blocks * FLASH_LOG_BLOCK_SIZE;

    if (!at_serial_console("flash dump")) {
        return;
    }

    // The LF of the Enter that ran this command may still be on its way.
    drain_input();
    printf("flash: sending run %lu, %lu blocks, any key stops\r\n",
           (unsigned long)run->run, (unsigned long)run->blocks);

    for (uint32_t offset = 0; offset < total; offset += frame.length) {
        uint32_t in_block = offset % FLASH_LOG_BLOCK_SIZE;
        uint32_t left = FLASH_LOG_BLOCK_SIZE - in_block;
        frame.length = (uint8_t)(left < DUMP_DATA_SIZE ? left : DUMP_DATA_SIZE);
        frame.offset = offset;
        if (!w25q128_read(flash_log_block_address(run, offset / FLASH_LOG_BLOCK_SIZE) + in_block,
                          frame.data, frame.length)) {
            printf("flash: read failed\r\n");
            return;
        }
        frame.crc = crc32(&frame.length, checked);
        log_write_wait(&frame, sizeof frame);

        char key;
        if (xStreamBufferReceive(rx_buffer, &key, 1, 0) > 0) {
            drain_input();
            printf("flash: stopped\r\n");
            return;
        }
    }

    frame.length = 0;
    frame.offset = total;
    frame.crc = crc32(&frame.length, checked);
    log_write_wait(&frame, sizeof frame);
    printf("flash: done\r\n");
}

static void cmd_flash(int argc, char *argv[])
{
    bool dump = argc >= 2 && strcmp(argv[1], "dump") == 0;
    if (argc > 3 || (argc >= 2 && !dump)) {
        printf("usage: flash [dump [run]]\r\n");
        return;
    }

    flash_log_run_t runs[FLASH_RUNS_LISTED];
    unsigned count;
    if (!flash_log_runs(runs, FLASH_RUNS_LISTED, &count)) {
        printf("flash: not responding (the HAT needs power)\r\n");
        return;
    }
    if (count == 0) {
        printf("flash: no runs\r\n");
        return;
    }

    if (!dump) {
        for (unsigned i = 0; i < count; i++) {
            printf("flash: run %lu, %lu KB%s\r\n", (unsigned long)runs[i].run,
                   (unsigned long)(runs[i].blocks * FLASH_LOG_BLOCK_SIZE / 1024UL),
                   runs[i].start_lost ? ", start overwritten" : "");
        }
        return;
    }

    datalog_status_t status;
    datalog_status(&status);
    if (status.recording) {
        printf("flash: still recording, run 'log stop' first\r\n");
        return;
    }

    unsigned long wanted = argc == 3 ? strtoul(argv[2], NULL, 10) : runs[0].run;
    for (unsigned i = 0; i < count; i++) {
        if (runs[i].run == wanted) {
            flash_dump(&runs[i]);
            return;
        }
    }
    printf("flash: no run %lu\r\n", wanted);
}
