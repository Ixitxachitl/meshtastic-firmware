#include "OSThread.h"
#include "configuration.h"
#include "memGet.h"
#include <assert.h>

namespace concurrency
{

/// Show debugging info for disabled threads
bool OSThread::showDisabled;

/// Show debugging info for threads when we run them
bool OSThread::showRun = false;

/// Show debugging info for threads we decide not to run;
bool OSThread::showWaiting = false;

const OSThread *OSThread::currentThread;

ThreadController mainController, timerController;
InterruptableDelay mainDelay;

void OSThread::setup()
{
    mainController.ThreadName = "mainController";
    timerController.ThreadName = "timerController";
}

OSThread::OSThread(const char *_name, uint32_t period, ThreadController *_controller)
    : Thread(NULL, period), controller(_controller)
{
    assertIsSetup();

    ThreadName = _name;

    if (controller) {
        bool added = controller->add(this);
        assert(added);
    }
}

OSThread::~OSThread()
{
#if defined(ARDUINO_ARCH_ESP32)
    stopFreeRTOSTask();
#endif
    if (controller)
        controller->remove(this);
}

/**
 * Wait a specified number msecs starting from the current time (rather than the last time we were run)
 */
void OSThread::setIntervalFromNow(unsigned long _interval)
{
    // Save interval
    interval = _interval;

    // Cache the next run based on the last_run
    _cached_next_run = millis() + interval;
}

bool OSThread::shouldRun(unsigned long time)
{
    bool r = Thread::shouldRun(time);

    if (showRun && r) {
        LOG_DEBUG("Thread %s: run", ThreadName.c_str());
    }

    if (showWaiting && enabled && !r) {
        LOG_DEBUG("Thread %s: wait %lu", ThreadName.c_str(), interval);
    }

    if (showDisabled && !enabled) {
        LOG_DEBUG("Thread %s: disabled", ThreadName.c_str());
    }

    return r;
}

void OSThread::run()
{
#ifdef DEBUG_HEAP
    auto heap = memGet.getFreeHeap();
#endif
    currentThread = this;
    auto newDelay = runOnce();
#ifdef DEBUG_HEAP
    auto newHeap = memGet.getFreeHeap();
    if (newHeap < heap)
        LOG_HEAP("------ Thread %s leaked heap %d -> %d (%d) ------", ThreadName.c_str(), heap, newHeap, newHeap - heap);
    if (heap < newHeap)
        LOG_HEAP("++++++ Thread %s freed heap %d -> %d (%d) ++++++", ThreadName.c_str(), heap, newHeap, newHeap - heap);
#endif
#ifdef DEBUG_LOOP_TIMING
    LOG_DEBUG("====== Thread next run in: %d", newDelay);
#endif
    runned();

    if (newDelay >= 0)
        setInterval(newDelay);

    currentThread = NULL;
}

int32_t OSThread::disable()
{
    enabled = false;
    setInterval(INT32_MAX);

    return INT32_MAX;
}

/**
 * This flag is set **only** when setup() starts, to provide a way for us to check for sloppy static constructor calls.
 * Call assertIsSetup() to force a crash if someone tries to create an instance too early.
 *
 * it is super important to never allocate those object statically.  instead, you should explicitly
 *  new them at a point where you are guaranteed that other objects that this instance
 * depends on have already been created.
 *
 * in particular, for OSThread that means "all instances must be declared via new() in setup() or later" -
 * this makes it guaranteed that the global mainController is fully constructed first.
 */
bool hasBeenSetup;

#if defined(ARDUINO_ARCH_ESP32)

/// How long stopFreeRTOSTask() waits for a clean exit before killing the task.
static const int kTaskExitTimeoutMs = 250;

void OSThread::setFreeRTOSTask(bool enable, uint32_t stackSizeBytes, UBaseType_t priority, BaseType_t coreAffinity)
{
    if (taskHandle != nullptr) {
        LOG_WARN("Cannot reconfigure FreeRTOS task while it's running");
        return;
    }
    rtosConfig.enabled = enable;
    rtosConfig.stackSizeBytes = stackSizeBytes;
    rtosConfig.priority = priority;
    rtosConfig.coreAffinity = coreAffinity;
}

bool OSThread::startFreeRTOSTask()
{
    if (!rtosConfig.enabled) {
        LOG_WARN("Thread %s: FreeRTOS task not enabled", ThreadName.c_str());
        return false;
    }
    if (taskHandle != nullptr) {
        LOG_WARN("Thread %s: FreeRTOS task already running", ThreadName.c_str());
        return false;
    }

    // Detach from the cooperative scheduler before the task exists, so runOnce()
    // never has two callers, and re-attach if the create fails - a thread that
    // cannot get a task still works, just on main-loop timing.
    ThreadController *previousController = controller;
    if (controller) {
        controller->remove(this);
        controller = nullptr;
    }

    taskShouldExit = false;
    taskRunning = true;
    BaseType_t result = xTaskCreatePinnedToCore(rtosTaskEntryPoint, ThreadName.c_str(), rtosConfig.stackSizeBytes, this,
                                                rtosConfig.priority, &taskHandle, rtosConfig.coreAffinity);
    if (result != pdPASS) {
        LOG_ERROR("Thread %s: Failed to create FreeRTOS task", ThreadName.c_str());
        taskHandle = nullptr;
        taskRunning = false;
        if (previousController && previousController->add(this))
            controller = previousController;
        return false;
    }

    LOG_INFO("Thread %s: FreeRTOS task started", ThreadName.c_str());
    return true;
}

void OSThread::wakeFreeRTOSTask()
{
    if (taskHandle != nullptr)
        xTaskNotifyGive(taskHandle);
}

void OSThread::stopFreeRTOSTask()
{
    if (taskHandle == nullptr)
        return;

    if (xTaskGetCurrentTaskHandle() == taskHandle) {
        // Called from inside our own task; it will unwind and delete itself.
        taskShouldExit = true;
        return;
    }

    // Let the task finish the runOnce() it may be in and drop whatever locks it
    // holds. vTaskDelete()ing it mid-call would leak those permanently.
    taskShouldExit = true;
    wakeFreeRTOSTask(); // don't wait out the current interval first
    for (int i = 0; taskRunning && i < kTaskExitTimeoutMs; i++)
        vTaskDelay(pdMS_TO_TICKS(1));

    if (taskRunning) {
        LOG_ERROR("Thread %s: FreeRTOS task did not exit, forcing delete", ThreadName.c_str());
        vTaskDelete(taskHandle);
        taskRunning = false;
    }
    taskHandle = nullptr;
}

void OSThread::rtosTaskEntryPoint(void *pvParameters)
{
    OSThread *instance = static_cast<OSThread *>(pvParameters);
    if (instance) {
        instance->rtosTaskLoop();
        instance->taskRunning = false; // hands control back to stopFreeRTOSTask()
    }
    vTaskDelete(nullptr);
}

void OSThread::rtosTaskLoop()
{
    // Deliberately does not touch OSThread::currentThread: that static is owned by
    // the main loop's ThreadController, and writing it from a second core both
    // races with it and mislabels the main thread's log lines.
    while (!taskShouldExit) {
        int32_t delayMs = runOnce();
        if (delayMs <= 0)
            taskYIELD();
        else
            // Sleep out the interval, but let wakeFreeRTOSTask() cut it short so
            // work handed to us from another thread starts without waiting.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delayMs));
    }
}
#endif // ARDUINO_ARCH_ESP32

void assertIsSetup()
{

    /**
     * Dear developer comrade - If this assert fails() that means you need to fix the following:
     *
     * This flag is set **only** when setup() starts, to provide a way for us to check for sloppy static constructor calls.
     * Call assertIsSetup() to force a crash if someone tries to create an instance too early.
     *
     * it is super important to never allocate those object statically.  instead, you should explicitly
     *  new them at a point where you are guaranteed that other objects that this instance
     * depends on have already been created.
     *
     * in particular, for OSThread that means "all instances must be declared via new() in setup() or later" -
     * this makes it guaranteed that the global mainController is fully constructed first.
     */
    assert(hasBeenSetup);
}

} // namespace concurrency
