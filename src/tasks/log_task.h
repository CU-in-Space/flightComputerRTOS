/* Debug log on USART3 (the ST-LINK virtual COM port, 115200 8N1).
 *
 * Use printf. Its output goes through _write (src/libc/syscalls.c) into a
 * queue, and vLogTask sends it at the lowest priority. Don't print from an
 * interrupt handler.
 */
#ifndef TASKS_LOG_TASK_H
#define TASKS_LOG_TASK_H

#include <stddef.h>

#define LOG_MESSAGE_MAX  128U   // longer writes are split across messages

// Creates the queue. Call before the scheduler starts.
void log_init(void);

// Queues text for the log task. Before the scheduler starts it sends straight
// to the UART instead. Drops the text if the queue is full.
void log_write(const char *text, size_t length);

// Queues one message of at most LOG_MESSAGE_MAX bytes, waiting for room. It goes
// out whole, so printf text can only land between two of these.
void log_write_wait(const void *data, size_t length);

void vLogTask(void *pvParameters);

#endif /* TASKS_LOG_TASK_H */
