#ifndef TASKS_CONSOLE_TASK_H
#define TASKS_CONSOLE_TASK_H

#include <stdbool.h>

// Command prompt on USART3, the same ST-LINK serial port as the debug log.
// Echoes each key, handles backspace, runs the line on Enter. Up and down step
// through the last 16 lines typed here (not ones from the radio). Type "help".

// Creates the receive buffer and turns on the USART3 receive interrupt.
void console_init(void);
void vConsoleTask(void *pvParameters);

// Runs one command line and copies it to the data log, marked "typed>" or
// "radio>". Cuts the line up in place. The remote task calls it too, for lines
// that came over the radio.
void console_run_line(char *line, bool from_radio);

#endif /* TASKS_CONSOLE_TASK_H */
