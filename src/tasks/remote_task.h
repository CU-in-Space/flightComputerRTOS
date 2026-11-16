#ifndef TASKS_REMOTE_TASK_H
#define TASKS_REMOTE_TASK_H

#include <stdbool.h>
#include <stddef.h>

// Runs the command lines that arrive over the radio, one at a time, as if they
// had been typed at the console. It's a task of its own so that a slow command
// (log start, lc tare) never keeps the radio task from answering.

// Creates the queue. Call before the scheduler starts.
void remote_init(void);

// Runs each queued line through the console's commands.
void vRemoteTask(void *pvParameters);

// Queues one line of at most RADIO_COMMAND_MAX characters; no terminator needed.
// False if it's too long or four are already waiting.
bool remote_submit(const char *text, size_t length);

// Drops the lines still waiting. The one already running finishes.
void remote_flush(void);

#endif /* TASKS_REMOTE_TASK_H */
