#ifndef LIBC_LOCKS_H
#define LIBC_LOCKS_H

/* Create the mutexes behind the C library's internal locks. Call once from
 * main(), before creating any task. See locks.c for why this exists. */
void libc_locks_init(void);

#endif /* LIBC_LOCKS_H */
