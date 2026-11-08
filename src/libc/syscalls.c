/* newlib system calls.
 *
 * newlib, the C library, expects the platform to provide a few low-level
 * functions that an operating system would normally supply. This file holds the
 * ones this firmware implements.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "tasks/log_task.h"

/* Linker-script symbols. Only their addresses mean anything. */
extern uint8_t end;              /* first byte after .bss: where the heap starts */
extern uint8_t _estack;          /* top of RAM: where the ISR stack starts */
extern uint8_t _Min_Stack_Size;  /* its address is the size reserved for that stack */

/* _sbrk: grow malloc's heap.
 *
 * The heap grows up from `end`; the interrupt stack grows down from `_estack`.
 * Refuse to grow into the _Min_Stack_Size bytes reserved for that stack. The
 * libnosys default this replaces does no check at all, so malloc could quietly
 * overwrite the interrupt stack.
 *
 * Called with newlib's malloc lock held (src/libc/locks.c), so it needs no lock
 * of its own. This heap serves malloc/free and stdio buffers; FreeRTOS objects
 * come from the kernel's separate heap_4 pool (configTOTAL_HEAP_SIZE).
 */
void *_sbrk(ptrdiff_t increment)
{
    static uint8_t *heap_top = &end;
    const uintptr_t limit = (uintptr_t)&_estack - (uintptr_t)&_Min_Stack_Size;

    if ((uintptr_t)heap_top + increment > limit) {
        errno = ENOMEM;
        return (void *)-1;
    }

    uint8_t *previous = heap_top;
    heap_top += increment;
    return previous;
}

// printf: newlib flushes stdout (line-buffered) through _write.
int _write(int file, char *ptr, int len)
{
    (void)file;
    log_write(ptr, (size_t)len);
    return len;
}

// The rest stand in for libnosys's stubs, which warn at link time.
int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

int _close(int file)
{
    (void)file;
    errno = EBADF;
    return -1;
}

int _lseek(int file, int offset, int whence)
{
    (void)file;
    (void)offset;
    (void)whence;
    errno = ESPIPE;
    return -1;
}

int _fstat(int file, struct stat *status)
{
    (void)file;
    status->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

// strtof brings in abort(), which signals the current process. There are no
// processes here, so these only exist to link.
int _kill(int pid, int signal)
{
    (void)pid;
    (void)signal;
    errno = EINVAL;
    return -1;
}

int _getpid(void)
{
    return 1;
}
