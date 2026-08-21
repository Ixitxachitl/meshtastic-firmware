#pragma once

#include <cstdlib>
#include <stdint.h>

#include "../freertosinc.h"
#include "Thread.h"
#include "ThreadController.h"
#include "concurrency/InterruptableDelay.h"

namespace concurrency
{

extern ThreadController mainController, timerController;
extern InterruptableDelay mainDelay;

#define RUN_SAME -1

#if defined(ARDUINO_ARCH_ESP32)
struct FreeRTOSTaskConfig {
    /// Stack size in BYTES. ESP-IDF's xTaskCreate() differs from vanilla FreeRTOS
    /// here, where the same argument counts StackType_t words.
    uint32_t stackSizeBytes = 4096;
    UBaseType_t priority = tskIDLE_PRIORITY + 1;
    BaseType_t coreAffinity = tskNO_AFFINITY;
    bool enabled = false;
};
#endif

/**
 * @brief Base threading
 *
 * This is a pseudo threading layer that is super easy to port, well suited to our slow network and very ram & power efficient.
 *
 * TODO FIXME @geeksville
 *
 * move more things into OSThreads
 * remove lock/lockguard
 *
 * move typedQueue into concurrency
 * remove freertos from typedqueue
 */
class OSThread : public Thread
{
    ThreadController *controller;

    /// Show debugging info for disabled threads
    static bool showDisabled;

    /// Show debugging info for threads when we run them
    static bool showRun;

    /// Show debugging info for threads we decide not to run;
    static bool showWaiting;

#if defined(ARDUINO_ARCH_ESP32)
    FreeRTOSTaskConfig rtosConfig;
    TaskHandle_t taskHandle = nullptr;
    /// Written by stopFreeRTOSTask(), read by the task; cleared by the task on exit.
    volatile bool taskShouldExit = false;
    volatile bool taskRunning = false;
    static void rtosTaskEntryPoint(void *pvParameters);
    void rtosTaskLoop();
#endif

  public:
    /// For debug printing only (might be null)
    static const OSThread *currentThread;

    OSThread(const char *name, uint32_t period = 0, ThreadController *controller = &mainController);

    virtual ~OSThread();

    virtual bool shouldRun(unsigned long time);

    static void setup();

    virtual int32_t disable();

    /**
     * Wait a specified number msecs starting from the current time (rather than the last time we were run)
     */
    void setIntervalFromNow(unsigned long _interval);

#if defined(ARDUINO_ARCH_ESP32)
    /**
     * Ask for runOnce() to be driven by a dedicated FreeRTOS task instead of the
     * cooperative ThreadController. Call before startFreeRTOSTask(); stackSizeBytes
     * is in bytes, per ESP-IDF's xTaskCreate().
     */
    void setFreeRTOSTask(bool enable = true, uint32_t stackSizeBytes = 4096, UBaseType_t priority = tskIDLE_PRIORITY + 1,
                         BaseType_t coreAffinity = tskNO_AFFINITY);
    /// Returns false and leaves the thread on the ThreadController if the task cannot be created.
    bool startFreeRTOSTask();
    /// Cut short the task's current sleep so runOnce() runs now. Safe to call from any thread.
    void wakeFreeRTOSTask();
    /// Asks the task to exit and waits for it, so it is never killed mid-runOnce().
    void stopFreeRTOSTask();
    bool isFreeRTOSTask() const { return rtosConfig.enabled; }
#endif

  protected:
    /**
     * The method that will be called each time our thread gets a chance to run
     *
     * Returns desired period for next invocation (or RUN_SAME for no change)
     */
    virtual int32_t runOnce() = 0;
    bool sleepOnNextExecution = false;

    // Do not override this
    virtual void run();
};

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
extern bool hasBeenSetup;

void assertIsSetup();

} // namespace concurrency