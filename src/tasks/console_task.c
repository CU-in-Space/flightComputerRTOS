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
#include "tasks/log_task.h"

#define RX_BUFFER_SIZE  64U
#define LINE_MAX        80U   // longest command line, including its arguments
#define ARGS_MAX        8     // most words in one command
#define SEND_TIMEOUT_MS 100U

typedef struct {
    const char *name;
    const char *usage;   // shown by "help"
    void (*run)(int argc, char *argv[]);
} console_command_t;

static void cmd_help(int argc, char *argv[]);
static void cmd_status(int argc, char *argv[]);
static void cmd_off(int argc, char *argv[]);
static void cmd_sol(int argc, char *argv[]);

// To add a command: write its handler below, declare it above, add a row here.
static const console_command_t commands[] = {
    { "help",   "list commands",                     cmd_help   },
    { "status", "interlock, VBAT and each solenoid", cmd_status },
    { "off",    "every actuator off",                cmd_off    },
    { "sol",    "<1-4> <on|off>: switch a solenoid", cmd_sol    },
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

    char *end;
    unsigned long number = strtoul(argv[1], &end, 10);
    bool on = strcmp(argv[2], "on") == 0;
    if (*end != '\0' || number < 1 || number > SOLENOID_COUNT
        || (!on && strcmp(argv[2], "off") != 0)) {
        printf("usage: sol <1-4> <on|off>\r\n");
        return;
    }

    command_t command = { .type = COMMAND_SOLENOID, .channel = (uint8_t)(number - 1), .on = on };
    send_command(&command);
}
