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
#include "app/command.h"
#include "tasks/actuator_task.h"
#include "drivers/max31856.h"
#include "drivers/pt_adc.h"
#include "tasks/interlock_task.h"
#include "tasks/log_task.h"
#include "tasks/pt_task.h"
#include "tasks/tc_task.h"

#define RX_BUFFER_SIZE  64U
#define LINE_MAX        80U   // longest command line, including its arguments
#define ARGS_MAX        8     // most words in one command
#define SEND_TIMEOUT_MS 100U
#define STREAM_PERIOD_MS 100U

typedef struct {
    const char *name;
    const char *usage;   // shown by "help"
    void (*run)(int argc, char *argv[]);
} console_command_t;

static void cmd_help(int argc, char *argv[]);
static void cmd_status(int argc, char *argv[]);
static void cmd_off(int argc, char *argv[]);
static void cmd_sol(int argc, char *argv[]);
static void cmd_srv(int argc, char *argv[]);
static void cmd_stp(int argc, char *argv[]);
static void cmd_pt(int argc, char *argv[]);
static void cmd_tc(int argc, char *argv[]);

// To add a command: write its handler below, declare it above, add a row here.
static const console_command_t commands[] = {
    { "help",   "list commands",                               cmd_help   },
    { "status", "interlock, VBAT and every actuator",           cmd_status },
    { "off",    "every actuator off",                           cmd_off    },
    { "sol",    "<1-4> <on|off>: switch a solenoid",            cmd_sol    },
    { "srv",    "<1-4> <us|off>: set a servo's pulse",          cmd_srv    },
    { "stp",    "home | zero | stop | go <step> | jog <steps>", cmd_stp    },
    { "pt",     "[stream]: pressures; stream runs until a key", cmd_pt     },
    { "tc",     "[stream]: thermocouples",                      cmd_tc     },
};
static const size_t command_count = sizeof commands / sizeof commands[0];

static StreamBufferHandle_t rx_buffer;

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

// Splits the line into words and runs the matching command.
static void run_line(char *line)
{
    char *argv[ARGS_MAX];
    int argc = 0;
    char *rest;   // where strtok_r picks up next time

    for (char *word = strtok_r(line, " ", &rest); word != NULL && argc < ARGS_MAX;
         word = strtok_r(NULL, " ", &rest)) {
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

    char line[LINE_MAX];
    size_t length = 0;
    char previous = 0;

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
            run_line(line);
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

static void cmd_sol(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: sol <1-4> <on|off>\r\n");
        return;
    }

    long number;
    bool on = strcmp(argv[2], "on") == 0;
    if (!read_number(argv[1], &number) || number < 1 || number > SOLENOID_COUNT
        || (!on && strcmp(argv[2], "off") != 0)) {
        printf("usage: sol <1-4> <on|off>\r\n");
        return;
    }

    command_t command = { .type = COMMAND_SOLENOID, .channel = (uint8_t)(number - 1), .on = on };
    send_command(&command);
}

// The actuator task checks the pulse against that servo's limits.
static void cmd_srv(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: srv <1-4> <us|off>\r\n");
        return;
    }

    long number;
    long pulse_us = 0;   // "off"
    bool off = strcmp(argv[2], "off") == 0;
    if (!read_number(argv[1], &number) || number < 1 || number > SERVO_COUNT
        || (!off && (!read_number(argv[2], &pulse_us) || pulse_us < 1 || pulse_us > UINT16_MAX))) {
        printf("usage: srv <1-4> <us|off>\r\n");
        return;
    }

    command_t command = { .type = COMMAND_SERVO, .channel = (uint8_t)(number - 1),
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
        pt_latest(i, &reading);
        if (!reading.powered) {
            printf("pt%u no supply (%lu mV)\r\n", i + 1, (unsigned long)reading.supply_mv);
            continue;
        }

        char psi[16];
        char ratio[16];
        format_fixed(psi, sizeof psi, reading.psi, 1);
        format_fixed(ratio, sizeof ratio, reading.ratio, 4);
        printf("pt%u %s psi  ratio %s  out %lu mV  supply %lu mV\r\n", i + 1, psi, ratio,
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

// Calls print every 100 ms until a key comes in.
static void stream(void (*print)(void))
{
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
        tc_latest(i, &reading);
        // Not set up yet, not answering, or the last read failed.
        if (!reading.present) {
            printf("tc%u no reading\r\n", i + 1);
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
        printf("tc%u %s C  cold junction %s C%s\r\n", i + 1, temp, cold_junction, faults);
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
