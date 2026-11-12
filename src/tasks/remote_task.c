#include "tasks/remote_task.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "main.h"
#include "app/radio_link.h"
#include "tasks/console_task.h"

#define QUEUE_DEPTH  4U

typedef struct {
    char text[RADIO_COMMAND_MAX + 1];
} remote_line_t;

static QueueHandle_t remote_queue;

void remote_init(void)
{
    remote_queue = xQueueCreate(QUEUE_DEPTH, sizeof(remote_line_t));
    if (remote_queue == NULL) {
        Error_Handler();
    }
}

bool remote_submit(const char *text, size_t length)
{
    if (length > RADIO_COMMAND_MAX) {
        return false;
    }
    remote_line_t line;
    memcpy(line.text, text, length);
    line.text[length] = '\0';
    return xQueueSend(remote_queue, &line, 0) == pdPASS;
}

void remote_flush(void)
{
    xQueueReset(remote_queue);
}

void vRemoteTask(void *pvParameters)
{
    (void)pvParameters;

    remote_line_t line;
    for (;;) {
        xQueueReceive(remote_queue, &line, portMAX_DELAY);
        // Shown on the serial port the way a typed command would be.
        printf("radio> %s\r\n", line.text);
        console_run_line(line.text);
    }
}
