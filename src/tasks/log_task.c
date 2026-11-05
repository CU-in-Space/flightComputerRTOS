/* Debug log: tasks write into a queue; this task drains it onto USART3.
 *
 * HAL_UART_Transmit blocks until the last byte is out (~11 ms for a full
 * message), so only this task calls it, at the lowest priority. Every task
 * that matters preempts it, and no caller ever waits on the UART.
 */
#include "log_task.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "usart.h"

#define LOG_QUEUE_DEPTH  32U    // ~4 KB of heap, ~0.35 s of backlog

typedef struct {
    uint16_t length;
    char text[LOG_MESSAGE_MAX];
} log_message_t;

static QueueHandle_t log_queue;

void log_init(void)
{
    log_queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(log_message_t));
    if (log_queue == NULL) {
        Error_Handler();
    }
}

void log_write(const char *text, size_t length)
{
    // Before the scheduler nothing drains the queue, and a crash would lose it.
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)length, HAL_MAX_DELAY);
        return;
    }

    while (length > 0) {
        log_message_t message;
        message.length = length < LOG_MESSAGE_MAX ? length : LOG_MESSAGE_MAX;
        memcpy(message.text, text, message.length);

        // Timeout 0: if the queue is full, drop the line rather than wait.
        xQueueSend(log_queue, &message, 0);

        text += message.length;
        length -= message.length;
    }
}

void vLogTask(void *pvParameters)
{
    (void)pvParameters;

    log_message_t message;
    for (;;) {
        xQueueReceive(log_queue, &message, portMAX_DELAY);
        HAL_UART_Transmit(&huart3, (uint8_t *)message.text, message.length, HAL_MAX_DELAY);
    }
}
