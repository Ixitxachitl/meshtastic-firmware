#pragma once
// configuration.h is what defines HAS_TOUCHSCREEN / HAS_TFT / USE_EINK - include it before the
// gate below rather than relying on a transitive include, or BASEUI_HAS_TOUCH_DRAG silently
// resolves to 0 on targets that should have it.
#include "configuration.h"

#include "TouchScreenBase.h"

// Opt-in: continuous touch drag reporting. With this off, a touchscreen gesture is only reported
// once the finger lifts, collapsed to one of four directions with no magnitude (see
// TouchScreenBase::runOnce). With it on, the touch layer additionally emits INPUT_BROKER_TOUCH_DRAG
// while the finger is held down and moving, so a consumer can follow the finger instead of
// receiving a single step at the end.
//
// Gated on a display that can actually keep up: a color TFT redrawing at speed. E-Ink can't track a
// finger at all, and monochrome OLED targets have no use for it, so both keep the release-only
// swipe behaviour unchanged. ARCH_PORTDUINO is included so the native/SDL build - where a mouse
// drag arrives as a touch drag - can exercise this path without hardware.
//
// Lives here rather than in configuration.h because the touch input layer is its only consumer;
// override per build/variant with -DBASEUI_HAS_TOUCH_DRAG=0/1.
#ifndef BASEUI_HAS_TOUCH_DRAG
#if (HAS_TOUCHSCREEN || defined(ARCH_PORTDUINO)) && (HAS_TFT || defined(HAS_SPI_TFT)) && !defined(USE_EINK)
#define BASEUI_HAS_TOUCH_DRAG 1
#else
#define BASEUI_HAS_TOUCH_DRAG 0
#endif
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
