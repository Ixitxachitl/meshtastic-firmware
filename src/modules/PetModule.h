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
    HAPPY,    // Good network activity
    NEUTRAL,  // Normal state
    SAD,      // Low activity
    EXCITED,  // High activity / new nodes
    SLEEPING, // Idle / power saving
    HUNGRY,   // Needs attention (low battery?)
    ALERT     // Important event (incoming message)
};

// Animation states
enum class PetAnimation {
    IDLE,
    WALKING,
    HAPPY_BOUNCE,
    SLEEPING,
    EXCITED,
    EATING,   // When receiving data
    PLAYING,  // Random playful animation
    ALERT,    // When important message arrives
    SAD,      // When lonely/low activity
    READING,  // For text messages
    LOOKING,  // For position/navigation
    WAVING,   // For meeting nodes
    THINKING, // For telemetry data
    SCARED,   // For alerts/admin messages
    // Additional variety animations
    HOPPING,    // Happy hopping/skipping
    SCRATCHING, // Idle scratching
    DANCING,    // Happy dancing
    YAWNING,    // Bored/neutral yawning
    SNIFFING    // Curious sniffing around
};

// Last received message types
enum class LastMessageType {
    NONE,
    UNKNOWN,
    TEXT,
    REMOTE_HW,
    POSITION,
    NODEINFO,
    ROUTING,
    ADMIN,
    TEXT_COMPRESSED,
    WAYPOINT,
    AUDIO,
    DETECTION,
    ALERT,
    KEY_VERIFY,
    REPLY,
    IP_TUNNEL,
    PAXCOUNT,
    STORE_FWD_PP,
    NODE_STATUS,
    SERIAL_PORT,
    STORE_FWD,
    RANGE_TEST,
    TELEMETRY,
    ZPS,
    SIMULATOR,
    TRACEROUTE,
    NEIGHBOR,
    ATAK,
    MAP_REPORT,
    POWERSTRESS,
    RETICULUM,
    CAYENNE,
    PRIVATE,
    ATAK_FWD
};

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

#if defined(SENSECAP_INDICATOR) || defined(T_DECK)
    // Message log ring buffer (wide screen devices - shows received messages)
#ifdef SENSECAP_INDICATOR
    static constexpr uint8_t MSG_LOG_LINES = 18;
#else
    static constexpr uint8_t MSG_LOG_LINES = 6;
#endif
    static constexpr uint8_t MSG_LOG_LINE_LEN = 80;
    char msgLogBuffer[MSG_LOG_LINES][MSG_LOG_LINE_LEN] = {{0}};
    uint8_t msgLogHead = 0;
    uint8_t msgLogCount = 0;
    void addMessageLog(const char *line);
#endif

    // Statistics tracking
    uint32_t messagesReceived = 0;
    uint32_t nodesDiscovered = 0;
    uint32_t lastActivityTime = 0;
    uint32_t bootTime = 0;
    LastMessageType lastMessageType = LastMessageType::NONE;
    uint8_t lastMessageHops = 0;  // Hops the last message traveled
    uint32_t lastMessageTime = 0; // When last message was received (millis)

    // Rolling message history for traffic rate (6 ten-second buckets = 1 minute)
    static constexpr uint8_t MESSAGE_HISTORY_BUCKETS = 6;
    static constexpr uint32_t BUCKET_DURATION_MS = 10000; // 10 seconds per bucket
    uint16_t messageHistory[MESSAGE_HISTORY_BUCKETS] = {0};
    uint8_t historyBucketIndex = 0;
    uint32_t lastBucketRotation = 0;

    // Animation state
    PetMood currentMood = PetMood::NEUTRAL;
    PetAnimation currentAnimation = PetAnimation::IDLE;
    uint8_t animationFrame = 0;
    uint32_t lastFrameTime = 0;
    int16_t petX = 0;        // Pet position for walking animation
    int8_t petDirection = 1; // 1 = right, -1 = left

    // Happiness/health tracking (0-100)
    uint8_t happiness = 75;
    uint8_t energy = 100; // Now reflects battery percent

    // XP and leveling system
    uint32_t experience = 0;       // Total XP earned
    uint16_t level = 1;            // Current level
    uint32_t xpForNextLevel = 100; // XP needed for next level

    // Animation timing (milliseconds)
    static constexpr uint32_t FRAME_DURATION_MS = 300;
    static constexpr uint32_t MOOD_UPDATE_INTERVAL_MS = 30000; // 30 seconds
    static constexpr uint32_t IDLE_THRESHOLD_MS = 300000;      // 5 minutes until sleeping

    // Internal methods
    void updateMood();
    void updateAnimation();
    void advanceFrame();
    uint8_t getFrameCount() const;
    void updateHappiness();
    uint8_t calculateHealth() const;
    uint16_t getMessagesPerMinute() const;
    const uint8_t *getSpeedIcon(uint16_t messagesPerMinute) const;

#if HAS_SCREEN
    void drawPet(OLEDDisplay *display, int16_t x, int16_t y, uint8_t scale = 1);
    void drawXbmScaled(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *xbm,
                       uint8_t scale);
    void drawXbmFlipped(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *xbm,
                        uint8_t scale = 1);
    void drawStats(OLEDDisplay *display, int16_t x, int16_t y, int16_t width);
    void drawMoodIndicator(OLEDDisplay *display, int16_t x, int16_t y, uint8_t scale = 1);
    void drawStatusBarWithIcon(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, uint8_t percent, bool isHeart,
                               uint8_t scale = 1);
    void drawPetArea(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, uint8_t scale = 1);
    const char *getMessageTypeName(LastMessageType type);
#endif
};

extern PetModule *petModule;

#endif // !MESHTASTIC_EXCLUDE_PET
