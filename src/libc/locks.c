/* Thread-safe C library: newlib's internal locks, backed by FreeRTOS mutexes.
 *
 * This is the bare-metal equivalent of compiling with -pthread. It has two halves:
 *
 *  1. Per-task state. configUSE_NEWLIB_REENTRANT (FreeRTOSConfig.h) gives each
 *     task its own `struct _reent`, and the kernel swaps newlib's `_impure_ptr`
 *     to it on every context switch. That makes errno, strtok(), rand() and
 *     friends private to each task.
 *
 *  2. Shared state. Some things really are shared by all tasks: malloc's heap,
 *     the list of open stdio streams, the environment. newlib protects them by
 *     calling the __retarget_lock_* hooks below, which this file implements with
 *     FreeRTOS recursive mutexes -- so priority inheritance applies.
 *
 * Rules that follow:
 *  - Never call a locking C-library function (malloc, free, printf, snprintf
 *    with %f, ...) from an interrupt. An ISR cannot wait for a mutex, so this
 *    file asserts if one tries.
 *  - Before the scheduler starts there is only one thread, so locks do nothing.
 *
 * Background: http://www.nadler.com/embedded/newlibAndFreeRTOS.html
 */
#include "libc/locks.h"

#include <stddef.h>
#include <sys/lock.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

struct __lock {
    SemaphoreHandle_t mutex;
};

/* The fixed locks newlib's own code refers to by name. */
struct __lock __lock___sinit_recursive_mutex;
struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

static struct __lock *const s_static_locks[] = {
    &__lock___sinit_recursive_mutex,  &__lock___sfp_recursive_mutex,
    &__lock___atexit_recursive_mutex, &__lock___at_quick_exit_mutex,
    &__lock___malloc_recursive_mutex, &__lock___env_recursive_mutex,
    &__lock___tz_mutex,               &__lock___dd_hash_mutex,
    &__lock___arc4random_mutex,
};

void libc_locks_init(void)
{
    for (size_t i = 0; i < sizeof s_static_locks / sizeof s_static_locks[0]; i++) {
        s_static_locks[i]->mutex = xSemaphoreCreateRecursiveMutex();
        configASSERT(s_static_locks[i]->mutex != NULL);
    }
}

/* ---- Locks newlib creates at runtime (e.g. one per stdio stream) --------- */

void __retarget_lock_init_recursive(_LOCK_T *lock)
{
    struct __lock *l = pvPortMalloc(sizeof *l);
    configASSERT(l != NULL);
    l->mutex = xSemaphoreCreateRecursiveMutex();
    configASSERT(l->mutex != NULL);
    *lock = l;
}

void __retarget_lock_close_recursive(_LOCK_T lock)
{
    if (lock != NULL) {
        vSemaphoreDelete(lock->mutex);
        vPortFree(lock);
    }
}

/* ---- Acquire / release ---------------------------------------------------- */

void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return;
    }
    configASSERT(!xPortIsInsideInterrupt());
    configASSERT(lock != NULL && lock->mutex != NULL);
    (void)xSemaphoreTakeRecursive(lock->mutex, portMAX_DELAY);
}

int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return 0;
    }
    configASSERT(!xPortIsInsideInterrupt());
    configASSERT(lock != NULL && lock->mutex != NULL);
    return (xSemaphoreTakeRecursive(lock->mutex, 0) == pdTRUE) ? 0 : -1;
}

void __retarget_lock_release_recursive(_LOCK_T lock)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return;
    }
    configASSERT(lock != NULL && lock->mutex != NULL);
    (void)xSemaphoreGiveRecursive(lock->mutex);
}

/* ---- Non-recursive variants -----------------------------------------------
 * A recursive mutex satisfies every requirement of a non-recursive lock, so
 * these simply forward. */

void __retarget_lock_init(_LOCK_T *lock)          { __retarget_lock_init_recursive(lock); }
void __retarget_lock_close(_LOCK_T lock)          { __retarget_lock_close_recursive(lock); }
void __retarget_lock_acquire(_LOCK_T lock)        { __retarget_lock_acquire_recursive(lock); }
int  __retarget_lock_try_acquire(_LOCK_T lock)    { return __retarget_lock_try_acquire_recursive(lock); }
void __retarget_lock_release(_LOCK_T lock)        { __retarget_lock_release_recursive(lock); }
