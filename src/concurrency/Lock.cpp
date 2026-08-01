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

void Lock::unlock()
{
    if (xSemaphoreGive(handle) == false) {
        abort();
    }
}
#else
Lock::Lock() {}

Lock::~Lock() {}

void Lock::lock() {}

void Lock::unlock() {}
#endif

} // namespace concurrency
