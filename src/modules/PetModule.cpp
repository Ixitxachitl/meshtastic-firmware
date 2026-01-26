#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_PET

#include "NodeDB.h"
#include "PetImages.h"
#include "PetModule.h"
#include "graphics/SharedUIDisplay.h"
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

    // Check if this is from a "new" node we haven't counted
    // (Simple heuristic - actual implementation might track unique node IDs)
    if (nodeDB) {
        uint32_t currentNodes = nodeDB->getNumMeshNodes();
        if (currentNodes > nodesDiscovered) {
            nodesDiscovered = currentNodes;
            // New node discovered - get excited!
            currentMood = PetMood::EXCITED;
            currentAnimation = PetAnimation::EXCITED;
        }
    }

    // Update mood if we receive messages (shows activity)
    if (currentMood != PetMood::EXCITED) {
        currentMood = PetMood::HAPPY;
        currentAnimation = PetAnimation::HAPPY_BOUNCE;
    }

    return ProcessMessage::CONTINUE; // Let other modules also process
}

uint32_t PetModule::getUptimeMinutes() const
{
    return (millis() - bootTime) / 60000;
}

void PetModule::updateMood()
{
    uint32_t now = millis();
    uint32_t timeSinceActivity = now - lastActivityTime;

    // Determine mood based on recent activity
    if (timeSinceActivity > IDLE_THRESHOLD_MS) {
        // Been idle for a while - pet goes to sleep
        currentMood = PetMood::SLEEPING;
        currentAnimation = PetAnimation::SLEEPING;
    } else if (timeSinceActivity > IDLE_THRESHOLD_MS / 2) {
        // Getting bored
        currentMood = PetMood::SAD;
        currentAnimation = PetAnimation::IDLE;
    } else if (messagesReceived > 0 && (messagesReceived % 10) < 3) {
        // Normal activity
        currentMood = PetMood::NEUTRAL;
        currentAnimation = PetAnimation::WALKING;
    } else {
        // Good activity
        currentMood = PetMood::HAPPY;
        currentAnimation = PetAnimation::IDLE;
    }
}

void PetModule::updateAnimation()
{
    // Handle walking animation - move pet back and forth
    if (currentAnimation == PetAnimation::WALKING) {
        petX += petDirection * 2;
        // Bounce off screen edges (assuming ~64 pixel usable width)
        if (petX > 48) {
            petDirection = -1;
        } else if (petX < 16) {
            petDirection = 1;
        }
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

    // Draw header
    const char *titleStr = "MeshPet";
    graphics::drawCommonHeader(display, x, y, titleStr);

    // Get display dimensions
    const int16_t screenW = display->getWidth();
    const int16_t screenH = display->getHeight();

    // Draw the pet in the center area
    int16_t petAreaY = y + FONT_HEIGHT_SMALL + 4;
    drawPet(display, x + petX, petAreaY);

    // Draw mood indicator
    drawMoodIndicator(display, x + screenW - 20, petAreaY);

    // Draw stats at the bottom
    int16_t statsY = screenH - FONT_HEIGHT_SMALL - 2;
    drawStats(display, x, statsY);
}

// Helper function to draw XBM bitmap flipped horizontally
void PetModule::drawXbmFlipped(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *xbm)
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
                // Draw at flipped X position
                int16_t flippedX = x + (width - 1 - col);
                display->setPixel(flippedX, y + row);
            }
        }
    }
}

void PetModule::drawPet(OLEDDisplay *display, int16_t x, int16_t y)
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
    }

    if (frameData) {
        if (flipHorizontal) {
            drawXbmFlipped(display, x, y, PET_FRAME_WIDTH, PET_FRAME_HEIGHT, frameData);
        } else {
            display->drawXbm(x, y, PET_FRAME_WIDTH, PET_FRAME_HEIGHT, frameData);
        }
    }
}

void PetModule::drawStats(OLEDDisplay *display, int16_t x, int16_t y)
{
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Format: "Msgs: X | Nodes: Y | Up: Zh"
    char statsStr[64];
    uint32_t uptimeHours = getUptimeMinutes() / 60;
    uint32_t uptimeMins = getUptimeMinutes() % 60;

    if (uptimeHours > 0) {
        snprintf(statsStr, sizeof(statsStr), "M:%lu N:%lu %luh%lum", (unsigned long)messagesReceived,
                 (unsigned long)nodesDiscovered, (unsigned long)uptimeHours, (unsigned long)uptimeMins);
    } else {
        snprintf(statsStr, sizeof(statsStr), "M:%lu N:%lu %lum", (unsigned long)messagesReceived, (unsigned long)nodesDiscovered,
                 (unsigned long)uptimeMins);
    }

    display->drawString(x, y, statsStr);
}

void PetModule::drawMoodIndicator(OLEDDisplay *display, int16_t x, int16_t y)
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
    }

    if (moodBitmap) {
        display->drawXbm(x - moodWidth, y, moodWidth, moodHeight, moodBitmap);
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
    }

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_RIGHT);
    display->drawString(x, y, moodStr);
#endif
}

#endif // HAS_SCREEN

#endif // !MESHTASTIC_EXCLUDE_PET
