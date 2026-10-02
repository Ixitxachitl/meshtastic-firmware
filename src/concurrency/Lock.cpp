#include "Lock.h"
#include "configuration.h"
#include <cassert>

namespace concurrency
{

#ifdef HAS_FREE_RTOS
// A mutex rather than a binary semaphore, for priority inheritance: without it a
// low priority task holding the lock can be preempted indefinitely while a high
// priority task waits on it. Still non-recursive, so a thread that may re-enter
// needs to track that itself (see ReentrantSpiLock in graphics/tftSetup.cpp).
Lock::Lock() : handle(xSemaphoreCreateMutex())
{
    assert(handle);
}

Lock::~Lock()
{
    vSemaphoreDelete(handle);
}

void Lock::lock()
{
    if (xSemaphoreTake(handle, portMAX_DELAY) == false) {
        abort();
    }
}

bool Lock::lock(uint32_t timeout)
{
    return xSemaphoreTake(handle, pdMS_TO_TICKS(timeout)) == pdTRUE;
}

void Lock::unlock()
{
    if (xSemaphoreGive(handle) == false) {
        abort();
    }
}
#elif defined(ARCH_PORTDUINO)
Lock::Lock()
{
    pthread_mutex_init(&mutex, NULL);
}

void Lock::lock()
{
    pthread_mutex_lock(&mutex);
}

bool Lock::lock(uint32_t)
{
    // No portable timed pthread lock across Linux and macOS, so block instead: returning true
    // without acquiring would leave callers such as SPILock unlocking a mutex they never took.
    lock();
    return true;
}

void Lock::unlock()
{
    pthread_mutex_unlock(&mutex);
}

Lock::~Lock()
{
    pthread_mutex_destroy(&mutex);
}
#else
// Neither FreeRTOS nor pthreads: single-threaded targets such as STM32WL, whose newlib has no
// pthread at all. Unchanged from upstream - the real implementation above is Portduino's.
Lock::Lock() {}

Lock::~Lock() {}

void Lock::lock() {}

bool Lock::lock(uint32_t)
{
    return true;
}

void Lock::unlock() {}
#endif

} // namespace concurrency
