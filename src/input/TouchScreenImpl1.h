#pragma once
// configuration.h is what pulls in the variant - include it before the gate below rather than
// relying on a transitive include, or BASEUI_HAS_TOUCH_DRAG silently resolves to 0 on a target
// whose variant.h sets it.
#include "configuration.h"

#include "TouchScreenBase.h"

// Opt-in per variant: continuous touch drag reporting. With this off, a touchscreen gesture is only
// reported once the finger lifts, collapsed to one of four directions with no magnitude (see
// TouchScreenBase::runOnce). With it on, the touch layer additionally emits INPUT_BROKER_TOUCH_DRAG
// while the finger is held down and moving, so a consumer can follow the finger.
//
// Off by default: it needs a display that can redraw fast enough to track a finger, and consumers
// that know what to do with the drag stream. Enable it on a board once both are true.
#ifndef BASEUI_HAS_TOUCH_DRAG
#define BASEUI_HAS_TOUCH_DRAG 0
#endif

class TouchScreenImpl1 : public TouchScreenBase
{
  public:
    TouchScreenImpl1(uint16_t width, uint16_t height, bool (*getTouch)(int16_t *, int16_t *));
    void init(void);

  protected:
    virtual bool getTouch(int16_t &x, int16_t &y);
    virtual void onEvent(const TouchEvent &event);
    bool fastTapModeEnabled() const override;
    bool longPressEnabled() const override;
    bool dragEventsEnabled() const override;

    // Attach/detach a hardware interrupt on the touch IRQ pin (SCREEN_TOUCH_INT) so a new touch
    // wakes the polling thread immediately. No-op on boards without a usable touch interrupt line.
    void attachTouchInterrupt();

    bool (*_getTouch)(int16_t *, int16_t *);

#ifdef ARCH_ESP32
    // Detach the touch interrupt before light sleep (so sleep.cpp can own the wake config),
    // and reattach it afterwards. Mirrors ButtonThread's interrupt handling.
    int beforeLightSleep(void *unused);
    int afterLightSleep(esp_sleep_wakeup_cause_t cause);

    CallbackObserver<TouchScreenImpl1, void *> lsObserver =
        CallbackObserver<TouchScreenImpl1, void *>(this, &TouchScreenImpl1::beforeLightSleep);
    CallbackObserver<TouchScreenImpl1, esp_sleep_wakeup_cause_t> lsEndObserver =
        CallbackObserver<TouchScreenImpl1, esp_sleep_wakeup_cause_t>(this, &TouchScreenImpl1::afterLightSleep);
#endif
};

extern TouchScreenImpl1 *touchScreenImpl1;
