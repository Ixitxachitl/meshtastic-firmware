#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_PET

#include "NodeDB.h"
#include "NodeStatus.h"
#include "PetImages.h"
#include "PetModule.h"
#include "PowerStatus.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/images.h"
#include "main.h"

#if HAS_SCREEN
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#ifndef EXCLUDE_EMOJI
#include "graphics/emotes.h"
#endif
#endif

PetModule *petModule = nullptr;

PetModule::PetModule() : MeshModule("pet"), concurrency::OSThread("PetModule")
{
    bootTime = millis();
    lastActivityTime = bootTime;

    // Start with neutral mood
    currentMood = PetMood::NEUTRAL;
    currentAnimation = PetAnimation::IDLE;

    // Initialize pet position (center of screen area)
    petX = 32;

    // This module is promiscuous - it sees all packets to track activity
    isPromiscuous = true;
}

void PetModule::setEnabled(bool enable)
{
    if (enabled != enable) {
        enabled = enable;
#if HAS_SCREEN
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
#endif
    }
}

int32_t PetModule::runOnce()
{
    if (!enabled)
        return 5000; // Check again in 5 seconds

    uint32_t now = millis();

    // Update animation frame
    if (now - lastFrameTime >= FRAME_DURATION_MS) {
        advanceFrame();
        lastFrameTime = now;

#if HAS_SCREEN
        // Request screen redraw
        if (screen) {
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REDRAW_ONLY;
            notifyObservers(&e);
        }
#endif
    }

    // Periodically update mood based on activity
    static uint32_t lastMoodUpdate = 0;
    if (now - lastMoodUpdate >= MOOD_UPDATE_INTERVAL_MS) {
        updateMood();
        lastMoodUpdate = now;
    }

    return 100; // Run frequently for smooth animation
}

ProcessMessage PetModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Track message activity
    messagesReceived++;
    lastActivityTime = millis();
    lastMessageTime = lastActivityTime;

    // Calculate hops traveled (hop_start - hop_limit gives hops used)
    if (mp.hop_start > 0 && mp.hop_limit <= mp.hop_start) {
        lastMessageHops = mp.hop_start - mp.hop_limit;
    } else {
        lastMessageHops = 0; // Direct or unknown
    }

    // Track what type of message was received
    switch (mp.decoded.portnum) {
    case meshtastic_PortNum_UNKNOWN_APP:
        lastMessageType = LastMessageType::UNKNOWN;
        break;
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        lastMessageType = LastMessageType::TEXT;
        break;
    case meshtastic_PortNum_REMOTE_HARDWARE_APP:
        lastMessageType = LastMessageType::REMOTE_HW;
        break;
    case meshtastic_PortNum_POSITION_APP:
        lastMessageType = LastMessageType::POSITION;
        break;
    case meshtastic_PortNum_NODEINFO_APP:
        lastMessageType = LastMessageType::NODEINFO;
        break;
    case meshtastic_PortNum_ROUTING_APP:
        lastMessageType = LastMessageType::ROUTING;
        break;
    case meshtastic_PortNum_ADMIN_APP:
        lastMessageType = LastMessageType::ADMIN;
        break;
    case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP:
        lastMessageType = LastMessageType::TEXT_COMPRESSED;
        break;
    case meshtastic_PortNum_WAYPOINT_APP:
        lastMessageType = LastMessageType::WAYPOINT;
        break;
    case meshtastic_PortNum_AUDIO_APP:
        lastMessageType = LastMessageType::AUDIO;
        break;
    case meshtastic_PortNum_DETECTION_SENSOR_APP:
        lastMessageType = LastMessageType::DETECTION;
        break;
    case meshtastic_PortNum_ALERT_APP:
        lastMessageType = LastMessageType::ALERT;
        break;
    case meshtastic_PortNum_KEY_VERIFICATION_APP:
        lastMessageType = LastMessageType::KEY_VERIFY;
        break;
    case meshtastic_PortNum_REPLY_APP:
        lastMessageType = LastMessageType::REPLY;
        break;
    case meshtastic_PortNum_IP_TUNNEL_APP:
        lastMessageType = LastMessageType::IP_TUNNEL;
        break;
    case meshtastic_PortNum_PAXCOUNTER_APP:
        lastMessageType = LastMessageType::PAXCOUNT;
        break;
    case meshtastic_PortNum_STORE_FORWARD_PLUSPLUS_APP:
        lastMessageType = LastMessageType::STORE_FWD_PP;
        break;
    case meshtastic_PortNum_NODE_STATUS_APP:
        lastMessageType = LastMessageType::NODE_STATUS;
        break;
    case meshtastic_PortNum_SERIAL_APP:
        lastMessageType = LastMessageType::SERIAL_PORT;
        break;
    case meshtastic_PortNum_STORE_FORWARD_APP:
        lastMessageType = LastMessageType::STORE_FWD;
        break;
    case meshtastic_PortNum_RANGE_TEST_APP:
        lastMessageType = LastMessageType::RANGE_TEST;
        break;
    case meshtastic_PortNum_TELEMETRY_APP:
        lastMessageType = LastMessageType::TELEMETRY;
        break;
    case meshtastic_PortNum_ZPS_APP:
        lastMessageType = LastMessageType::ZPS;
        break;
    case meshtastic_PortNum_SIMULATOR_APP:
        lastMessageType = LastMessageType::SIMULATOR;
        break;
    case meshtastic_PortNum_TRACEROUTE_APP:
        lastMessageType = LastMessageType::TRACEROUTE;
        break;
    case meshtastic_PortNum_NEIGHBORINFO_APP:
        lastMessageType = LastMessageType::NEIGHBOR;
        break;
    case meshtastic_PortNum_ATAK_PLUGIN:
        lastMessageType = LastMessageType::ATAK;
        break;
    case meshtastic_PortNum_MAP_REPORT_APP:
        lastMessageType = LastMessageType::MAP_REPORT;
        break;
    case meshtastic_PortNum_POWERSTRESS_APP:
        lastMessageType = LastMessageType::POWERSTRESS;
        break;
    case meshtastic_PortNum_RETICULUM_TUNNEL_APP:
        lastMessageType = LastMessageType::RETICULUM;
        break;
    case meshtastic_PortNum_CAYENNE_APP:
        lastMessageType = LastMessageType::CAYENNE;
        break;
    case meshtastic_PortNum_PRIVATE_APP:
        lastMessageType = LastMessageType::PRIVATE;
        break;
    case meshtastic_PortNum_ATAK_FORWARDER:
        lastMessageType = LastMessageType::ATAK_FWD;
        break;
    default:
        lastMessageType = LastMessageType::UNKNOWN;
        break;
    }

    // Boost happiness when receiving messages (pet likes activity!)
    if (happiness < 95) {
        happiness += 5;
    }

    // Gain XP for receiving messages
    experience += 5;

    // Wake up from sleep if sleeping (but only if battery is OK)
    if (currentMood == PetMood::SLEEPING && energy >= 30) {
        // Wake up animation
        currentMood = PetMood::HAPPY;
        currentAnimation = PetAnimation::HAPPY_BOUNCE;
    }

    // Check if this is from a "new" node we haven't counted
    if (nodeDB) {
        uint32_t currentNodes = nodeDB->getNumMeshNodes();
        if (currentNodes > nodesDiscovered) {
            nodesDiscovered = currentNodes;
            // New node discovered - get excited!
            currentMood = PetMood::EXCITED;
            currentAnimation = PetAnimation::EXCITED;
            happiness = 100;  // Max happiness for new discovery!
            experience += 50; // Bonus XP for new node!
        }
    }

    // Check for level up
    while (experience >= xpForNextLevel) {
        experience -= xpForNextLevel;
        level++;
        xpForNextLevel = 100 * level; // Each level requires more XP
        // Level up excitement!
        currentMood = PetMood::EXCITED;
        currentAnimation = PetAnimation::EXCITED;
    }

    // Set animation based on message type (if not already excited from level up or new node)
    if (currentMood != PetMood::EXCITED) {
        switch (lastMessageType) {
        case LastMessageType::TEXT:
        case LastMessageType::TEXT_COMPRESSED:
            // Text message - reading!
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::READING;
            break;
        case LastMessageType::POSITION:
        case LastMessageType::WAYPOINT:
        case LastMessageType::TRACEROUTE:
        case LastMessageType::ROUTING:
            // Movement/navigation - looking around
            currentMood = PetMood::NEUTRAL;
            currentAnimation = PetAnimation::LOOKING;
            break;
        case LastMessageType::NODEINFO:
        case LastMessageType::NEIGHBOR:
            // Node info - wave hello!
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::WAVING;
            break;
        case LastMessageType::TELEMETRY:
            // Telemetry data - thinking/processing
            currentMood = PetMood::NEUTRAL;
            currentAnimation = PetAnimation::THINKING;
            break;
        case LastMessageType::ADMIN:
        case LastMessageType::ALERT:
        case LastMessageType::DETECTION:
            // Important/alert messages - scared!
            currentMood = PetMood::ALERT;
            currentAnimation = PetAnimation::SCARED;
            break;
        case LastMessageType::RANGE_TEST:
            // Range test - running around excited
            currentMood = PetMood::EXCITED;
            currentAnimation = PetAnimation::EXCITED;
            break;
        case LastMessageType::STORE_FWD:
        case LastMessageType::STORE_FWD_PP:
            // Store & forward - eating stored food
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::EATING;
            break;
        case LastMessageType::ATAK:
        case LastMessageType::ATAK_FWD:
        case LastMessageType::MAP_REPORT:
            // Tactical/map - looking around carefully
            currentMood = PetMood::ALERT;
            currentAnimation = PetAnimation::LOOKING;
            break;
        default:
            // Other messages - just alert briefly
            currentMood = PetMood::ALERT;
            currentAnimation = PetAnimation::ALERT;
            break;
        }
    }

    return ProcessMessage::CONTINUE; // Let other modules also process
}

uint32_t PetModule::getUptimeMinutes() const
{
    return (millis() - bootTime) / 60000;
}

void PetModule::updateHappiness()
{
    uint32_t now = millis();
    uint32_t timeSinceActivity = now - lastActivityTime;

    // Energy is battery percent
    if (powerStatus) {
        energy = powerStatus->getBatteryChargePercent();
        if (energy > 100)
            energy = 100;
    }

    // Don't decay happiness while sleeping
    if (currentMood == PetMood::SLEEPING) {
        return;
    }

    // Happiness decays slowly over time without activity
    if (timeSinceActivity > 60000 && happiness > 10) { // 1 minute
        happiness--;
    }
}

void PetModule::updateMood()
{
    uint32_t now = millis();
    uint32_t timeSinceActivity = now - lastActivityTime;

    // Update happiness/energy first
    updateHappiness();

    // Random chance to play (5% each update when happy)
    bool wantsToPlay = (happiness > 60) && (rand() % 20 == 0);

    // Determine mood based on recent activity and stats
    // Low battery (under 30%) - pet takes a nap temporarily
    if (energy < 30 && energy > 0) {
        currentMood = PetMood::SLEEPING;
        currentAnimation = PetAnimation::SLEEPING;
    } else if (energy < 50) {
        // Low-ish battery - pet is hungry (wants charging)
        currentMood = PetMood::HUNGRY;
        currentAnimation = PetAnimation::SAD;
    } else if (timeSinceActivity > IDLE_THRESHOLD_MS) {
        // Been idle for a while - pet goes to sleep
        currentMood = PetMood::SLEEPING;
        currentAnimation = PetAnimation::SLEEPING;
    } else if (happiness < 30) {
        // Unhappy - needs attention, use SAD animation
        currentMood = PetMood::SAD;
        currentAnimation = PetAnimation::SAD;
    } else if (timeSinceActivity < 2000) {
        // Very recent activity - alert/attentive
        currentMood = PetMood::ALERT;
        currentAnimation = PetAnimation::ALERT;
    } else if (timeSinceActivity > IDLE_THRESHOLD_MS / 2) {
        // Getting bored
        currentMood = PetMood::NEUTRAL;
        currentAnimation = PetAnimation::IDLE;
    } else if (wantsToPlay && currentAnimation != PetAnimation::PLAYING) {
        // Random play - pick a happy animation
        currentMood = PetMood::HAPPY;
        uint8_t playChoice = rand() % 3;
        if (playChoice == 0) {
            currentAnimation = PetAnimation::PLAYING;
        } else if (playChoice == 1) {
            currentAnimation = PetAnimation::DANCING;
        } else {
            currentAnimation = PetAnimation::HOPPING;
        }
    } else if (messagesReceived > 0 && (now - lastActivityTime) < 10000) {
        // Recently received a message - eating animation
        currentMood = PetMood::HAPPY;
        currentAnimation = PetAnimation::EATING;
    } else if (messagesReceived > 0 && (messagesReceived % 10) < 3) {
        // Normal activity - walking or hopping around
        currentMood = PetMood::NEUTRAL;
        currentAnimation = (rand() % 3 == 0) ? PetAnimation::HOPPING : PetAnimation::WALKING;
    } else if (timeSinceActivity > 30000 && (rand() % 10 == 0)) {
        // Getting a bit bored - might yawn or scratch
        currentMood = PetMood::NEUTRAL;
        currentAnimation = (rand() % 2 == 0) ? PetAnimation::YAWNING : PetAnimation::SCRATCHING;
    } else if (happiness > 70 && (rand() % 8 == 0)) {
        // Happy and curious - sniff around
        currentMood = PetMood::HAPPY;
        currentAnimation = PetAnimation::SNIFFING;
    } else {
        // Good activity - variety of idle animations
        currentMood = PetMood::HAPPY;
        uint8_t idleChoice = rand() % 4;
        if (idleChoice == 0) {
            currentAnimation = PetAnimation::HAPPY_BOUNCE;
        } else {
            currentAnimation = PetAnimation::IDLE;
        }
    }
}

void PetModule::updateAnimation()
{
    // Handle walking animation - move pet back and forth within pet box
    // petX ranges from 0 to 20, which gets scaled to actual walk range in drawFrame
    const int16_t maxWalkX = 20;

    if (currentAnimation == PetAnimation::WALKING || currentAnimation == PetAnimation::HOPPING) {
        petX += petDirection * 2; // Move 2 units per frame for visible movement
        // Bounce within the range
        if (petX >= maxWalkX) {
            petX = maxWalkX;
            petDirection = -1;
        } else if (petX <= 0) {
            petX = 0;
            petDirection = 1;
        }
    } else {
        // Center position when not walking
        petX = maxWalkX / 2;
    }
}

void PetModule::advanceFrame()
{
    animationFrame++;
    if (animationFrame >= getFrameCount()) {
        animationFrame = 0;
    }
    updateAnimation();
}

uint8_t PetModule::getFrameCount() const
{
    // Return frame count based on current animation
    switch (currentAnimation) {
    case PetAnimation::IDLE:
        return PET_IDLE_FRAMES;
    case PetAnimation::WALKING:
        return PET_WALK_FRAMES;
    case PetAnimation::HAPPY_BOUNCE:
        return PET_HAPPY_FRAMES;
    case PetAnimation::SLEEPING:
        return PET_SLEEP_FRAMES;
    case PetAnimation::EXCITED:
        return PET_EXCITED_FRAMES;
    case PetAnimation::EATING:
        return PET_EATING_FRAMES;
    case PetAnimation::PLAYING:
        return PET_PLAYING_FRAMES;
    case PetAnimation::ALERT:
        return PET_ALERT_FRAMES;
    case PetAnimation::SAD:
        return PET_SAD_FRAMES;
    case PetAnimation::READING:
        return PET_READING_FRAMES;
    case PetAnimation::LOOKING:
        return PET_LOOKING_FRAMES;
    case PetAnimation::WAVING:
        return PET_WAVING_FRAMES;
    case PetAnimation::THINKING:
        return PET_THINKING_FRAMES;
    case PetAnimation::SCARED:
        return PET_SCARED_FRAMES;
    case PetAnimation::HOPPING:
        return PET_HOPPING_FRAMES;
    case PetAnimation::SCRATCHING:
        return PET_SCRATCHING_FRAMES;
    case PetAnimation::DANCING:
        return PET_DANCING_FRAMES;
    case PetAnimation::YAWNING:
        return PET_YAWNING_FRAMES;
    case PetAnimation::SNIFFING:
        return PET_SNIFFING_FRAMES;
    default:
        return 1;
    }
}

#if HAS_SCREEN

void PetModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    if (!enabled)
        return;

    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    // Determine scale factor based on screen resolution
    const uint8_t scale = graphics::isHighResolution() ? 2 : 1;

    // Draw header
    const char *titleStr = "Chirpy";
    graphics::drawCommonHeader(display, x, y, titleStr);

    // Get display dimensions
    const int16_t screenW = display->getWidth();
    const int16_t screenH = display->getHeight();
    const int16_t contentY = y + FONT_HEIGHT_SMALL; // Tighter - right below header

    // Scaled dimensions
    const int16_t moodSize = 16 * scale;
    const int16_t petW = PET_FRAME_WIDTH * scale;
    const int16_t petH = PET_FRAME_HEIGHT * scale;
    const int16_t lvlIconW = lvl_icon_width * scale;
    const int16_t lvlIconH = lvl_icon_height * scale;

    // Mood indicator in top-left corner (below header)
    drawMoodIndicator(display, x + 1, contentY + 3 * scale, scale);

    // Show level under mood indicator - icon with number below
    int16_t lvlIconX = x + 3;
    int16_t lvlIconY = contentY + 3 * scale + moodSize;
    if (scale > 1) {
        drawXbmScaled(display, lvlIconX, lvlIconY, lvl_icon_width, lvl_icon_height, lvl_icon, scale);
    } else {
        display->drawXbm(lvlIconX, contentY + 3 + 16, lvl_icon_width, lvl_icon_height, lvl_icon);
    }
    char lvlBuf[8];
    snprintf(lvlBuf, sizeof(lvlBuf), "%u", level);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    int16_t lvlNumY = (scale > 1) ? (lvlIconY + lvlIconH - 1) : (contentY + 3 + 16 + lvl_icon_height - 3);
    display->drawString(lvlIconX + (lvlIconW / 2), lvlNumY, lvlBuf);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Pet area - wider box for pet
    const int16_t petBoxW = petW + 32 * scale;
    const int16_t petBoxH = petH + 4 * scale;
    const int16_t petBoxX = x + 18 * scale; // After mood indicator
    const int16_t petBoxY = contentY + 3 * scale;

    // Draw pet area with border
    drawPetArea(display, petBoxX, petBoxY, petBoxW, petBoxH, scale);

    // Draw the pet at petX position within box
    int16_t walkRange = petBoxW - petW - 4 * scale; // Available walk space
    int16_t petDrawX = petBoxX + 2 * scale + ((petX * walkRange) / 20);
    int16_t petDrawY = petBoxY + (petBoxH - petH) / 2;
    drawPet(display, petDrawX, petDrawY, scale);

    // Stats on the right side - align with top of pet box
    const int16_t statsX = petBoxX + petBoxW + 3 * scale;
    drawStats(display, statsX, petBoxY - 3, screenW - statsX);

    // Row below pet and stats: Last packet type with hops and time ago
    int16_t lastY = petBoxY + petBoxH + (scale > 1 ? 2 : -2);
    char buf[48];

    // Calculate icon scale based on font size
    uint8_t iconScale = (FONT_HEIGHT_SMALL >= 16) ? 2 : 1;
    int16_t iconW = last_icon_width * iconScale;
    int16_t iconH = last_icon_height * iconScale;
    int16_t textOffset = iconW + 2;

    // Draw last icon
    drawXbmScaled(display, x, lastY + (FONT_HEIGHT_SMALL - iconH) / 2, last_icon_width, last_icon_height, last_icon, iconScale);

    // Format time ago
    char timeBuf[8];
    if (lastMessageTime > 0) {
        uint32_t elapsed = (millis() - lastMessageTime) / 1000; // seconds
        if (elapsed < 60) {
            snprintf(timeBuf, sizeof(timeBuf), "%lus", (unsigned long)elapsed);
        } else if (elapsed < 3600) {
            snprintf(timeBuf, sizeof(timeBuf), "%lum", (unsigned long)(elapsed / 60));
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "%luh", (unsigned long)(elapsed / 3600));
        }
        snprintf(buf, sizeof(buf), "%s [%u] (%s)", getMessageTypeName(lastMessageType), lastMessageHops, timeBuf);
    } else {
        snprintf(buf, sizeof(buf), "%s", getMessageTypeName(lastMessageType));
    }
    display->drawString(x + textOffset, lastY + (iconScale > 1 ? 2 : 0), buf);

    // Status bars directly below Last (with extra padding for high-res)
    int16_t barY = lastY + FONT_HEIGHT_SMALL - 1 + (iconScale > 1 ? 4 : 0);
    int16_t barW = (screenW / 2) - 3;

    // Heart for happiness, EXP for experience
    drawStatusBarWithIcon(display, x, barY, barW, happiness, true, scale);
    uint8_t xpPercent = (xpForNextLevel > 0) ? (uint8_t)((experience * 100) / xpForNextLevel) : 0;
    drawStatusBarWithIcon(display, x + barW + 4, barY, barW, xpPercent, false, scale);
}

// Helper function to draw XBM bitmap scaled 2x (for high-resolution screens)
void PetModule::drawXbmScaled(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *xbm,
                              uint8_t scale)
{
    int16_t bytesPerRow = (width + 7) / 8;

    for (int16_t row = 0; row < height; row++) {
        for (int16_t col = 0; col < width; col++) {
            int16_t byteIndex = row * bytesPerRow + (col / 8);
            int16_t bitIndex = col % 8;
            uint8_t byte = pgm_read_byte(&xbm[byteIndex]);
            bool pixel = (byte >> bitIndex) & 1;

            if (pixel) {
                // Draw scaled pixel (scale x scale block)
                for (uint8_t sy = 0; sy < scale; sy++) {
                    for (uint8_t sx = 0; sx < scale; sx++) {
                        display->setPixel(x + col * scale + sx, y + row * scale + sy);
                    }
                }
            }
        }
    }
}

// Helper function to draw XBM bitmap flipped horizontally (with optional scale)
void PetModule::drawXbmFlipped(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *xbm,
                               uint8_t scale)
{
    // XBM format: each row is stored as bytes, LSB first within each byte
    // We need to draw pixels from right to left for horizontal flip
    int16_t bytesPerRow = (width + 7) / 8;

    for (int16_t row = 0; row < height; row++) {
        for (int16_t col = 0; col < width; col++) {
            // Calculate which byte and bit this pixel is in
            int16_t byteIndex = row * bytesPerRow + (col / 8);
            int16_t bitIndex = col % 8;

            // Read the pixel (XBM is LSB first)
            uint8_t byte = pgm_read_byte(&xbm[byteIndex]);
            bool pixel = (byte >> bitIndex) & 1;

            if (pixel) {
                // Draw at flipped X position with scale
                int16_t flippedCol = width - 1 - col;
                for (uint8_t sy = 0; sy < scale; sy++) {
                    for (uint8_t sx = 0; sx < scale; sx++) {
                        display->setPixel(x + flippedCol * scale + sx, y + row * scale + sy);
                    }
                }
            }
        }
    }
}

void PetModule::drawPet(OLEDDisplay *display, int16_t x, int16_t y, uint8_t scale)
{
    const uint8_t *frameData = nullptr;
    bool flipHorizontal = false;

    // Select the appropriate animation frame
    switch (currentAnimation) {
    case PetAnimation::IDLE:
        frameData = petIdleFrames[animationFrame % PET_IDLE_FRAMES];
        break;
    case PetAnimation::WALKING:
        frameData = petWalkFrames[animationFrame % PET_WALK_FRAMES];
        // Flip sprite when walking left
        flipHorizontal = (petDirection < 0);
        break;
    case PetAnimation::HAPPY_BOUNCE:
        frameData = petHappyFrames[animationFrame % PET_HAPPY_FRAMES];
        break;
    case PetAnimation::SLEEPING:
        frameData = petSleepFrames[animationFrame % PET_SLEEP_FRAMES];
        break;
    case PetAnimation::EXCITED:
        frameData = petExcitedFrames[animationFrame % PET_EXCITED_FRAMES];
        break;
    case PetAnimation::EATING:
        frameData = petEatingFrames[animationFrame % PET_EATING_FRAMES];
        break;
    case PetAnimation::PLAYING:
        frameData = petPlayingFrames[animationFrame % PET_PLAYING_FRAMES];
        break;
    case PetAnimation::ALERT:
        frameData = petAlertFrames[animationFrame % PET_ALERT_FRAMES];
        break;
    case PetAnimation::SAD:
        frameData = petSadFrames[animationFrame % PET_SAD_FRAMES];
        break;
    case PetAnimation::READING:
        frameData = petReadingFrames[animationFrame % PET_READING_FRAMES];
        break;
    case PetAnimation::LOOKING:
        frameData = petLookingFrames[animationFrame % PET_LOOKING_FRAMES];
        break;
    case PetAnimation::WAVING:
        frameData = petWavingFrames[animationFrame % PET_WAVING_FRAMES];
        break;
    case PetAnimation::THINKING:
        frameData = petThinkingFrames[animationFrame % PET_THINKING_FRAMES];
        break;
    case PetAnimation::SCARED:
        frameData = petScaredFrames[animationFrame % PET_SCARED_FRAMES];
        break;
    case PetAnimation::HOPPING:
        frameData = petHoppingFrames[animationFrame % PET_HOPPING_FRAMES];
        // Flip when moving left
        flipHorizontal = (petDirection < 0);
        break;
    case PetAnimation::SCRATCHING:
        frameData = petScratchingFrames[animationFrame % PET_SCRATCHING_FRAMES];
        break;
    case PetAnimation::DANCING:
        frameData = petDancingFrames[animationFrame % PET_DANCING_FRAMES];
        break;
    case PetAnimation::YAWNING:
        frameData = petYawningFrames[animationFrame % PET_YAWNING_FRAMES];
        break;
    case PetAnimation::SNIFFING:
        frameData = petSniffingFrames[animationFrame % PET_SNIFFING_FRAMES];
        // Alternate direction while sniffing
        flipHorizontal = (animationFrame % 2 == 1);
        break;
    }

    if (frameData) {
        if (flipHorizontal) {
            drawXbmFlipped(display, x, y, PET_FRAME_WIDTH, PET_FRAME_HEIGHT, frameData, scale);
        } else if (scale > 1) {
            drawXbmScaled(display, x, y, PET_FRAME_WIDTH, PET_FRAME_HEIGHT, frameData, scale);
        } else {
            display->drawXbm(x, y, PET_FRAME_WIDTH, PET_FRAME_HEIGHT, frameData);
        }
    }
}

void PetModule::drawStats(OLEDDisplay *display, int16_t x, int16_t y, int16_t width)
{
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    char buf[24];
    // Calculate icon scale based on font size
    uint8_t iconScale = (FONT_HEIGHT_SMALL >= 16) ? 2 : 1;
    // Add extra padding between rows for high-res screens
    int16_t lineH = FONT_HEIGHT_SMALL - 4 + (iconScale > 1 ? 4 : 0);
    int16_t iconW = messages_icon_width * iconScale;
    int16_t iconH = messages_icon_height * iconScale;
    int16_t textOffset = iconW + 2;                // Gap between icon and text
    int16_t textYOffset = (iconScale > 1) ? 2 : 0; // Move text down 2px for high-res

    // Row 1: Messages received (envelope icon) - icon shifted down 1 pixel
    drawXbmScaled(display, x, y + 1 + (lineH - iconH) / 2, messages_icon_width, messages_icon_height, messages_icon, iconScale);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)messagesReceived);
    display->drawString(x + textOffset, y + textYOffset, buf);

    // Row 2: Nodes: active/total (nodes icon) - icon shifted down 1 pixel
    uint16_t activeNodes = 0;
    uint16_t totalNodes = 0;
    if (nodeStatus) {
        activeNodes = nodeStatus->getNumOnline();
        totalNodes = nodeStatus->getNumTotal();
    }
    drawXbmScaled(display, x, y + 1 + lineH + (lineH - iconH) / 2, nodes_icon_width, nodes_icon_height, nodes_icon, iconScale);
    snprintf(buf, sizeof(buf), "%u/%u", activeNodes, totalNodes);
    display->drawString(x + textOffset, y + lineH + textYOffset, buf);

    // Row 3: Uptime (clock icon) - icon shifted down 1 pixel
    uint32_t uptimeHours = getUptimeMinutes() / 60;
    uint32_t uptimeMins = getUptimeMinutes() % 60;
    drawXbmScaled(display, x, y + 1 + lineH * 2 + (lineH - iconH) / 2, uptime_icon_width, uptime_icon_height, uptime_icon,
                  iconScale);
    if (uptimeHours > 0) {
        snprintf(buf, sizeof(buf), "%luh%02lum", (unsigned long)uptimeHours, (unsigned long)uptimeMins);
    } else {
        snprintf(buf, sizeof(buf), "%lum", (unsigned long)uptimeMins);
    }
    display->drawString(x + textOffset, y + lineH * 2 + textYOffset, buf);
}

void PetModule::drawStatusBarWithIcon(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, uint8_t percent, bool isHeart,
                                      uint8_t scale)
{
    // Draw tiny icon (heart or exp)
    int16_t iconW;
    int16_t iconH;

    if (isHeart) {
        // Draw tiny heart icon
        iconW = heart_icon_width;
        iconH = heart_icon_height;
        if (scale > 1) {
            drawXbmScaled(display, x, y, iconW, iconH, heart_icon, scale);
        } else {
            display->drawXbm(x, y, iconW, iconH, heart_icon);
        }
    } else {
        // Draw EXP icon
        iconW = exp_icon_width;
        iconH = exp_icon_height;
        if (scale > 1) {
            drawXbmScaled(display, x, y, iconW, iconH, exp_icon, scale);
        } else {
            display->drawXbm(x, y, iconW, iconH, exp_icon);
        }
    }

    // Scale icon dimensions for bar positioning
    int16_t scaledIconW = iconW * scale;
    int16_t scaledIconH = iconH * scale;

    // Draw thin bar (scaled height)
    int16_t barX = x + scaledIconW + 2 * scale;
    int16_t barW = width - scaledIconW - 2 * scale;
    int16_t barH = 5 * scale;

    display->drawRect(barX, y, barW, barH);

    // Remove corner pixels for rounded effect
    display->setColor(BLACK);
    display->setPixel(barX, y);
    display->setPixel(barX + barW - 1, y);
    display->setPixel(barX, y + barH - 1);
    display->setPixel(barX + barW - 1, y + barH - 1);
    display->setColor(WHITE);

    // Draw filled portion
    int16_t fillW = ((barW - 2) * percent) / 100;
    if (fillW > 0) {
        display->fillRect(barX + 1, y + 1, fillW, barH - 2);
    }
}

void PetModule::drawPetArea(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, uint8_t scale)
{
    // Draw a box for the pet area (double thick for high resolution)
    display->drawRect(x, y, width, height);
    if (scale > 1) {
        display->drawRect(x + 1, y + 1, width - 2, height - 2);
    }

    // Remove corner pixels for rounded effect
    display->setColor(BLACK);
    display->setPixel(x, y);
    display->setPixel(x + width - 1, y);
    display->setPixel(x, y + height - 1);
    display->setPixel(x + width - 1, y + height - 1);
    if (scale > 1) {
        // Additional corner cleanup for double-thick border
        display->setPixel(x + 1, y);
        display->setPixel(x, y + 1);
        display->setPixel(x + width - 2, y);
        display->setPixel(x + width - 1, y + 1);
        display->setPixel(x, y + height - 2);
        display->setPixel(x + 1, y + height - 1);
        display->setPixel(x + width - 1, y + height - 2);
        display->setPixel(x + width - 2, y + height - 1);
    }
    display->setColor(WHITE);
}

void PetModule::drawMoodIndicator(OLEDDisplay *display, int16_t x, int16_t y, uint8_t scale)
{
#ifndef EXCLUDE_EMOJI
    // Draw mood indicator using emote bitmaps
    const unsigned char *moodBitmap = nullptr;
    int moodWidth = 16;
    int moodHeight = 16;

    switch (currentMood) {
    case PetMood::HAPPY:
        moodBitmap = graphics::grinning_smiling_eyes_2;
        moodWidth = grinning_smiling_eyes_2_width;
        moodHeight = grinning_smiling_eyes_2_height;
        break;
    case PetMood::NEUTRAL:
        moodBitmap = graphics::slightly_smiling;
        moodWidth = slightly_smiling_width;
        moodHeight = slightly_smiling_height;
        break;
    case PetMood::SAD:
        moodBitmap = graphics::loudly_crying_face;
        moodWidth = loudly_crying_face_width;
        moodHeight = loudly_crying_face_height;
        break;
    case PetMood::EXCITED:
        moodBitmap = graphics::grinning;
        moodWidth = grinning_width;
        moodHeight = grinning_height;
        break;
    case PetMood::SLEEPING:
        moodBitmap = graphics::first_quarter_moon_face;
        moodWidth = first_quarter_moon_face_width;
        moodHeight = first_quarter_moon_face_height;
        break;
    case PetMood::HUNGRY:
        moodBitmap = graphics::question;
        moodWidth = question_width;
        moodHeight = question_height;
        break;
    case PetMood::ALERT:
        moodBitmap = graphics::bang;
        moodWidth = bang_width;
        moodHeight = bang_height;
        break;
    }

    if (moodBitmap) {
        if (scale > 1) {
            drawXbmScaled(display, x, y, moodWidth, moodHeight, moodBitmap, scale);
        } else {
            display->drawXbm(x, y, moodWidth, moodHeight, moodBitmap);
        }
    }
#else
    // Fallback to ASCII if emojis are excluded
    const char *moodStr = "";

    switch (currentMood) {
    case PetMood::HAPPY:
        moodStr = ":)";
        break;
    case PetMood::NEUTRAL:
        moodStr = ":|";
        break;
    case PetMood::SAD:
        moodStr = ":(";
        break;
    case PetMood::EXCITED:
        moodStr = ":D";
        break;
    case PetMood::SLEEPING:
        moodStr = "Zz";
        break;
    case PetMood::HUNGRY:
        moodStr = ":O";
        break;
    case PetMood::ALERT:
        moodStr = "!!";
        break;
    }

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(x, y, moodStr);
#endif
}

const char *PetModule::getMessageTypeName(LastMessageType type)
{
    switch (type) {
    case LastMessageType::TEXT:
        return "Text";
    case LastMessageType::TEXT_COMPRESSED:
        return "TextComp";
    case LastMessageType::POSITION:
        return "Position";
    case LastMessageType::TELEMETRY:
        return "Telemetry";
    case LastMessageType::NODEINFO:
        return "NodeInfo";
    case LastMessageType::ROUTING:
        return "Routing";
    case LastMessageType::ADMIN:
        return "Admin";
    case LastMessageType::WAYPOINT:
        return "Waypoint";
    case LastMessageType::TRACEROUTE:
        return "Traceroute";
    case LastMessageType::NEIGHBOR:
        return "Neighbor";
    case LastMessageType::STORE_FWD:
        return "Store&Fwd";
    case LastMessageType::STORE_FWD_PP:
        return "S&F++";
    case LastMessageType::RANGE_TEST:
        return "RangeTest";
    case LastMessageType::DETECTION:
        return "Detection";
    case LastMessageType::ALERT:
        return "Alert";
    case LastMessageType::PAXCOUNT:
        return "PaxCount";
    case LastMessageType::REMOTE_HW:
        return "RemoteHW";
    case LastMessageType::AUDIO:
        return "Audio";
    case LastMessageType::KEY_VERIFY:
        return "KeyVerify";
    case LastMessageType::REPLY:
        return "Reply";
    case LastMessageType::IP_TUNNEL:
        return "IPTunnel";
    case LastMessageType::NODE_STATUS:
        return "NodeStatus";
    case LastMessageType::SERIAL_PORT:
        return "Serial";
    case LastMessageType::ZPS:
        return "ZPS";
    case LastMessageType::SIMULATOR:
        return "Simulator";
    case LastMessageType::ATAK:
        return "ATAK";
    case LastMessageType::ATAK_FWD:
        return "ATAKFwd";
    case LastMessageType::MAP_REPORT:
        return "MapReport";
    case LastMessageType::POWERSTRESS:
        return "PwrStress";
    case LastMessageType::RETICULUM:
        return "Reticulum";
    case LastMessageType::CAYENNE:
        return "Cayenne";
    case LastMessageType::PRIVATE:
        return "Private";
    case LastMessageType::UNKNOWN:
        return "Unknown";
    case LastMessageType::NONE:
    default:
        return "None";
    }
}

#endif // HAS_SCREEN

#endif // !MESHTASTIC_EXCLUDE_PET
