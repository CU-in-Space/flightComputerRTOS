#ifndef TASKS_CONSOLE_TASK_H
#define TASKS_CONSOLE_TASK_H

// Command prompt on USART3, the same ST-LINK serial port as the debug log.
// Echoes each key, handles backspace, runs the line on Enter. Type "help".

// Creates the receive buffer and turns on the USART3 receive interrupt.
void console_init(void);
void vConsoleTask(void *pvParameters);

// Runs one command line and copies it to the data log. Cuts the line up in
// place. The remote task calls it too, for lines that came over the radio.
void console_run_line(char *line);

#endif /* TASKS_CONSOLE_TASK_H */
