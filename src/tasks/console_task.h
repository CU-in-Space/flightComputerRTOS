#ifndef TASKS_CONSOLE_TASK_H
#define TASKS_CONSOLE_TASK_H

// Command prompt on USART3, the same ST-LINK serial port as the debug log.
// Echoes each key, handles backspace, runs the line on Enter. Type "help".

// Creates the receive buffer and turns on the USART3 receive interrupt.
void console_init(void);
void vConsoleTask(void *pvParameters);

#endif /* TASKS_CONSOLE_TASK_H */
