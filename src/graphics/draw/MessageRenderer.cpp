#include "configuration.h"
#if HAS_SCREEN
#include "MessageRenderer.h"

// Core includes
#include "MessageStore.h"
#include "NodeDB.h"
#include "UIRenderer.h"
#include "buzz/buzz.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TimeFormatters.h"
#include "graphics/emotes.h"
#include "main.h"
#include "meshUtils.h"
#include <string>
#include <vector>

// Exception support varies by platform; avoid try/catch when disabled
#if defined(__EXCEPTIONS)
#define MR_HAS_EXCEPTIONS 1
#else
#define MR_HAS_EXCEPTIONS 0
#endif

template <typename T> static inline void reserve_best_effort(std::vector<T> &v, size_t n)
{
#if MR_HAS_EXCEPTIONS
    try {
        v.reserve(n);
    } catch (...) {
        // ignore failures
    }
#else
    (void)n; // skip reserving to avoid abort on OOM with -fno-exceptions
#endif
}

// External declarations
extern bool hasUnreadMessage;
extern meshtastic_DeviceState devicestate;
extern graphics::Screen *screen;

using graphics::Emote;
using graphics::emotes;
using graphics::numEmotes;

namespace graphics
{
namespace MessageRenderer
{

static std::vector<std::string> cachedLines;
static std::vector<int> cachedHeights;

// Rebuild control
static bool s_dirty = true; // true = caches must be rebuilt
static inline void markDirty()
{
    s_dirty = true;
}

static std::vector<bool> cachedIsHeader;
static std::vector<bool> cachedIsMine;
static std::vector<AckStatus> cachedAckForLine;

// Time metadata for live label (index-aligned with cachedLines)
static std::vector<uint32_t> cachedMsgTimestamp; // original message timestamp
static std::vector<bool> cachedIsBootRelative;   // true if timestamp is boot-relative

// Cached filtered messages to avoid recreating the deque on every frame
static std::deque<StoredMessage> cachedFiltered;
static ThreadMode lastFilterMode = ThreadMode::ALL;
static int lastFilterChannel = -1;
static uint32_t lastFilterPeer = 0;

// C++11-friendly helpers (no generic-lambda params)
template <typename T> static inline void trim_vec_front(std::vector<T> &v, size_t n)
{
    if (v.empty())
        return;
    size_t k = std::min(n, v.size());
    v.erase(v.begin(), v.begin() + k);
}

static bool shedOldest(size_t n)
{
    const size_t before = cachedLines.size();
    if (!before)
        return false;

    trim_vec_front(cachedLines, n);
    trim_vec_front(cachedHeights, n);
    trim_vec_front(cachedIsHeader, n);
    trim_vec_front(cachedIsMine, n);
    trim_vec_front(cachedAckForLine, n);
    trim_vec_front(cachedMsgTimestamp, n);
    trim_vec_front(cachedIsBootRelative, n);

    // If we've shed a significant amount, shrink capacity to reduce memory footprint
    if (n >= before / 2) {
        cachedLines.shrink_to_fit();
        cachedHeights.shrink_to_fit();
        cachedIsHeader.shrink_to_fit();
        cachedIsMine.shrink_to_fit();
        cachedAckForLine.shrink_to_fit();
        cachedMsgTimestamp.shrink_to_fit();
        cachedIsBootRelative.shrink_to_fit();
    }

    // request a clean rebuild/relayout; draw code clamps scroll position
    markDirty();
    return cachedLines.size() < before;
}

template <typename F> static bool retry_on_oom(F &&fn, size_t shedCount = 16, int maxRetries = 3)
{
#if MR_HAS_EXCEPTIONS
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        try {
            fn();
            return true;
        } catch (const std::bad_alloc &) {
            if (!shedOldest(shedCount))
                return false;
        }
    }
    return false;
#else
    // Without exceptions: shed once and attempt the operation
    shedOldest(shedCount);
    fn();
    return true;
#endif
}

// ---- Low-RAM guards for line buffers ----
// Absolute caps; tune per device via build flags
#ifndef MR_MAX_TOTAL_LINES
#define MR_MAX_TOTAL_LINES 512
#endif
#ifndef MR_MAX_LINES_PER_MESSAGE
#define MR_MAX_LINES_PER_MESSAGE 32
#endif
#ifndef MR_SHED_BATCH
#define MR_SHED_BATCH 64
#endif
#ifndef MR_HIGH_WATER_MARK
#define MR_HIGH_WATER_MARK 384
#endif

static constexpr size_t kMaxTotalLines = MR_MAX_TOTAL_LINES;
static constexpr size_t kMaxLinesPerMessage = MR_MAX_LINES_PER_MESSAGE;
static constexpr size_t kShedBatch = MR_SHED_BATCH;
static constexpr size_t kHighWaterMark = MR_HIGH_WATER_MARK; // Start shedding when we hit this many lines

// Check if we should proactively shed cache to prevent memory issues
static inline void checkMemoryPressure()
{
    if (cachedLines.size() >= kHighWaterMark) {
        LOG_DEBUG("MessageRenderer: Cache high water mark reached (%zu lines), proactive shedding", cachedLines.size());
        shedOldest(kShedBatch);
    }
}

// Reserve for the lines vector with clamping and exception safety.
static bool try_reserve_lines(std::vector<std::string> &v, size_t want)
{
    if (want > kMaxTotalLines)
        want = kMaxTotalLines;
#if MR_HAS_EXCEPTIONS
    try {
        // Never request less than current size() to avoid shrink reallocation.
        if (want < v.size())
            want = v.size();
        v.reserve(want);
        return true;
    } catch (const std::bad_alloc &) {
        // Caller decides whether to shed caches or continue without reserve.
        return false;
    }
#else
    // Best-effort: skip pre-reserve; proceed without clamping memory
    (void)v;
    (void)want;
    return true;
#endif
}

// UTF-8 skip helper
static inline size_t utf8CharLen(uint8_t c)
{
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

// Custom bitmap for upside-down question mark (¿) - 8x12 pixels
static const unsigned char upsidedown_qmark[] PROGMEM = {
    0x00, // 00000000
    0x18, // 00011000  dot at top
    0x18, // 00011000  dot
    0x00, // 00000000  gap
    0x18, // 00011000  stem
    0x18, // 00011000  stem
    0x18, // 00011000  stem
    0x0C, // 00001100  curve
    0x06, // 00110000  curve
    0x66, // 01100110  curve
    0x7E, // 01111110  curve bottom
    0x3C  // 00111100  curve bottom
};
static const int upsidedown_qmark_width = 8;
static const int upsidedown_qmark_height = 12;

// Remove variation selectors (FE0F) and skin tone modifiers from emoji so they match your labels
std::string normalizeEmoji(const std::string &s)
{
    std::string out;
    out.reserve(s.size()); // Pre-allocate to avoid reallocations

    for (size_t i = 0; i < s.size();) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        size_t len = utf8CharLen(c);

        if (c == 0xEF && i + 2 < s.size() && (uint8_t)s[i + 1] == 0xB8 && (uint8_t)s[i + 2] == 0x8F) {
            i += 3;
            continue;
        }

        // Skip skin tone modifiers
        if (c == 0xF0 && i + 3 < s.size() && (uint8_t)s[i + 1] == 0x9F && (uint8_t)s[i + 2] == 0x8F &&
            ((uint8_t)s[i + 3] >= 0xBB && (uint8_t)s[i + 3] <= 0xBF)) {
            i += 4;
            continue;
        }

        out.append(s, i, len);
        i += len;
    }
    return out;
}

// Replace unknown 4-byte emoji with upside-down question mark
std::string replaceUnknownEmoji(const std::string &s, const Emote *emotes, int emoteCount)
{
    // Normalize input to strip variation selectors and skin tone modifiers first
    const std::string normInput = normalizeEmoji(s);

    std::string out;
    out.reserve(normInput.size());

    for (size_t i = 0; i < normInput.size();) {
        uint8_t c = static_cast<uint8_t>(normInput[i]);
        size_t charLen = utf8CharLen(c);

        // Check if this matches a known emote
        bool isKnownEmote = false;
        for (int e = 0; e < emoteCount; ++e) {
            const std::string labelNorm = normalizeEmoji(std::string(emotes[e].label));
            size_t labelLen = labelNorm.length();
            if (labelLen > 0 && i + labelLen <= normInput.size() && normInput.compare(i, labelLen, labelNorm) == 0) {
                isKnownEmote = true;
                out.append(normInput, i, labelLen);
                i += labelLen;
                break;
            }
        }

        if (isKnownEmote) {
            continue;
        }

        // Check if this is a 3-byte emoji or a 4-byte emoji (most modern emoji are 4-byte UTF-8)
        bool isUnknownEmoji = false;
        if (charLen == 4 && c == 0xF0) {
            // 4-byte emoji starting with 0xF0 (U+10000 and above)
            isUnknownEmoji = true;
        } else if (charLen == 3 && c == 0xE2 && i + 1 < normInput.size()) {
            // 3-byte sequences starting with 0xE2 - need to check second byte
            uint8_t b2 = static_cast<uint8_t>(normInput[i + 1]);
            // E2 80 XX = General Punctuation (U+2000-U+206F) - NOT emoji (includes ', ", –, —, etc.)
            // E2 81 XX = Subscripts/Superscripts (U+2070-U+209F) - NOT emoji
            // E2 86-97 XX = Arrows, Math, Technical (U+2190-U+27BF) - treat as emoji
            // E2 98-9B XX = Miscellaneous Symbols (U+2600-U+26FF) - includes ⚡☀️❤️ - emoji!
            // E2 9C-9F XX = Dingbats (U+2700-U+27BF) - emoji!
            if (b2 >= 0x86 && b2 <= 0x9F) {
                isUnknownEmoji = true; // Arrows, symbols, dingbats
            }
        } else if (charLen == 3 && c == 0xE3) {
            // 3-byte emoji starting with 0xE3 (U+3000-U+3FFF range)
            // Includes CJK Symbols and some emoji-like characters
            isUnknownEmoji = true;
        }

        if (isUnknownEmoji) {
            // Unknown emoji - replace with upside-down question mark
            out.append("\xC2\xBF"); // ¿ in UTF-8
            i += charLen;
        } else {
            // Regular character - keep it
            out.append(normInput, i, charLen);
            i += charLen;
        }
    }

    return out;
}

int getStringWidthWithEmotes(OLEDDisplay *display, const std::string &line, const Emote *emotes, int emoteCount)
{
    const std::string normLine = normalizeEmoji(line);
    int totalWidth = 0;

    for (size_t i = 0; i < normLine.length();) {
        bool matched = false;
        for (int e = 0; e < emoteCount; ++e) {
            const std::string labelNorm = normalizeEmoji(std::string(emotes[e].label));
            const size_t emojiLen = labelNorm.length();
            if (emojiLen && normLine.compare(i, emojiLen, labelNorm) == 0) {
                totalWidth += emotes[e].width + 1; // emote width + spacing
                i += emojiLen;
                matched = true;
                break;
            }
        }
        if (!matched) {
            size_t charLen = utf8CharLen(static_cast<uint8_t>(normLine[i]));
            // Check for special ¿ character
            if (charLen == 2 && (uint8_t)normLine[i] == 0xC2 && i + 1 < normLine.length() && (uint8_t)normLine[i + 1] == 0xBF) {
                totalWidth += upsidedown_qmark_width + 1;
            } else {
                std::string singleChar = normLine.substr(i, charLen);
#if defined(OLED_UA) || defined(OLED_RU)
                totalWidth += display->getStringWidth(singleChar.c_str(), singleChar.length(), true);
#else
                totalWidth += display->getStringWidth(singleChar.c_str());
#endif
            }
            i += charLen;
        }
    }
    return totalWidth;
}

void drawStringWithEmotes(OLEDDisplay *display, int x, int y, const std::string &line, const Emote *emotes, int emoteCount)
{
    // Normalize the incoming line so variation selectors and skin tones don't render as stray glyphs
    const std::string normLine = normalizeEmoji(line);
    int cursorX = x;
    const int fontHeight = FONT_HEIGHT_SMALL;

    // Step 1: Find tallest emote in the line
    int maxIconHeight = fontHeight;
    for (size_t i = 0; i < normLine.length();) {
        bool matched = false;
        for (int e = 0; e < emoteCount; ++e) {
            // Compare against normalized label so FE0F variants also match
            const std::string labelNorm = normalizeEmoji(std::string(emotes[e].label));
            const size_t emojiLen = labelNorm.length();
            if (emojiLen && normLine.compare(i, emojiLen, labelNorm) == 0) {
                if (emotes[e].height > maxIconHeight)
                    maxIconHeight = emotes[e].height;
                i += emojiLen;
                matched = true;
                break;
            }
        }
        if (!matched) {
            i += utf8CharLen(static_cast<uint8_t>(normLine[i]));
        }
    }

    // Step 2: Baseline alignment
    int lineHeight = std::max(fontHeight, maxIconHeight);
    int baselineOffset = (lineHeight - fontHeight) / 2;
    int fontY = y + baselineOffset;

    // Step 3: Render line in segments
    size_t i = 0;
    bool inBold = false;

    while (i < normLine.length()) {
        // Check for ** start/end for faux bold
        if (normLine.compare(i, 2, "**") == 0) {
            inBold = !inBold;
            i += 2;
            continue;
        }

        // Look ahead for the next emote match
        size_t nextEmotePos = std::string::npos;
        const Emote *matchedEmote = nullptr;
        size_t emojiLen = 0;

        for (int e = 0; e < emoteCount; ++e) {
            const std::string labelNorm = normalizeEmoji(std::string(emotes[e].label));
            if (labelNorm.empty())
                continue;
            size_t pos = normLine.find(labelNorm, i);
            if (pos != std::string::npos && (nextEmotePos == std::string::npos || pos < nextEmotePos)) {
                nextEmotePos = pos;
                matchedEmote = &emotes[e];
                emojiLen = labelNorm.length();
            }
        }

        // Render normal text segment up to the emote or bold toggle
        size_t nextControl = std::min(nextEmotePos, normLine.find("**", i));
        if (nextControl == std::string::npos)
            nextControl = normLine.length();

        if (nextControl > i) {
            std::string textChunk = normLine.substr(i, nextControl - i);

            // Handle special characters - render char by char to override width
            size_t j = 0;
            while (j < textChunk.length()) {
                size_t charLen = utf8CharLen(static_cast<uint8_t>(textChunk[j]));

                // Check if this is the ¿ character
                if (charLen == 2 && (uint8_t)textChunk[j] == 0xC2 && j + 1 < textChunk.length() &&
                    (uint8_t)textChunk[j + 1] == 0xBF) {
                    // Render custom upside-down question mark bitmap
                    int iconY = fontY + (fontHeight - upsidedown_qmark_height) / 2;
                    display->drawXbm(cursorX, iconY, upsidedown_qmark_width, upsidedown_qmark_height, upsidedown_qmark);
                    cursorX += upsidedown_qmark_width + 1; // Bitmap width + spacing
                    j += 2;
                    // Check if this is the ° character followed by C or F
                } else if (charLen == 2 && (uint8_t)textChunk[j] == 0xC2 && j + 1 < textChunk.length() &&
                           (uint8_t)textChunk[j + 1] == 0xB0 && j + 2 < textChunk.length() &&
                           (textChunk[j + 2] == 'C' || textChunk[j + 2] == 'F')) {
                    // Render °C or °F as a single unit to avoid spacing issues
                    std::string tempUnit = textChunk.substr(j, 3); // °C or °F
                    if (inBold) {
                        display->drawString(cursorX + 1, fontY, tempUnit.c_str());
                    }
                    display->drawString(cursorX, fontY, tempUnit.c_str());
#if defined(OLED_UA) || defined(OLED_RU)
                    cursorX += display->getStringWidth(tempUnit.c_str(), tempUnit.length(), true);
#else
                    cursorX += display->getStringWidth(tempUnit.c_str());
#endif
                    j += 3;
                } else {
                    // Regular character - render it
                    std::string singleChar = textChunk.substr(j, charLen);
                    if (inBold) {
                        display->drawString(cursorX + 1, fontY, singleChar.c_str());
                    }
                    display->drawString(cursorX, fontY, singleChar.c_str());
#if defined(OLED_UA) || defined(OLED_RU)
                    cursorX += display->getStringWidth(singleChar.c_str(), singleChar.length(), true);
#else
                    cursorX += display->getStringWidth(singleChar.c_str());
#endif
                    j += charLen;
                }
            }

            i = nextControl;
            continue;
        }

        // Render the emote (if found)
        if (matchedEmote && i == nextEmotePos) {
            // Vertically center emote relative to font baseline (not just midline)
            int iconY = fontY + (fontHeight - matchedEmote->height) / 2;
            display->drawXbm(cursorX, iconY, matchedEmote->width, matchedEmote->height, matchedEmote->bitmap);
            cursorX += matchedEmote->width + 1;
            i += emojiLen;
            continue;
        } else {
            // No more emotes — render the rest of the line
            std::string remaining = normLine.substr(i);

            // Handle special characters - render char by char to override width
            size_t j = 0;
            while (j < remaining.length()) {
                size_t charLen = utf8CharLen(static_cast<uint8_t>(remaining[j]));

                // Check if this is the ¿ character
                if (charLen == 2 && (uint8_t)remaining[j] == 0xC2 && j + 1 < remaining.length() &&
                    (uint8_t)remaining[j + 1] == 0xBF) {
                    // Render custom upside-down question mark bitmap
                    int iconY = fontY + (fontHeight - upsidedown_qmark_height) / 2;
                    display->drawXbm(cursorX, iconY, upsidedown_qmark_width, upsidedown_qmark_height, upsidedown_qmark);
                    cursorX += upsidedown_qmark_width + 1; // Bitmap width + spacing
                    j += 2;
                    // Check if this is the ° character followed by C or F
                } else if (charLen == 2 && (uint8_t)remaining[j] == 0xC2 && j + 1 < remaining.length() &&
                           (uint8_t)remaining[j + 1] == 0xB0 && j + 2 < remaining.length() &&
                           (remaining[j + 2] == 'C' || remaining[j + 2] == 'F')) {
                    // Render °C or °F as a single unit to avoid spacing issues
                    std::string tempUnit = remaining.substr(j, 3); // °C or °F
                    if (inBold) {
                        display->drawString(cursorX + 1, fontY, tempUnit.c_str());
                    }
                    display->drawString(cursorX, fontY, tempUnit.c_str());
#if defined(OLED_UA) || defined(OLED_RU)
                    cursorX += display->getStringWidth(tempUnit.c_str(), tempUnit.length(), true);
#else
                    cursorX += display->getStringWidth(tempUnit.c_str());
#endif
                    j += 3;
                } else {
                    // Regular character - render it
                    std::string singleChar = remaining.substr(j, charLen);
                    if (inBold) {
                        display->drawString(cursorX + 1, fontY, singleChar.c_str());
                    }
                    display->drawString(cursorX, fontY, singleChar.c_str());
#if defined(OLED_UA) || defined(OLED_RU)
                    cursorX += display->getStringWidth(singleChar.c_str(), singleChar.length(), true);
#else
                    cursorX += display->getStringWidth(singleChar.c_str());
#endif
                    j += charLen;
                }
            }
            break;
        }
    }
}

// Scroll state (file scope so we can reset on new message)
float scrollY = 0.0f;
uint32_t lastTime = 0;
uint32_t scrollStartDelay = 0;
uint32_t pauseStart = 0;
bool waitingToReset = false;
bool scrollStarted = false;
static bool manualScrollActive = false; // manual override pauses auto-scroll
static uint32_t lastManualMs = 0;
static int lastUsableScrollHeight = 0;
static bool didReset = false; // <-- add here

// Reset scroll state when new messages arrive
void resetScrollState()
{
    scrollY = 0.0f;
    scrollStarted = false;
    waitingToReset = false;
    scrollStartDelay = millis();
    manualScrollActive = false;
    lastTime = millis();

    didReset = false;
}

// Fully free cached message data from heap (no eager re-reserve)
void clearMessageCache()
{
    // Drop contents and capacity for all aligned caches
    cachedLines.clear();
    cachedLines.shrink_to_fit();

    cachedHeights.clear();
    cachedHeights.shrink_to_fit();

    cachedIsHeader.clear();
    cachedIsHeader.shrink_to_fit();

    cachedIsMine.clear();
    cachedIsMine.shrink_to_fit();

    cachedAckForLine.clear();
    cachedAckForLine.shrink_to_fit();

    cachedMsgTimestamp.clear();
    cachedMsgTimestamp.shrink_to_fit();

    cachedIsBootRelative.clear();
    cachedIsBootRelative.shrink_to_fit();

    cachedFiltered.clear();
    cachedFiltered.shrink_to_fit();

    // Rebuild from scratch on next draw
    resetScrollState();
    markDirty();
}

// Current thread state
static ThreadMode currentMode = ThreadMode::ALL;
static int currentChannel = -1;
static uint32_t currentPeer = 0;

// Registry of seen threads for manual toggle
static std::vector<int> seenChannels;
static std::vector<uint32_t> seenPeers;

// Public helper so menus / store can clear stale registries
void clearThreadRegistries()
{
    seenChannels.clear();
    seenPeers.clear();
}

// Setter so other code can switch threads
void setThreadMode(ThreadMode mode, int channel /* = -1 */, uint32_t peer /* = 0 */)
{
    currentMode = mode;
    currentChannel = channel;
    currentPeer = peer;
    // Immediately refresh the view after a mode change
    resetScrollState(); // clears scroll & prepares for a fresh layout
    markDirty();        // forces cache rebuild on next draw
    didReset = false;   // keep existing contract (we reset above)

    // Track channels we’ve seen
    if (mode == ThreadMode::CHANNEL && channel >= 0) {
        if (std::find(seenChannels.begin(), seenChannels.end(), channel) == seenChannels.end()) {
            seenChannels.push_back(channel);
        }
    }

    // Track DMs we’ve seen
    if (mode == ThreadMode::DIRECT && peer != 0) {
        if (std::find(seenPeers.begin(), seenPeers.end(), peer) == seenPeers.end()) {
            seenPeers.push_back(peer);
        }
    }
}

ThreadMode getThreadMode()
{
    return currentMode;
}

int getThreadChannel()
{
    return currentChannel;
}

uint32_t getThreadPeer()
{
    return currentPeer;
}

// Accessors for menuHandler
const std::vector<int> &getSeenChannels()
{
    return seenChannels;
}
const std::vector<uint32_t> &getSeenPeers()
{
    return seenPeers;
}

static int centerYForRow(int y, int size)
{
    int midY = y + (FONT_HEIGHT_SMALL / 2);
    return midY - (size / 2);
}

// Helpers for drawing status marks (thickened strokes)
static void drawCheckMark(OLEDDisplay *display, int x, int y, int size)
{
    int topY = centerYForRow(y, size);
    display->setColor(WHITE);
    display->drawLine(x, topY + size / 2, x + size / 3, topY + size);
    display->drawLine(x, topY + size / 2 + 1, x + size / 3, topY + size + 1);
    display->drawLine(x + size / 3, topY + size, x + size, topY);
    display->drawLine(x + size / 3, topY + size + 1, x + size, topY + 1);
}

static void drawXMark(OLEDDisplay *display, int x, int y, int size = 8)
{
    int topY = centerYForRow(y, size);
    display->setColor(WHITE);
    display->drawLine(x, topY, x + size, topY + size);
    display->drawLine(x, topY + 1, x + size, topY + size + 1);
    display->drawLine(x + size, topY, x, topY + size);
    display->drawLine(x + size, topY + 1, x, topY + size + 1);
}

static void drawRelayMark(OLEDDisplay *display, int x, int y, int size = 8)
{
    int r = size / 2;
    int centerY = centerYForRow(y, size) + r;
    int centerX = x + r;
    display->setColor(WHITE);
    display->drawCircle(centerX, centerY, r);
    display->drawLine(centerX, centerY - 2, centerX, centerY);
    display->setPixel(centerX, centerY + 2);
    display->drawLine(centerX - 1, centerY - 4, centerX + 1, centerY - 4);
}

static inline int getRenderedLineWidth(OLEDDisplay *display, const std::string &line, const Emote *emotes, int emoteCount)
{
    std::string normalized = normalizeEmoji(line);
    int totalWidth = 0;

    size_t i = 0;
    while (i < normalized.length()) {
        bool matched = false;
        for (int e = 0; e < emoteCount; ++e) {
            const std::string labelNorm = normalizeEmoji(std::string(emotes[e].label));
            size_t emojiLen = labelNorm.length();
            if (emojiLen > 0 && normalized.compare(i, emojiLen, labelNorm) == 0) {
                totalWidth += emotes[e].width + 1; // +1 spacing
                i += emojiLen;
                matched = true;
                break;
            }
        }
        if (!matched) {
            size_t charLen = utf8CharLen(static_cast<uint8_t>(normalized[i]));

            // Check if this is likely an emoji (4-byte UTF-8 starting with 0xF0, or 3-byte symbols)
            if ((charLen >= 4 && (uint8_t)normalized[i] == 0xF0) ||
                (charLen == 3 && ((uint8_t)normalized[i] == 0xE2 || (uint8_t)normalized[i] == 0xEF))) {
                // Unknown emoji: use a reasonable width estimate
                totalWidth += 16 + 1; // Most emojis are 16px + spacing
            } else if (charLen == 2 && (uint8_t)normalized[i] == 0xC2 && i + 1 < normalized.length() &&
                       (uint8_t)normalized[i + 1] == 0xBF) {
                // ¿ character (our emoji replacement) - use custom bitmap width
                totalWidth += 8 + 1; // upsidedown_qmark_width + spacing
            } else {
                // Regular character - use actual font width
#if defined(OLED_UA) || defined(OLED_RU)
                totalWidth += display->getStringWidth(normalized.substr(i, charLen).c_str(), charLen, true);
#else
                totalWidth += display->getStringWidth(normalized.substr(i, charLen).c_str());
#endif
            }
            i += charLen;
        }
    }
    return totalWidth;
}

void drawTextMessageFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    // Ensure any boot-relative timestamps are upgraded if RTC is valid
    messageStore.upgradeBootRelativeTimestamps();

    bool needsInitialBottomSnap = false;
    if (!didReset) {
        resetScrollState();
        didReset = true;
        needsInitialBottomSnap = true; // Remember we need to snap to bottom after calculating layout
    }

    // Clear the unread message indicator when viewing the message
    hasUnreadMessage = false;

    // Check if we need to rebuild the filtered message list
    bool filterChanged = (currentMode != lastFilterMode || currentChannel != lastFilterChannel || currentPeer != lastFilterPeer);

    if (filterChanged || s_dirty || cachedFiltered.empty()) {
        // Filter messages based on thread mode
        cachedFiltered.clear();
        for (const auto &m : messageStore.getLiveMessages()) {
            bool include = false;
            switch (currentMode) {
            case ThreadMode::ALL:
                include = true;
                break;
            case ThreadMode::CHANNEL:
                if (m.type == MessageType::BROADCAST && (int)m.channelIndex == currentChannel)
                    include = true;
                break;
            case ThreadMode::DIRECT:
                if (m.dest != NODENUM_BROADCAST && (m.sender == currentPeer || m.dest == currentPeer))
                    include = true;
                break;
            }
            if (include)
                cachedFiltered.push_back(m);
        }

        // Remember filter state
        lastFilterMode = currentMode;
        lastFilterChannel = currentChannel;
        lastFilterPeer = currentPeer;
    }

    // Use cached filtered list
    const std::deque<StoredMessage> &filtered = cachedFiltered;

    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    const int navHeight = FONT_HEIGHT_SMALL;
    const int scrollBottom = SCREEN_HEIGHT - navHeight;
    const int usableHeight = scrollBottom;
    const int textWidth = SCREEN_WIDTH;

    // Title string depending on mode
    static char titleBuf[32];
    const char *titleStr = "Messages";
    switch (currentMode) {
    case ThreadMode::ALL:
        titleStr = "Messages";
        break;
    case ThreadMode::CHANNEL: {
        const char *cname = channels.getName(currentChannel);
        if (cname && cname[0]) {
            snprintf(titleBuf, sizeof(titleBuf), "#%s", cname);
        } else {
            snprintf(titleBuf, sizeof(titleBuf), "Ch%d", currentChannel);
        }
        titleStr = titleBuf;
        break;
    }
    case ThreadMode::DIRECT: {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(currentPeer);
        if (node && node->has_user) {
            snprintf(titleBuf, sizeof(titleBuf), "@%s", node->user.short_name);
        } else {
            snprintf(titleBuf, sizeof(titleBuf), "@%08x", currentPeer);
        }
        titleStr = titleBuf;
        break;
    }
    }

    if (filtered.empty()) {
        // If current conversation is empty go back to ALL view
        if (currentMode != ThreadMode::ALL) {
            setThreadMode(ThreadMode::ALL);
            resetScrollState();
            markDirty();
            return; // Next draw will rerun in ALL mode
        }

        // Still in ALL mode and no messages at all → show placeholder
        graphics::drawCommonHeader(display, x, y, titleStr);
        didReset = false;
        const char *messageString = "No messages";
        int center_text = (SCREEN_WIDTH / 2) - (display->getStringWidth(messageString) / 2);
        display->drawString(center_text, getTextPositions(display)[2], messageString);
        return;
    }
    if (s_dirty) {
        // Check if we need to shed cache proactively before building new content
        checkMemoryPressure();

        // Build lines for filtered messages (newest first)
        std::vector<std::string> allLines;
        std::vector<bool> isMine;          // track alignment
        std::vector<bool> isHeader;        // track header lines
        std::vector<AckStatus> ackForLine; // per-header ACK badge

        // Reuse capacity but avoid giant reserves on tiny heaps.
        // Start with a modest estimate; clamp and tolerate failure.
        {
            size_t baseCap = cachedLines.capacity() ? cachedLines.capacity() : 256;
            size_t estimate = std::min(baseCap, kMaxTotalLines);
            (void)try_reserve_lines(allLines, estimate);

            // Match side vectors to whatever capacity we actually ended up with.
            size_t cap = allLines.capacity();
            reserve_best_effort(isMine, cap);
            reserve_best_effort(isHeader, cap);
            reserve_best_effort(ackForLine, cap);
        }

        std::vector<uint32_t> msgTs;
        std::vector<bool> isBootRel;
        // Side buffers reserve best-effort (don’t throw if they can’t).
        {
            size_t cap = allLines.capacity();
            reserve_best_effort(msgTs, cap);
            reserve_best_effort(isBootRel, cap);
        }

        for (auto it = filtered.begin(); it != filtered.end(); ++it) {
            const auto &m = *it;

            // Channel / destination labeling (single declaration)
            char chanType[32] = "";
            if (currentMode == ThreadMode::ALL) {
                if (m.type == MessageType::BROADCAST) {
                    snprintf(chanType, sizeof(chanType), "(Ch%d)", m.channelIndex);
                } else {
                    snprintf(chanType, sizeof(chanType), "(DM)");
                }
            }

            // We render time live; just reserve a conservative slot width (e.g., "999d")
            const int timeSlotPx = display->getStringWidth("999d");

            // Sender lookups (single set of declarations)
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(m.sender);
            meshtastic_NodeInfoLite *node_recipient = nodeDB->getMeshNode(m.dest);

            char senderBuf[48] = "???";
            if (node && node->has_user && node->user.long_name && node->user.long_name[0] != '\0') {
                std::snprintf(senderBuf, sizeof(senderBuf), "%s", node->user.long_name);
            }

            // If this is *our own* message, override senderBuf to the recipient's name
            bool mine = (m.sender == nodeDB->getNodeNum());
            if (mine && node_recipient && node_recipient->has_user && node_recipient->user.long_name &&
                node_recipient->user.long_name[0] != '\0') {
                std::snprintf(senderBuf, sizeof(senderBuf), "%s", node_recipient->user.long_name);
            }

            // Compute how much room the sender label has (use timeSlotPx, NOT timeBuf)
            int availWidth =
                SCREEN_WIDTH - timeSlotPx - display->getStringWidth(chanType) - display->getStringWidth(" @...") - 10;
            if (availWidth < 0)
                availWidth = 0;

            // Fit sender to available width with emoji support
            std::string senderStr(senderBuf);
            size_t origLen = senderStr.length();
            while (!senderStr.empty() && getRenderedLineWidth(display, senderStr, emotes, numEmotes) > availWidth) {
                // Remove last UTF-8 character
                size_t pos = senderStr.length();
                while (pos > 0 && (senderStr[pos - 1] & 0xC0) == 0x80) {
                    pos--; // Skip UTF-8 continuation bytes
                }
                if (pos > 0) {
                    pos--; // Remove the start of the UTF-8 character
                }
                senderStr.erase(pos);
            }

            // If we actually truncated, append "..."
            if (senderStr.length() < origLen) {
                senderStr += "...";
            }

            // Copy back to senderBuf
            strncpy(senderBuf, senderStr.c_str(), sizeof(senderBuf) - 1);
            senderBuf[sizeof(senderBuf) - 1] = '\0';

            // Final header line (no time here; time is drawn live during render)
            char headerStr[96] = "";
            if (mine)
                std::snprintf(headerStr, sizeof(headerStr), "%s", chanType);
            else
                std::snprintf(headerStr, sizeof(headerStr), "@%s %s", senderBuf, chanType);

            // Header line (guard memory)
            if (!retry_on_oom([&] {
                    allLines.emplace_back(headerStr); // Avoid temporary string construction
                    isMine.push_back(mine);
                    isHeader.push_back(true);
                    ackForLine.push_back(m.ackStatus);
                    // time metadata aligned with this header line
                    msgTs.push_back(m.timestamp);
                    isBootRel.push_back(m.isBootRelative);
                })) {
                break; // couldn't grow even after shedding
            }

            // Body lines (from feat/m5stack-cardputer-adv)
            const char *msgText = MessageStore::getText(m);

            // Replace unknown emoji with ? before processing
            std::string processedText = replaceUnknownEmoji(std::string(msgText), emotes, numEmotes);

            std::vector<std::string> wrapped = generateLines(display, "", processedText.c_str(), textWidth);

            // Cap per-message wrapped lines so one long message can't explode memory.
            if (wrapped.size() > kMaxLinesPerMessage) {
                // keep as many as possible, add ellipsis to last
                wrapped.resize(kMaxLinesPerMessage);
                if (!wrapped.empty()) {
                    // Avoid huge reallocation by appending a tiny suffix
                    wrapped.back() += " \xE2\x80\xA6"; // UTF-8 ellipsis
                }
            }
            for (auto &ln : wrapped) {
                if (!retry_on_oom([&] {
                        allLines.push_back(std::move(ln));
                        isMine.push_back(mine);
                        isHeader.push_back(false);
                        ackForLine.push_back(AckStatus::NONE);
                        // keep vectors aligned (no per-body time in this section)
                        msgTs.push_back(0);
                        isBootRel.push_back(false);
                    })) {
                    break; // stop adding more for this message
                }
                // If we are nearing the hard cap, try to expand a little, else keep going.
                if (allLines.size() + 8 >= allLines.capacity()) {
                    (void)try_reserve_lines(allLines, allLines.size() + 32);
                }
                // If reserve still can’t grow and we’re truly huge, stop early.
                if (allLines.size() >= kMaxTotalLines) {
                    break;
                }
            }
        }

        // Heights can also OOM — compute with retry and then swap all caches
        std::vector<int> newHeights;
        if (!retry_on_oom([&] { newHeights = calculateLineHeights(allLines, emotes, isHeader); })) {
            // keep existing caches; try again next time we become dirty
            s_dirty = false;
            return;
        }

        // swap into persistent caches (fast, avoids realloc)
        cachedLines.clear();
        cachedLines.swap(allLines);
        cachedHeights.clear();
        cachedHeights.swap(newHeights);
        cachedIsHeader.clear();
        cachedIsHeader.swap(isHeader);
        cachedIsMine.clear();
        cachedIsMine.swap(isMine);
        cachedAckForLine.clear();
        cachedAckForLine.swap(ackForLine);
        cachedMsgTimestamp.clear();
        cachedMsgTimestamp.swap(msgTs);
        cachedIsBootRelative.clear();
        cachedIsBootRelative.swap(isBootRel);
        s_dirty = false;
    }

    // Scrolling logic (reverse auto-scroll + manual override)
    // Newest-at-bottom is enforced by building lines via filtered.rbegin()->rend()
    int totalHeight = 0;
    for (size_t i = 0; i < cachedHeights.size(); ++i) {
        totalHeight += cachedHeights[i];
    }
    int usableScrollHeight = usableHeight;

#ifndef USE_EINK
    uint32_t now = millis();
    float delta = (now - lastTime) / 400.0f;
    lastTime = now;
    const float scrollSpeed = 2.0f;

    if (scrollStartDelay == 0)
        scrollStartDelay = now;
    if (!scrollStarted && now - scrollStartDelay > 2000)
        scrollStarted = true;

    // remember last usable height for manual scroll math
    lastUsableScrollHeight = usableScrollHeight;

    // bottom alignment (one text row above bottom)
    int kBottomPadPx = FONT_HEIGHT_SMALL * 2; // Two lines of spacing from bottom
    int bottomOffsetOneRow = totalHeight - usableScrollHeight + kBottomPadPx;
    if (bottomOffsetOneRow < 0)
        bottomOffsetOneRow = 0; // guard small lists

    // snap to bottom pad when first entering (unless user is manually scrolling)
    // needsInitialBottomSnap overrides scrollStarted to ensure we always start at bottom
    if (needsInitialBottomSnap || (!manualScrollActive && !scrollStarted)) {
        scrollY = bottomOffsetOneRow;
    }

    // Enable scrolling when content is close to fitting, not just when it overflows
    // This ensures messages scroll away from the top UI bar even with less content
    int scrollThreshold = usableScrollHeight - (FONT_HEIGHT_SMALL * 2); // Start scrolling 2 lines early
    if (totalHeight > scrollThreshold) {
        // freeze autoscroll briefly after manual input
        if (manualScrollActive && (now - lastManualMs) < 5000) {
            lastTime = now; // keep timebase fresh
        } else {
            if (manualScrollActive)
                manualScrollActive = false;
            if (scrollStarted) {
                if (!waitingToReset) {
                    // reverse (bottom->top) sweep
                    scrollY -= delta * scrollSpeed;
                    if (scrollY <= 0) {
                        scrollY = 0;
                        waitingToReset = true;
                        pauseStart = lastTime;
                    }
                } else if (lastTime - pauseStart > 3000) {
                    // jump back near bottom and restart
                    scrollY = bottomOffsetOneRow;
                    waitingToReset = false;
                    scrollStarted = false;
                    scrollStartDelay = lastTime;
                }
            }
        }
    } else {
        // content fits: bottom-align with one-row pad
        scrollY = bottomOffsetOneRow;
    }
#else
    // E-Ink: disable autoscroll but anchor to bottom
    int kBottomPadPx = FONT_HEIGHT_SMALL * 2; // Two lines of spacing from bottom
    int bottomOffsetOneRow = totalHeight - usableScrollHeight + kBottomPadPx;
    if (bottomOffsetOneRow < 0)
        bottomOffsetOneRow = 0; // guard small lists
    scrollY = bottomOffsetOneRow;
    waitingToReset = false;
    scrollStarted = false;
    lastTime = millis(); // keep timebase sane
#endif

    int scrollOffset = static_cast<int>(scrollY);
    int yOffset = -scrollOffset + getTextPositions(display)[1];

    // Render visible lines (clamp counts defensively)
    const size_t N =
        std::min({cachedLines.size(), cachedHeights.size(), cachedIsHeader.size(), cachedIsMine.size(), cachedAckForLine.size()});
    for (size_t i = 0; i < N; ++i) {
        int lineY = yOffset;
        for (size_t j = 0; j < i; ++j)
            lineY += cachedHeights[j];

        if (lineY > -cachedHeights[i] && lineY < (scrollBottom + cachedHeights[i])) {
            if (cachedIsHeader[i]) {
                // ---- LIVE time computation (same logic as before) ----
                uint32_t ts = (i < cachedMsgTimestamp.size()) ? cachedMsgTimestamp[i] : 0;
                bool isBootRel = (i < cachedIsBootRelative.size()) ? cachedIsBootRelative[i] : false;

                uint32_t nowSecs = getValidTime(RTCQuality::RTCQualityDevice, true);
                uint32_t seconds = 0;
                bool invalidTime = true;

                if (ts > 0 && nowSecs > 0) {
                    if (nowSecs >= ts) {
                        seconds = nowSecs - ts;
                        invalidTime = (seconds > 315360000); // >10 years
                    } else {
                        uint32_t ahead = ts - nowSecs;
                        if (ahead <= 600) { // allow small skew
                            seconds = 0;
                            invalidTime = false;
                        }
                    }
                } else if (ts > 0 && nowSecs == 0) {
                    // RTC not valid: only trust boot-relative if same boot
                    uint32_t bootNow = millis() / 1000;
                    if (isBootRel && ts <= bootNow) {
                        seconds = bootNow - ts;
                        invalidTime = false;
                    } else {
                        invalidTime = true;
                    }
                }

                char tbuf[16];
                if (invalidTime) {
                    snprintf(tbuf, sizeof(tbuf), "???");
                } else if (seconds < 60) {
                    snprintf(tbuf, sizeof(tbuf), "%us", seconds);
                } else if (seconds < 3600) {
                    snprintf(tbuf, sizeof(tbuf), "%um", seconds / 60);
                } else if (seconds < 86400) {
                    snprintf(tbuf, sizeof(tbuf), "%uh", seconds / 3600);
                } else {
                    snprintf(tbuf, sizeof(tbuf), "%ud", seconds / 86400);
                }

                // Compose full header inline: "<time> " + cached header (which holds @sender + chan)
                std::string fullHeader;
                fullHeader.reserve(strlen(tbuf) + 1 + cachedLines[i].size());
                fullHeader.append(tbuf).push_back(' ');
                fullHeader.append(cachedLines[i]);

                // Render header (measure/draw/underline using the full string)
                int w = getRenderedLineWidth(display, fullHeader, emotes, numEmotes);
                int headerX = cachedIsMine[i] ? (SCREEN_WIDTH - w - 2) : x;
                drawStringWithEmotes(display, headerX, lineY, fullHeader, emotes, numEmotes);

                // Draw ACK/NACK mark for our own messages
                if (cachedIsMine[i]) {
                    int markX = headerX - 10;
                    int markY = lineY;
                    if (cachedAckForLine[i] == AckStatus::ACKED) {
                        drawCheckMark(display, markX, markY, 8);
                    } else if (cachedAckForLine[i] == AckStatus::NACKED || cachedAckForLine[i] == AckStatus::TIMEOUT) {
                        drawXMark(display, markX, markY, 8);
                    } else if (cachedAckForLine[i] == AckStatus::RELAYED) {
                        drawRelayMark(display, markX, markY, 8);
                    }
                    // AckStatus::NONE → show nothing
                }

                // Draw underline just under header text
                int underlineY = lineY + FONT_HEIGHT_SMALL;
                for (int px = 0; px < w; ++px) {
                    display->setPixel(headerX + px, underlineY);
                }

            } else {
                // Render message line
                if (cachedIsMine[i]) {
                    // Calculate actual rendered width including emotes
                    int renderedWidth = getRenderedLineWidth(display, cachedLines[i], emotes, numEmotes);
                    int rightX = SCREEN_WIDTH - renderedWidth - 2; // -2 for slight padding from the edge
                    drawStringWithEmotes(display, rightX, lineY, cachedLines[i], emotes, numEmotes);
                } else {
                    drawStringWithEmotes(display, x, lineY, cachedLines[i], emotes, numEmotes);
                }
            }
        }
    }

    graphics::drawCommonHeader(display, x, y, titleStr);
}

std::vector<std::string> generateLines(OLEDDisplay *display, const char *headerStr, const char *messageBuf, int textWidth)
{
    std::vector<std::string> lines;

    // Pre-allocate reasonable capacity to avoid reallocations
    lines.reserve(8); // Most messages won't exceed 8 lines

    // Only push headerStr if it's not empty (prevents extra blank line after headers)
    if (headerStr && headerStr[0] != '\0') {
        lines.emplace_back(headerStr); // Use emplace_back to avoid temporary string
    }

    std::string line, word;
    // Pre-allocate string capacity for typical message lengths
    line.reserve(64);
    word.reserve(32);

    for (int i = 0; messageBuf[i];) {
        // Handle UTF-8 characters properly
        uint8_t firstByte = static_cast<uint8_t>(messageBuf[i]);
        size_t charLen = utf8CharLen(firstByte);

        // Handle smart quotes replacement
        if (firstByte == 0xE2 && i + 2 < (int)strlen(messageBuf) && (unsigned char)messageBuf[i + 1] == 0x80 &&
            (unsigned char)messageBuf[i + 2] == 0x99) {
            // Replace smart quote with plain apostrophe
            if (messageBuf[i] == '\n') {
                if (!word.empty())
                    line += word;
                if (!line.empty())
                    lines.emplace_back(std::move(line));
                line.clear();
                word.clear();
            } else if (messageBuf[i] == ' ') {
                line += word;
                line += ' ';
                word.clear();
            } else {
                word += '\'';
            }
            i += 3; // skip the 3-byte UTF-8 sequence
            continue;
        }

        // Extract the complete UTF-8 character
        std::string utfChar;
        utfChar.reserve(charLen);
        for (size_t j = 0; j < charLen && messageBuf[i + j]; ++j) {
            utfChar += messageBuf[i + j];
        }

        // Check if it's a newline or space (should be single byte)
        if (charLen == 1 && messageBuf[i] == '\n') {
            if (!word.empty())
                line += word;
            if (!line.empty())
                lines.emplace_back(std::move(line));
            line.clear();
            word.clear();
        } else if (charLen == 1 && messageBuf[i] == ' ') {
            line += word;
            line += ' ';
            word.clear();
        } else {
            // Check if this is an emoji (before adding to word)
            // Note: Don't treat ¿ as an emoji - it's a regular character replacement for unknown emoji
            bool isEmoji = (charLen >= 4 && firstByte == 0xF0) || (charLen == 3 && (firstByte == 0xE2 || firstByte == 0xEF));

            // If current word is not empty and we're about to add an emoji, flush word first
            if (isEmoji && !word.empty()) {
                // Flush current word to line
                line += word;
                word.clear();
            }

            // Add the complete UTF-8 character to word
            word += utfChar;

            // Check if adding this word would exceed the line width
            std::string test_buffer = line + word;
            uint16_t strWidth = getRenderedLineWidth(display, test_buffer, emotes, numEmotes);

            if (strWidth > textWidth) {
                if (!line.empty()) {
                    // Line is full, wrap
                    lines.emplace_back(std::move(line));
                    line = std::move(word);
                    word.clear();
                } else {
                    // Even a single word is too wide, force it to its own line
                    lines.emplace_back(std::move(word));
                    word.clear();
                    line.clear();
                }
            }

            // If we just added an emoji, flush it to line so next emoji can wrap independently
            if (isEmoji) {
                line += word;
                word.clear();
            }
        }

        i += charLen; // Move by the complete character length
    }

    if (!word.empty())
        line += word;
    if (!line.empty())
        lines.emplace_back(std::move(line)); // Use move semantics

    return lines;
}
std::vector<int> calculateLineHeights(const std::vector<std::string> &lines, const Emote *emotes,
                                      const std::vector<bool> &isHeaderVec)
{
    // Tunables for layout control
    constexpr int HEADER_UNDERLINE_GAP = 0; // space between underline and first body line
    constexpr int HEADER_UNDERLINE_PIX = 1; // underline thickness (1px row drawn)
    constexpr int BODY_LINE_LEADING = -4;   // default vertical leading for normal body lines
    constexpr int MESSAGE_BLOCK_GAP = 4;    // gap after a message block before a new header
    constexpr int EMOTE_PADDING_ABOVE = 4;  // space above emote line (added to line above)
    constexpr int EMOTE_PADDING_BELOW = 3;  // space below emote line (added to emote line)

    std::vector<int> rowHeights;
    rowHeights.reserve(lines.size());

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const auto &line = lines[idx];
        const int baseHeight = FONT_HEIGHT_SMALL;

        // Detect if THIS line or NEXT line contains an emote
        bool hasEmote = false;
        int tallestEmote = baseHeight;
        for (int i = 0; i < numEmotes; ++i) {
            if (line.find(emotes[i].label) != std::string::npos) {
                hasEmote = true;
                tallestEmote = std::max(tallestEmote, emotes[i].height);
            }
        }

        bool nextHasEmote = false;
        if (idx + 1 < lines.size()) {
            for (int i = 0; i < numEmotes; ++i) {
                if (lines[idx + 1].find(emotes[i].label) != std::string::npos) {
                    nextHasEmote = true;
                    break;
                }
            }
        }

        int lineHeight = baseHeight;

        if (isHeaderVec[idx]) {
            // Header line spacing
            lineHeight = baseHeight + HEADER_UNDERLINE_PIX + HEADER_UNDERLINE_GAP;
        } else {
            // Base spacing for normal lines
            int desiredBody = baseHeight + BODY_LINE_LEADING;

            if (hasEmote) {
                // Emote line: add overshoot + bottom padding
                int overshoot = std::max(0, tallestEmote - baseHeight);
                lineHeight = desiredBody + overshoot + EMOTE_PADDING_BELOW;
            } else {
                // Regular line: no emote → standard spacing
                lineHeight = desiredBody;

                // If next line has an emote → add top padding *here*
                if (nextHasEmote) {
                    lineHeight += EMOTE_PADDING_ABOVE;
                }
            }

            // Add block gap if next is a header
            if (idx + 1 < lines.size() && isHeaderVec[idx + 1]) {
                lineHeight += MESSAGE_BLOCK_GAP;
            }
        }

        rowHeights.push_back(lineHeight);
    }

    return rowHeights;
}

void renderMessageContent(OLEDDisplay *display, const std::vector<std::string> &lines, const std::vector<int> &rowHeights, int x,
                          int yOffset, int scrollBottom, const Emote *emotes, int numEmotes, bool isInverted, bool isBold)
{
    for (size_t i = 0; i < lines.size(); ++i) {
        int lineY = yOffset;
        for (size_t j = 0; j < i; ++j)
            lineY += rowHeights[j];
        if (lineY > -rowHeights[i] && lineY < (scrollBottom + rowHeights[i])) {
            if (i == 0 && isInverted) {
                display->drawString(x, lineY, lines[i].c_str());
                if (isBold)
                    display->drawString(x, lineY, lines[i].c_str());
            } else {
                drawStringWithEmotes(display, x, lineY, lines[i], emotes, numEmotes);
            }
        }
    }
}

void handleNewMessage(OLEDDisplay *display, const StoredMessage &sm, const meshtastic_MeshPacket &packet)
{
    if (packet.from != 0) {
        hasUnreadMessage = true;

        // Determine if message belongs to a muted channel
        bool isChannelMuted = false;
        if (sm.type == MessageType::BROADCAST) {
            const meshtastic_Channel channel = channels.getByIndex(packet.channel ? packet.channel : channels.getPrimaryIndex());
            if (channel.settings.has_module_settings && channel.settings.module_settings.is_muted)
                isChannelMuted = true;
        }

        // Banner logic
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(packet.from);
        char longName[48] = "???";
        if (node && node->user.long_name) {
            strncpy(longName, node->user.long_name, sizeof(longName) - 1);
            longName[sizeof(longName) - 1] = '\0';
        }
        int availWidth = display->getWidth() - (isHighResolution ? 40 : 20);
        if (availWidth < 0)
            availWidth = 0;

        size_t origLen = strlen(longName);
        while (longName[0] && display->getStringWidth(longName) > availWidth) {
            longName[strlen(longName) - 1] = '\0';
        }
        if (strlen(longName) < origLen) {
            strcat(longName, "...");
        }
        const char *msgRaw = reinterpret_cast<const char *>(packet.decoded.payload.bytes);

        char banner[256];
        bool isAlert = false;

        // Check if alert detection is enabled via external notification module
        if (moduleConfig.external_notification.alert_bell || moduleConfig.external_notification.alert_bell_vibra ||
            moduleConfig.external_notification.alert_bell_buzzer) {
            for (size_t i = 0; i < packet.decoded.payload.size && i < 100; i++) {
                if (msgRaw[i] == '\x07') {
                    isAlert = true;
                    break;
                }
            }
        }

        if (isAlert) {
            if (longName && longName[0])
                snprintf(banner, sizeof(banner), "Alert Received from\n%s", longName);
            else
                strcpy(banner, "Alert Received");
        } else {
            // Skip muted channels unless it's an alert
            if (isChannelMuted)
                return;

            if (longName && longName[0]) {
#if defined(M5STACK_UNITC6L)
                strcpy(banner, "New Message");
#else
                snprintf(banner, sizeof(banner), "New Message from\n%s", longName);
#endif
            } else
                strcpy(banner, "New Message");
        }

        // Append context (which channel or DM) so the banner shows where the message arrived
        {
            char contextBuf[64] = "";
            if (sm.type == MessageType::BROADCAST) {
                const char *cname = channels.getName(sm.channelIndex);
                if (cname && cname[0])
                    snprintf(contextBuf, sizeof(contextBuf), "in #%s", cname);
                else
                    snprintf(contextBuf, sizeof(contextBuf), "in Ch%d", sm.channelIndex);
            }

            if (contextBuf[0]) {
                size_t cur = strlen(banner);
                if (cur + 1 < sizeof(banner)) {
                    if (cur > 0 && banner[cur - 1] != '\n') {
                        banner[cur] = '\n';
                        banner[cur + 1] = '\0';
                        cur++;
                    }
                    strncat(banner, contextBuf, sizeof(banner) - cur - 1);
                }
            }
        }

        // Show banner for shorter time if currently viewing the messages screen,
        // longer time if on any other screen (regardless of which thread we were last viewing)
        bool onMessageScreen = graphics::isMessagesScreenActive();

        if (shouldWakeOnReceivedMessage()) {
            screen->setOn(true);
        }

        screen->showSimpleBanner(banner, onMessageScreen ? 1000 : 3000);
    }

    // Always focus into the correct conversation thread when a message with real text arrives
    const char *msgText = MessageStore::getText(sm);
    if (msgText && msgText[0] != '\0') {
        setThreadFor(sm, packet);
    }

    // Reset scroll for a clean start
    resetScrollState();
    markDirty();
}

void setThreadFor(const StoredMessage &sm, const meshtastic_MeshPacket &packet)
{
    if (packet.to == 0 || packet.to == NODENUM_BROADCAST) {
        setThreadMode(ThreadMode::CHANNEL, sm.channelIndex);
        markDirty();
    } else {
        uint32_t localNode = nodeDB->getNodeNum();
        uint32_t peer = (sm.sender == localNode) ? packet.to : sm.sender;
        setThreadMode(ThreadMode::DIRECT, -1, peer);
        markDirty();
    }
}

// -------- Manual scroll controls (UP/DOWN) ----------
static int computeMaxScroll()
{
    int totalHeight = 0;
    for (int h : cachedHeights)
        totalHeight += h;
    int usableScrollHeight = (lastUsableScrollHeight > 0) ? lastUsableScrollHeight : (FONT_HEIGHT_SMALL * 6);
    if (cachedHeights.empty())
        return 0;
    // bottom-aligned offset so newest sits one row above bottom
    int kBottomPadPx = FONT_HEIGHT_SMALL;
    int bottomOffsetOneRow = totalHeight - usableScrollHeight + kBottomPadPx;
    return std::max(0, bottomOffsetOneRow);
}

void scrollUp()
{
    manualScrollActive = true;
    lastManualMs = millis();
    waitingToReset = false;
    pauseStart = 0;
    scrollStarted = true;
    scrollY -= FONT_HEIGHT_SMALL;
    if (scrollY < 0)
        scrollY = 0;
    lastTime = millis();
    powerFSM.trigger(EVENT_PRESS);
    screen->forceDisplay(true);
    playChirp();
}

void scrollDown()
{
    manualScrollActive = true;
    lastManualMs = millis();
    waitingToReset = false;
    pauseStart = 0;
    scrollStarted = true;
    scrollY += FONT_HEIGHT_SMALL;
    int maxScroll = computeMaxScroll();
    if (scrollY > maxScroll)
        scrollY = maxScroll;
    lastTime = millis();
    powerFSM.trigger(EVENT_PRESS);
    screen->forceDisplay(true);
    playChirp();
}

} // namespace MessageRenderer
} // namespace graphics
#endif