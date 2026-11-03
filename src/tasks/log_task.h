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

void log_init(void);
void log_write(const char *text, size_t length);
void vLogTask(void *pvParameters);

#endif /* TASKS_LOG_TASK_H */
