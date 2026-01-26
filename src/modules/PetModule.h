#pragma once

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_PET

#include "MeshModule.h"
#include "Observer.h"
#include "concurrency/OSThread.h"

#if HAS_SCREEN
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
#endif

/**
 * PetModule - An animated virtual pet screen module for Meshtastic
 *
 * This module displays an animated pet character with statistics based on
 * mesh network activity. The pet's mood and behavior can reflect:
 * - Number of nodes seen
 * - Messages received
 * - Network health
 * - Device uptime
 *
 * Images can be customized by editing PetImages.h
 */

// Pet mood states based on activity
enum class PetMood {
    HAPPY,   // Good network activity
    NEUTRAL, // Normal state
    SAD,     // Low activity
    EXCITED, // High activity / new nodes
    SLEEPING // Idle / power saving
};

// Animation states
enum class PetAnimation { IDLE, WALKING, HAPPY_BOUNCE, SLEEPING, EXCITED };

class PetModule : public MeshModule, public Observable<const UIFrameEvent *>, private concurrency::OSThread
{
  public:
    PetModule();

    // Get the current pet statistics
    uint32_t getMessagesReceived() const { return messagesReceived; }
    uint32_t getNodesDiscovered() const { return nodesDiscovered; }
    uint32_t getUptimeMinutes() const;
    PetMood getCurrentMood() const { return currentMood; }

#if HAS_SCREEN
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
    virtual bool wantUIFrame() override { return enabled; }
#endif

    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }

    // Enable/disable the pet screen
    void setEnabled(bool enable);
    bool isEnabled() const { return enabled; }

  protected:
    virtual int32_t runOnce() override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override { return true; } // We want to track all packets

  private:
    // Configuration
    bool enabled = true;

    // Statistics tracking
    uint32_t messagesReceived = 0;
    uint32_t nodesDiscovered = 0;
    uint32_t lastActivityTime = 0;
    uint32_t bootTime = 0;

    // Animation state
    PetMood currentMood = PetMood::NEUTRAL;
    PetAnimation currentAnimation = PetAnimation::IDLE;
    uint8_t animationFrame = 0;
    uint32_t lastFrameTime = 0;
    int16_t petX = 0;        // Pet position for walking animation
    int8_t petDirection = 1; // 1 = right, -1 = left

    // Animation timing (milliseconds)
    static constexpr uint32_t FRAME_DURATION_MS = 300;
    static constexpr uint32_t MOOD_UPDATE_INTERVAL_MS = 30000; // 30 seconds
    static constexpr uint32_t IDLE_THRESHOLD_MS = 300000;      // 5 minutes until sleeping

    // Internal methods
    void updateMood();
    void updateAnimation();
    void advanceFrame();
    uint8_t getFrameCount() const;

#if HAS_SCREEN
    void drawPet(OLEDDisplay *display, int16_t x, int16_t y);
    void drawXbmFlipped(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *xbm);
    void drawStats(OLEDDisplay *display, int16_t x, int16_t y);
    void drawMoodIndicator(OLEDDisplay *display, int16_t x, int16_t y);
#endif
};

extern PetModule *petModule;

#endif // !MESHTASTIC_EXCLUDE_PET
