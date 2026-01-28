#include "configuration.h"
#if HAS_SCREEN
#include "MessageRenderer.h"

// Core includes
#include "FSCommon.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "UIRenderer.h"
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

// External declarations
extern bool hasUnreadMessage;
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
static bool manualScrolling = false;

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

// Remove variation selectors (FE0F), skin tone modifiers, and keycap modifiers from emoji so they match your labels
static std::string normalizeEmoji(const std::string &s)
{
    std::string out;
    out.reserve(s.size()); // Pre-allocate to avoid reallocations

    for (size_t i = 0; i < s.size();) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        size_t len = utf8CharLen(c);

        // Skip variation selector (U+FE0F): EF B8 8F
        if (c == 0xEF && i + 2 < s.size() && (uint8_t)s[i + 1] == 0xB8 && (uint8_t)s[i + 2] == 0x8F) {
            i += 3;
            continue;
        }

        // Skip combining enclosing keycap (U+20E3): E2 83 A3
        // This converts keycap emoji like 1️⃣ to just "1" by stripping the keycap modifier
        if (c == 0xE2 && i + 2 < s.size() && (uint8_t)s[i + 1] == 0x83 && (uint8_t)s[i + 2] == 0xA3) {
            i += 3;
            continue;
        }

        // Skip Zero Width Joiner (U+200D): E2 80 8D
        // Also skip the following gender/modifier symbol if present
        if (c == 0xE2 && i + 2 < s.size() && (uint8_t)s[i + 1] == 0x80 && (uint8_t)s[i + 2] == 0x8D) {
            i += 3;
            // Skip the next character which is typically a gender symbol (♂/♀) or other modifier
            if (i < s.size()) {
                uint8_t next = static_cast<uint8_t>(s[i]);
                size_t nextLen = utf8CharLen(next);
                i += nextLen;
                // Also skip variation selector after the gender symbol if present
                if (i + 2 < s.size() && (uint8_t)s[i] == 0xEF && (uint8_t)s[i + 1] == 0xB8 && (uint8_t)s[i + 2] == 0x8F) {
                    i += 3;
                }
            }
            continue;
        }

        // Skip skin tone modifiers (U+1F3FB-U+1F3FF): F0 9F 8F BB-BF
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

// Cache normalized emoji labels to avoid repeated allocations
static std::vector<std::string> cachedNormalizedEmoteLabels;
static bool emoteLabelsCached = false;

static void ensureEmoteLabelsNormalized()
{
    if (!emoteLabelsCached) {
        cachedNormalizedEmoteLabels.clear();
        cachedNormalizedEmoteLabels.reserve(numEmotes);
        for (int i = 0; i < numEmotes; ++i) {
            cachedNormalizedEmoteLabels.push_back(normalizeEmoji(std::string(emotes[i].label)));
        }
        emoteLabelsCached = true;
    }
}

// Replace unknown 4-byte emoji with upside-down question mark
static std::string replaceUnknownEmoji(const std::string &s, const Emote *emoteList, int emoteCount)
{
    ensureEmoteLabelsNormalized(); // Ensure cache is ready

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
            const std::string &labelNorm = cachedNormalizedEmoteLabels[e];
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
            // E2 80 XX = General Punctuation (U+2000-U+206F)
            if (b2 == 0x80 && i + 2 < normInput.size()) {
                uint8_t b3 = static_cast<uint8_t>(normInput[i + 2]);
                // E2 80 8D = Zero Width Joiner (U+200D) - part of composite emoji
                if (b3 == 0x8D) {
                    isUnknownEmoji = true;
                }
                // Other E2 80 XX are punctuation (', ", –, —) - keep as-is
            }
            // E2 86-97 XX = Arrows, Math, Technical (U+2190-U+27BF) - treat as emoji
            // E2 98-9B XX = Miscellaneous Symbols (U+2600-U+26FF) - includes ⚡☀️❤️ - emoji!
            // E2 9C-9F XX = Dingbats (U+2700-U+27BF) - emoji!
            else if (b2 >= 0x86 && b2 <= 0x9F) {
                isUnknownEmoji = true; // Arrows, symbols, dingbats
            }
        } else if (charLen == 3 && c == 0xE3) {
            // 3-byte emoji starting with 0xE3 (U+3000-U+3FFF range)
            // Includes CJK Symbols and some emoji-like characters
            isUnknownEmoji = true;
        } else if (charLen == 3 && c == 0xEF && i + 1 < normInput.size()) {
            // 3-byte sequences starting with 0xEF - check for variation selectors
            uint8_t b2 = static_cast<uint8_t>(normInput[i + 1]);
            uint8_t b3 = (i + 2 < normInput.size()) ? static_cast<uint8_t>(normInput[i + 2]) : 0;
            // EF B8 8F = Variation Selector-16 (U+FE0F) - emoji presentation
            if (b2 == 0xB8 && b3 == 0x8F) {
                isUnknownEmoji = true;
            }
        }

        if (isUnknownEmoji) {
            // Replace unknown emoji with upside-down question mark (¿)
            out.append("¿");
            i += charLen;
        } else {
            // Keep normal characters as-is
            out.append(normInput, i, charLen);
            i += charLen;
        }
    }

    return out;
}

// UTF-8 character count (exported)
size_t utf8CharCount(const char *str)
{
    size_t count = 0;
    for (size_t i = 0; str[i];) {
        size_t len = utf8CharLen((uint8_t)str[i]);
        i += len;
        count++;
    }
    return count;
}

// UTF-8 substring (exported)
std::string utf8Substr(const std::string &str, size_t maxChars)
{
    std::string result;
    size_t count = 0;
    for (size_t i = 0; i < str.size() && count < maxChars;) {
        size_t len = utf8CharLen((uint8_t)str[i]);
        result.append(str, i, len);
        i += len;
        count++;
    }
    return result;
}

// Scroll state (file scope so we can reset on new message)
float scrollY = 0.0f;
static int scrollMin = 0; // Minimum scroll to prevent bubble overlap with header
uint32_t lastTime = 0;
uint32_t scrollStartDelay = 0;
uint32_t pauseStart = 0;
bool waitingToReset = false;
bool scrollStarted = false;
static bool didReset = false;
static bool didLoadPreference = false;
static bool needsInitialScrollPosition = false; // Set after cache rebuild to position scroll for reversed order
static constexpr int MESSAGE_BLOCK_GAP = 6;

// Message order preference (default: newest first/on top)
static bool messageOrderNewestFirst = true;

#ifdef FSCom
static const char *messageOrderFileName = "/prefs/message_order.dat";
#endif

// Load message order preference from filesystem
static void loadMessageOrderPreference()
{
#ifdef FSCom
    auto file = FSCom.open(messageOrderFileName, FILE_O_READ);
    if (file) {
        uint8_t value = 0;
        if (file.read(&value, 1) == 1) {
            messageOrderNewestFirst = (value != 0);
            LOG_DEBUG("Loaded message order preference: %s", messageOrderNewestFirst ? "new on top" : "old on top");
        }
        file.close();
    }
#endif
}

// Save message order preference to filesystem
static void saveMessageOrderPreference()
{
#ifdef FSCom
    FSCom.mkdir("/prefs"); // Ensure directory exists
    // Remove old file to ensure clean write
    if (FSCom.exists(messageOrderFileName)) {
        FSCom.remove(messageOrderFileName);
    }
    auto file = FSCom.open(messageOrderFileName, FILE_O_WRITE);
    if (file) {
        uint8_t value = messageOrderNewestFirst ? 1 : 0;
        file.write(&value, 1);
        file.flush();
        file.close();
        LOG_INFO("Saved message order preference: %s", messageOrderNewestFirst ? "new on top" : "old on top");
    }
#endif
}

bool getMessageOrderNewestFirst()
{
    return messageOrderNewestFirst;
}

void setMessageOrderNewestFirst(bool newestFirst)
{
    if (messageOrderNewestFirst != newestFirst) {
        messageOrderNewestFirst = newestFirst;
        saveMessageOrderPreference();
        // Clear cache and reset scroll when ordering changes
        resetScrollState();
        cachedLines.clear();
        cachedHeights.clear();
        // Flag to set initial scroll position after cache is rebuilt
        needsInitialScrollPosition = true;
    }
}

// Adjust scroll for touch/drag gestures
void adjustScroll(int16_t deltaY)
{
    manualScrolling = true;
    scrollY -= deltaY;
    if (scrollY < scrollMin)
        scrollY = scrollMin;

    int totalHeight = 0;
    for (int h : cachedHeights)
        totalHeight += h;
    int visibleHeight = screen ? (screen->getHeight() - (FONT_HEIGHT_SMALL * 2)) : 64;
    int maxScroll = totalHeight - visibleHeight;
    if (maxScroll < 0)
        maxScroll = 0;
    if (scrollY > maxScroll)
        scrollY = maxScroll;
}

void scrollUp()
{
    manualScrolling = true;
    scrollY -= 12;
    if (scrollY < scrollMin)
        scrollY = scrollMin;
}

void scrollDown()
{
    manualScrolling = true;

    int totalHeight = 0;
    for (int h : cachedHeights)
        totalHeight += h;

    int visibleHeight = screen->getHeight() - (FONT_HEIGHT_SMALL * 2);
    int maxScroll = totalHeight - visibleHeight;
    if (maxScroll < 0)
        maxScroll = 0;

    scrollY += 12;
    if (scrollY > maxScroll)
        scrollY = maxScroll;
}

void drawStringWithEmotes(OLEDDisplay *display, int x, int y, const std::string &line, const Emote *emoteList, int emoteCount,
                          bool isMessageHeader)
{
    (void)isMessageHeader;         // Not used in this implementation
    ensureEmoteLabelsNormalized(); // Ensure cache is ready

    // Normalize and replace unknown emoji with placeholder so they don't render as blank
    const std::string normLine = replaceUnknownEmoji(line, emoteList, emoteCount);
    int cursorX = x;
    const int fontHeight = FONT_HEIGHT_SMALL;

    // Step 1: Find tallest emote in the line
    int maxIconHeight = fontHeight;
    for (size_t i = 0; i < normLine.length();) {
        bool matched = false;
        for (int e = 0; e < emoteCount; ++e) {
            // Compare against cached normalized label
            const std::string &labelNorm = cachedNormalizedEmoteLabels[e];
            const size_t emojiLen = labelNorm.length();
            if (emojiLen && normLine.compare(i, emojiLen, labelNorm) == 0) {
                if (emoteList[e].height > maxIconHeight)
                    maxIconHeight = emoteList[e].height;
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

        // Look ahead for the next emote match using normalized labels
        size_t nextEmotePos = std::string::npos;
        const Emote *matchedEmote = nullptr;
        size_t emojiLen = 0;

        for (int e = 0; e < emoteCount; ++e) {
            const std::string &labelNorm = cachedNormalizedEmoteLabels[e];
            size_t pos = normLine.find(labelNorm, i);
            if (pos != std::string::npos && (nextEmotePos == std::string::npos || pos < nextEmotePos)) {
                nextEmotePos = pos;
                matchedEmote = &emoteList[e];
                emojiLen = labelNorm.length();
            }
        }

        // Render normal text segment up to the emote or bold toggle
        size_t nextControl = std::min(nextEmotePos, normLine.find("**", i));
        if (nextControl == std::string::npos)
            nextControl = normLine.length();

        if (nextControl > i) {
            std::string textChunk = normLine.substr(i, nextControl - i);
            if (inBold) {
                // Faux bold: draw twice, offset by 1px
                display->drawString(cursorX + 1, fontY, textChunk.c_str());
            }
            display->drawString(cursorX, fontY, textChunk.c_str());
#if defined(OLED_UA) || defined(OLED_RU)
            cursorX += display->getStringWidth(textChunk.c_str(), textChunk.length(), true);
#else
            cursorX += display->getStringWidth(textChunk.c_str());
#endif
            i = nextControl;
            continue;
        }

        // Render the emote (if found)
        if (matchedEmote && i == nextEmotePos) {
            int iconY = y + (lineHeight - matchedEmote->height) / 2;
            display->drawXbm(cursorX, iconY, matchedEmote->width, matchedEmote->height, matchedEmote->bitmap);
            cursorX += matchedEmote->width + 1;
            i += emojiLen;
            continue;
        } else {
            // No more emotes — render the rest of the line
            std::string remaining = normLine.substr(i);
            if (inBold) {
                display->drawString(cursorX + 1, fontY, remaining.c_str());
            }
            display->drawString(cursorX, fontY, remaining.c_str());
#if defined(OLED_UA) || defined(OLED_RU)
            cursorX += display->getStringWidth(remaining.c_str(), remaining.length(), true);
#else
            cursorX += display->getStringWidth(remaining.c_str());
#endif
            break;
        }
    }
}

// Reset scroll state when new messages arrive
void resetScrollState()
{
    scrollY = 0.0f;
    scrollStarted = false;
    waitingToReset = false;
    scrollStartDelay = millis();
    lastTime = millis();
    manualScrolling = false;
    didReset = false;
    // Flag to reposition scroll for both modes (uses scrollMin for new-on-top, scrollStop for old-on-top)
    needsInitialScrollPosition = true;
}

void nudgeScroll(int8_t direction)
{
    if (direction == 0)
        return;

    if (cachedHeights.empty()) {
        scrollY = 0.0f;
        return;
    }

    OLEDDisplay *display = (screen != nullptr) ? screen->getDisplayDevice() : nullptr;
    const int displayHeight = display ? display->getHeight() : 64;
    const int navHeight = FONT_HEIGHT_SMALL;
    const int usableHeight = std::max(0, displayHeight - navHeight);

    int totalHeight = 0;
    for (int h : cachedHeights)
        totalHeight += h;

    if (totalHeight <= usableHeight) {
        scrollY = 0.0f;
        return;
    }

    const int scrollStop = std::max(0, totalHeight - usableHeight + cachedHeights.back());
    const int step = std::max(FONT_HEIGHT_SMALL, usableHeight / 3);

    float newScroll = scrollY + static_cast<float>(direction) * static_cast<float>(step);
    if (newScroll < 0.0f)
        newScroll = 0.0f;
    if (newScroll > scrollStop)
        newScroll = static_cast<float>(scrollStop);

    if (newScroll != scrollY) {
        scrollY = newScroll;
        waitingToReset = false;
        scrollStarted = false;
        scrollStartDelay = millis();
        lastTime = millis();
    }
}

// Fully free cached message data from heap
void clearMessageCache()
{
    std::vector<std::string>().swap(cachedLines);
    std::vector<int>().swap(cachedHeights);

    // Reset scroll so we rebuild cleanly next time we enter the screen
    resetScrollState();
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
    didReset = false; // force reset when mode changes

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

static inline int getRenderedLineWidth(OLEDDisplay *display, const std::string &line, const Emote *emoteList, int emoteCount)
{
    ensureEmoteLabelsNormalized(); // Ensure cache is ready

    // Use replaceUnknownEmoji to match how text is actually rendered
    // This ensures width calculation is consistent with drawStringWithEmotes
    std::string normalized = replaceUnknownEmoji(line, emoteList, emoteCount);
    int totalWidth = 0;

    size_t i = 0;
    while (i < normalized.length()) {
        bool matched = false;
        for (int e = 0; e < emoteCount; ++e) {
            const std::string &labelNorm = cachedNormalizedEmoteLabels[e];
            size_t emojiLen = labelNorm.length();
            if (emojiLen > 0 && normalized.compare(i, emojiLen, labelNorm) == 0) {
                totalWidth += emoteList[e].width + 1; // +1 spacing
                i += emojiLen;
                matched = true;
                break;
            }
        }
        if (!matched) {
            size_t charLen = utf8CharLen(static_cast<uint8_t>(normalized[i]));
#if defined(OLED_UA) || defined(OLED_RU)
            totalWidth += display->getStringWidth(normalized.substr(i, charLen).c_str(), charLen, true);
#else
            totalWidth += display->getStringWidth(normalized.substr(i, charLen).c_str());
#endif
            i += charLen;
        }
    }
    return totalWidth;
}

// Public wrapper for getRenderedLineWidth (used by other modules)
int getStringWidthWithEmotes(OLEDDisplay *display, const std::string &line, const Emote *emotes, int emoteCount)
{
    return getRenderedLineWidth(display, line, emotes, emoteCount);
}

struct MessageBlock {
    size_t start;
    size_t end;
    bool mine;
};

static int getDrawnLinePixelBottom(int lineTopY, const std::string &line, bool isHeaderLine)
{
    if (isHeaderLine) {
        return lineTopY + (FONT_HEIGHT_SMALL - 1);
    }

    int tallest = FONT_HEIGHT_SMALL;
    for (int e = 0; e < numEmotes; ++e) {
        if (line.find(emotes[e].label) != std::string::npos) {
            if (emotes[e].height > tallest)
                tallest = emotes[e].height;
        }
    }

    const int lineHeight = std::max(FONT_HEIGHT_SMALL, tallest);
    const int iconTop = lineTopY + (lineHeight - tallest) / 2;

    return iconTop + tallest - 1;
}

static std::vector<MessageBlock> buildMessageBlocks(const std::vector<bool> &isHeaderVec, const std::vector<bool> &isMineVec)
{
    std::vector<MessageBlock> blocks;
    if (isHeaderVec.empty())
        return blocks;

    size_t start = 0;
    bool mine = isMineVec[0];

    for (size_t i = 1; i < isHeaderVec.size(); ++i) {
        if (isHeaderVec[i]) {
            MessageBlock b;
            b.start = start;
            b.end = i - 1;
            b.mine = mine;
            blocks.push_back(b);

            start = i;
            mine = isMineVec[i];
        }
    }

    MessageBlock last;
    last.start = start;
    last.end = isHeaderVec.size() - 1;
    last.mine = mine;
    blocks.push_back(last);

    return blocks;
}

static void drawMessageScrollbar(OLEDDisplay *display, int visibleHeight, int totalHeight, int scrollOffset, int startY)
{
    if (totalHeight <= visibleHeight)
        return; // no scrollbar needed

    int scrollbarX = display->getWidth() - 2;
    int scrollbarHeight = visibleHeight;
    int thumbHeight = std::max(6, (scrollbarHeight * visibleHeight) / totalHeight);
    int maxScroll = std::max(1, totalHeight - visibleHeight);
    int thumbY = startY + (scrollbarHeight - thumbHeight) * scrollOffset / maxScroll;

    for (int i = 0; i < thumbHeight; i++) {
        display->setPixel(scrollbarX, thumbY + i);
    }
}

void drawTextMessageFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    // Ensure any boot-relative timestamps are upgraded if RTC is valid
    messageStore.upgradeBootRelativeTimestamps();

    // Load message order preference from filesystem (once)
    if (!didLoadPreference) {
        loadMessageOrderPreference();
        didLoadPreference = true;
        // If loaded as reversed (old on top), flag to set initial scroll position
        if (!messageOrderNewestFirst) {
            needsInitialScrollPosition = true;
        }
    }

    if (!didReset) {
        resetScrollState();
        didReset = true;
    }

    // Clear the unread message indicator when viewing the message
    hasUnreadMessage = false;

    // Filter messages based on thread mode
    std::deque<StoredMessage> filtered;
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
            filtered.push_back(m);
    }

    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    const int navHeight = FONT_HEIGHT_SMALL;
    const int scrollBottom = SCREEN_HEIGHT - navHeight;
    const int usableHeight = scrollBottom;
    constexpr int LEFT_MARGIN = 2;
    constexpr int RIGHT_MARGIN = 2;
    constexpr int SCROLLBAR_WIDTH = 3;
    constexpr int BUBBLE_PAD_X = 3;
    constexpr int BUBBLE_PAD_Y = 4;
    constexpr int BUBBLE_RADIUS = 4;
    constexpr int BUBBLE_MIN_W = 24;
    constexpr int BUBBLE_TEXT_INDENT = 2;

    // Derived widths
    const int leftTextWidth = SCREEN_WIDTH - LEFT_MARGIN - RIGHT_MARGIN - (BUBBLE_PAD_X * 2);
    const int rightTextWidth = SCREEN_WIDTH - LEFT_MARGIN - RIGHT_MARGIN - SCROLLBAR_WIDTH;

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
            return; // Next draw will rerun in ALL mode
        }

        // Still in ALL mode and no messages at all → show placeholder
        graphics::drawCommonHeader(display, x, y, titleStr);
        didReset = false;
        const char *messageString = "No messages";
        int center_text = (SCREEN_WIDTH / 2) - (display->getStringWidth(messageString) / 2);
        display->drawString(center_text, getTextPositions(display)[2], messageString);
        graphics::drawCommonFooter(display, x, y);
        return;
    }

    // Build lines for filtered messages
    // Order depends on messageOrderNewestFirst setting:
    // - true (default): newest first (iterate reverse, newest message at top)
    // - false: oldest first (iterate forward, oldest message at top)
    std::vector<std::string> allLines;
    std::vector<bool> isMine;   // track alignment
    std::vector<bool> isHeader; // track header lines
    std::vector<AckStatus> ackForLine;

    // Lambda to process a single message (avoids duplicating the code)
    auto processMessage = [&](const StoredMessage &m) {
        // Channel / destination labeling
        char chanType[32] = "";
        if (currentMode == ThreadMode::ALL) {
            if (m.dest == NODENUM_BROADCAST) {
                const char *name = channels.getName(m.channelIndex);
                if (currentResolution == ScreenResolution::Low || currentResolution == ScreenResolution::UltraLow) {
                    if (strcmp(name, "ShortTurbo") == 0)
                        name = "ShortT";
                    else if (strcmp(name, "ShortSlow") == 0)
                        name = "ShortS";
                    else if (strcmp(name, "ShortFast") == 0)
                        name = "ShortF";
                    else if (strcmp(name, "MediumSlow") == 0)
                        name = "MedS";
                    else if (strcmp(name, "MediumFast") == 0)
                        name = "MedF";
                    else if (strcmp(name, "LongSlow") == 0)
                        name = "LongS";
                    else if (strcmp(name, "LongFast") == 0)
                        name = "LongF";
                    else if (strcmp(name, "LongTurbo") == 0)
                        name = "LongT";
                    else if (strcmp(name, "LongMod") == 0)
                        name = "LongM";
                }
                snprintf(chanType, sizeof(chanType), "#%s", name);
            } else {
                snprintf(chanType, sizeof(chanType), "(DM)");
            }
        }

        // Calculate how long ago
        uint32_t nowSecs = getValidTime(RTCQuality::RTCQualityDevice, true);
        uint32_t seconds = 0;
        bool invalidTime = true;

        if (m.timestamp > 0 && nowSecs > 0) {
            if (nowSecs >= m.timestamp) {
                seconds = nowSecs - m.timestamp;
                invalidTime = (seconds > 315360000); // >10 years
            } else {
                uint32_t ahead = m.timestamp - nowSecs;
                if (ahead <= 600) { // allow small skew
                    seconds = 0;
                    invalidTime = false;
                }
            }
        } else if (m.timestamp > 0 && nowSecs == 0) {
            // RTC not valid: only trust boot-relative if same boot
            uint32_t bootNow = millis() / 1000;
            if (m.isBootRelative && m.timestamp <= bootNow) {
                seconds = bootNow - m.timestamp;
                invalidTime = false;
            } else {
                invalidTime = true; // old persisted boot-relative, ignore until healed
            }
        }

        char timeBuf[16];
        if (invalidTime) {
            snprintf(timeBuf, sizeof(timeBuf), "???");
        } else if (seconds < 60) {
            snprintf(timeBuf, sizeof(timeBuf), "%us", seconds);
        } else if (seconds < 3600) {
            snprintf(timeBuf, sizeof(timeBuf), "%um", seconds / 60);
        } else if (seconds < 86400) {
            snprintf(timeBuf, sizeof(timeBuf), "%uh", seconds / 3600);
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "%ud", seconds / 86400);
        }

        // Build header line for this message
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(m.sender);
        meshtastic_NodeInfoLite *node_recipient = nodeDB->getMeshNode(m.dest);

        char senderBuf[48] = "";
        if (node && node->has_user) {
            // Respect the same long/short name setting as NodeListRenderer
            const char *preferred = config.display.use_long_node_name ? node->user.long_name : node->user.short_name;
            const char *fallback = config.display.use_long_node_name ? node->user.short_name : node->user.long_name;
            const char *nameToUse = (preferred && preferred[0]) ? preferred : ((fallback && fallback[0]) ? fallback : nullptr);
            if (nameToUse) {
                strncpy(senderBuf, nameToUse, sizeof(senderBuf) - 1);
                senderBuf[sizeof(senderBuf) - 1] = '\0';
            }
        }
        if (senderBuf[0] == '\0') {
            // No name available → show NodeID in parentheses
            snprintf(senderBuf, sizeof(senderBuf), "(%08x)", m.sender);
        }

        // If this is *our own* message, override senderBuf to who the recipient was
        bool mine = (m.sender == nodeDB->getNodeNum());
        if (mine && node_recipient && node_recipient->has_user) {
            const char *preferred =
                config.display.use_long_node_name ? node_recipient->user.long_name : node_recipient->user.short_name;
            const char *fallback =
                config.display.use_long_node_name ? node_recipient->user.short_name : node_recipient->user.long_name;
            const char *nameToUse = (preferred && preferred[0]) ? preferred : ((fallback && fallback[0]) ? fallback : nullptr);
            if (nameToUse) {
                strncpy(senderBuf, nameToUse, sizeof(senderBuf) - 1);
                senderBuf[sizeof(senderBuf) - 1] = '\0';
            } else {
                snprintf(senderBuf, sizeof(senderBuf), "(%08x)", m.dest);
            }
        }

        // Shrink Sender name if needed
        int availWidth = (mine ? rightTextWidth : leftTextWidth) - display->getStringWidth(timeBuf) -
                         display->getStringWidth(chanType) - display->getStringWidth("   @...");
        if (availWidth < 0)
            availWidth = 0;

        size_t origLen = strlen(senderBuf);
        while (senderBuf[0] && display->getStringWidth(senderBuf) > availWidth) {
            senderBuf[strlen(senderBuf) - 1] = '\0';
        }

        // If we actually truncated, append "..."
        if (strlen(senderBuf) < origLen) {
            strcat(senderBuf, "...");
        }

        // Final header line
        char headerStr[96];
        if (mine) {
            if (currentMode == ThreadMode::ALL) {
                if (strcmp(chanType, "(DM)") == 0) {
                    snprintf(headerStr, sizeof(headerStr), "%s to %s", timeBuf, senderBuf);
                } else {
                    snprintf(headerStr, sizeof(headerStr), "%s to %s", timeBuf, chanType);
                }
            } else {
                snprintf(headerStr, sizeof(headerStr), "%s", timeBuf);
            }
        } else {
            snprintf(headerStr, sizeof(headerStr), "%s @%s %s", timeBuf, senderBuf, chanType);
        }

        // Push header line
        allLines.push_back(std::string(headerStr));
        isMine.push_back(mine);
        isHeader.push_back(true);
        ackForLine.push_back(m.ackStatus);

        const char *msgText = MessageStore::getText(m);

        int wrapWidth = mine ? rightTextWidth : leftTextWidth;
        std::vector<std::string> wrapped = generateLines(display, "", msgText, wrapWidth);
        for (auto &ln : wrapped) {
            allLines.push_back(ln);
            isMine.push_back(mine);
            isHeader.push_back(false);
            ackForLine.push_back(AckStatus::NONE);
        }
    };

    // Iterate based on ordering preference
    if (messageOrderNewestFirst) {
        // New on top: iterate REVERSE (messages stored oldest-first, so reverse = newest first)
        for (auto it = filtered.rbegin(); it != filtered.rend(); ++it) {
            processMessage(*it);
        }
    } else {
        // Old on top: iterate FORWARD (messages stored oldest-first, so forward = oldest first)
        for (auto it = filtered.begin(); it != filtered.end(); ++it) {
            processMessage(*it);
        }
    }

    // Cache lines and heights
    cachedLines = allLines;
    cachedHeights = calculateLineHeights(cachedLines, emotes, isHeader);

    std::vector<MessageBlock> blocks = buildMessageBlocks(isHeader, isMine);

    // Scrolling logic - direction depends on message ordering
    int totalHeight = 0;
    for (size_t i = 0; i < cachedHeights.size(); ++i)
        totalHeight += cachedHeights[i];
    int usableScrollHeight = usableHeight;
    int scrollStop = std::max(0, totalHeight - usableScrollHeight + (cachedHeights.empty() ? 0 : cachedHeights.back()));

    // Calculate minimum scroll to prevent bubble from overlapping header
    // When scrollY = scrollMin, first bubble top should be at contentTop + 1
    // yOffset = -scrollY + contentTop, first line at yOffset
    // First bubble top = yOffset - BUBBLE_PAD (either 1 or 2)
    // We want: yOffset - BUBBLE_PAD >= contentTop + 1
    // So: -scrollY + contentTop - BUBBLE_PAD >= contentTop + 1
    // Therefore: -scrollY >= BUBBLE_PAD + 1, scrollY <= -(BUBBLE_PAD + 1)
    // But scrollY increases downward, so we want scrollY >= -(BUBBLE_PAD + 1) to prevent going too high
    // Actually simpler: scrollMin = -(maxBubblePadTop + 1)
    constexpr int maxBubblePadTop = 2; // Max of BUBBLE_PAD_TOP_HEADER and BUBBLE_PAD_Y
    scrollMin = -(maxBubblePadTop + 1);

    // Set initial scroll position for reversed order (old on top = start at bottom)
    if (needsInitialScrollPosition) {
        if (!messageOrderNewestFirst && totalHeight > usableScrollHeight) {
            scrollY = scrollStop; // Start at bottom for "old on top"
        } else {
            scrollY = scrollMin; // Start at top with proper padding
        }
        needsInitialScrollPosition = false;
        scrollStartDelay = millis();
        scrollStarted = false;
        waitingToReset = false;
    }

#ifndef USE_EINK
    uint32_t now = millis();
    float delta = (now - lastTime) / 400.0f;
    lastTime = now;
    const float scrollSpeed = 2.0f;

    if (scrollStartDelay == 0)
        scrollStartDelay = now;
    if (!scrollStarted && now - scrollStartDelay > 2000)
        scrollStarted = true;

    if (!manualScrolling && totalHeight > usableScrollHeight) {
        if (scrollStarted) {
            if (messageOrderNewestFirst) {
                // New on top: start at top, scroll DOWN
                if (!waitingToReset) {
                    scrollY += delta * scrollSpeed;
                    if (scrollY >= scrollStop) {
                        scrollY = scrollStop;
                        waitingToReset = true;
                        pauseStart = lastTime;
                    }
                } else if (lastTime - pauseStart > 3000) {
                    scrollY = scrollMin;
                    waitingToReset = false;
                    scrollStarted = false;
                    scrollStartDelay = lastTime;
                }
            } else {
                // Old on top: start at bottom, scroll UP
                if (!waitingToReset) {
                    scrollY -= delta * scrollSpeed;
                    if (scrollY <= scrollMin) {
                        scrollY = scrollMin;
                        waitingToReset = true;
                        pauseStart = lastTime;
                    }
                } else if (lastTime - pauseStart > 3000) {
                    scrollY = scrollStop;
                    waitingToReset = false;
                    scrollStarted = false;
                    scrollStartDelay = lastTime;
                }
            }
        }
    } else if (!manualScrolling) {
        // Content fits on screen - no scrolling needed
        if (messageOrderNewestFirst) {
            scrollY = scrollMin;
        } else {
            scrollY = scrollMin; // Even with old-on-top, no scroll needed if content fits
        }
    }
#else
    // E-Ink: disable autoscroll
    scrollY = 0.0f;
    waitingToReset = false;
    scrollStarted = false;
    lastTime = millis();
#endif

    int finalScroll = (int)scrollY;
    int yOffset = -finalScroll + getTextPositions(display)[1];
    const int contentTop = getTextPositions(display)[1];
    const int contentBottom = scrollBottom; // already excludes nav line
    // Pre-render margin: render content slightly beyond visible area for smooth scroll-in
    constexpr int RENDER_MARGIN = 40; // pixels beyond visible area to pre-render
    const int renderBottom = contentBottom + RENDER_MARGIN;
    const int rightEdge = SCREEN_WIDTH - SCROLLBAR_WIDTH - RIGHT_MARGIN;
    const int bubbleGapY = std::max(1, MESSAGE_BLOCK_GAP / 2);

    std::vector<int> lineTop;
    lineTop.resize(cachedLines.size());
    {
        int acc = 0;
        for (size_t i = 0; i < cachedLines.size(); ++i) {
            lineTop[i] = yOffset + acc;
            acc += cachedHeights[i];
        }
    }

    // Draw bubbles
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const auto &b = blocks[bi];
        if (b.start >= cachedLines.size() || b.end >= cachedLines.size() || b.start > b.end)
            continue;

        int visualTop = lineTop[b.start];

        int topY;
        if (isHeader[b.start]) {
            // Header start
            constexpr int BUBBLE_PAD_TOP_HEADER = 1; // try 1 or 2
            topY = visualTop - BUBBLE_PAD_TOP_HEADER;
        } else {
            // Body start
            bool thisLineHasEmote = false;
            for (int e = 0; e < numEmotes; ++e) {
                if (cachedLines[b.start].find(emotes[e].label) != std::string::npos) {
                    thisLineHasEmote = true;
                    break;
                }
            }
            if (thisLineHasEmote) {
                constexpr int EMOTE_PADDING_ABOVE = 4;
                visualTop -= EMOTE_PADDING_ABOVE;
            }
            topY = visualTop - BUBBLE_PAD_Y;
        }

        int visualBottom = getDrawnLinePixelBottom(lineTop[b.end], cachedLines[b.end], isHeader[b.end]);
        int bottomY = visualBottom + BUBBLE_PAD_Y;

        if (bi + 1 < blocks.size()) {
            int nextHeaderIndex = (int)blocks[bi + 1].start;
            int nextTop = lineTop[nextHeaderIndex];
            int maxBottom = nextTop - 1 - bubbleGapY;
            if (bottomY > maxBottom)
                bottomY = maxBottom;
        }

        if (bottomY <= topY + 2)
            continue;

        // Skip bubbles entirely outside render area (includes margin for smooth scroll-in)
        if (bottomY < contentTop || topY > renderBottom)
            continue;

        int maxLineW = 0;

        for (size_t i = b.start; i <= b.end; ++i) {
            int w = 0;
            if (isHeader[i]) {
                // Use getRenderedLineWidth to account for emoji widths in sender names
                w = getRenderedLineWidth(display, cachedLines[i], emotes, numEmotes);
                if (b.mine)
                    w += 12; // room for ACK/NACK/relay mark
            } else {
                w = getRenderedLineWidth(display, cachedLines[i], emotes, numEmotes);
            }
            if (w > maxLineW)
                maxLineW = w;
        }

        int bubbleW = std::max(BUBBLE_MIN_W, maxLineW + (BUBBLE_PAD_X * 2));
        int bubbleH = (bottomY - topY) + 1;
        int bubbleX = 0;
        if (b.mine) {
            bubbleX = rightEdge - bubbleW;
        } else {
            bubbleX = x;
        }
        if (bubbleX < x)
            bubbleX = x;
        if (bubbleX + bubbleW > rightEdge)
            bubbleW = std::max(1, rightEdge - bubbleX);

        if (bubbleW > 1 && bubbleH > 1) {
            int x1 = bubbleX + bubbleW - 1;
            int y1 = topY + bubbleH - 1;

            if (b.mine) {
                // Send Message (Right side)
                display->drawRect(x1 + 2 - bubbleW, y1 - bubbleH, bubbleW, bubbleH);
                // Top Right Corner
                display->drawRect(x1, topY, 2, 1);
                display->drawRect(x1, topY, 1, 2);
                // Bottom Right Corner
                display->drawRect(x1 - 1, bottomY - 2, 2, 1);
                display->drawRect(x1, bottomY - 3, 1, 2);
                // Knock the corners off to make a bubble
                display->setColor(BLACK);
                display->drawRect(x1 - bubbleW, topY - 1, 1, 1);
                display->drawRect(x1 - bubbleW, bottomY - 1, 1, 1);
                display->setColor(WHITE);
            } else {
                // Received Message (Left Side)
                display->drawRect(bubbleX, topY, bubbleW + 1, bubbleH);
                // Top Left Corner
                display->drawRect(bubbleX + 1, topY + 1, 2, 1);
                display->drawRect(bubbleX + 1, topY + 1, 1, 2);
                // Bottom Left Corner
                display->drawRect(bubbleX + 1, bottomY - 1, 2, 1);
                display->drawRect(bubbleX + 1, bottomY - 2, 1, 2);
                // Knock the corners off to make a bubble
                display->setColor(BLACK);
                display->drawRect(bubbleX + bubbleW, topY, 1, 1);
                display->drawRect(bubbleX + bubbleW, bottomY, 1, 1);
                display->setColor(WHITE);
            }
        }
    }

    // Render visible lines
    int lineY = yOffset;
    for (size_t i = 0; i < cachedLines.size(); ++i) {
        // Only skip lines that are completely outside the render area (includes margin for smooth scroll-in)
        int lineBottom = lineY + cachedHeights[i];
        bool lineVisible = (lineBottom > contentTop) && (lineY < renderBottom);

        if (lineVisible) {
            if (isHeader[i]) {

                int w = getRenderedLineWidth(display, cachedLines[i], emotes, numEmotes);
                int headerX;
                if (isMine[i]) {
                    // push header left to avoid overlap with scrollbar
                    headerX = (SCREEN_WIDTH - SCROLLBAR_WIDTH - RIGHT_MARGIN) - w - BUBBLE_TEXT_INDENT;
                    if (headerX < LEFT_MARGIN)
                        headerX = LEFT_MARGIN;
                } else {
                    headerX = x + BUBBLE_PAD_X + BUBBLE_TEXT_INDENT;
                }
                // Use drawStringWithEmotes to render emotes in sender names
                drawStringWithEmotes(display, headerX, lineY, cachedLines[i], emotes, numEmotes, true);

                // Draw underline just under header text
                int underlineY = lineY + FONT_HEIGHT_SMALL;

                int underlineW = w;
                int maxW = rightEdge - headerX;
                if (maxW < 0)
                    maxW = 0;
                if (underlineW > maxW)
                    underlineW = maxW;

                for (int px = 0; px < underlineW; ++px) {
                    display->setPixel(headerX + px, underlineY);
                }

                // Draw ACK/NACK mark for our own messages
                if (isMine[i]) {
                    int markX = headerX - 10;
                    int markY = lineY;
                    if (ackForLine[i] == AckStatus::ACKED) {
                        // Destination ACK
                        drawCheckMark(display, markX, markY, 8);
                    } else if (ackForLine[i] == AckStatus::NACKED || ackForLine[i] == AckStatus::TIMEOUT) {
                        // Failure or timeout
                        drawXMark(display, markX, markY, 8);
                    } else if (ackForLine[i] == AckStatus::RELAYED) {
                        // Relay ACK
                        drawRelayMark(display, markX, markY, 8);
                    }
                    // AckStatus::NONE → show nothing
                }

            } else {
                // Render message line
                if (isMine[i]) {
                    // Calculate actual rendered width including emotes
                    int renderedWidth = getRenderedLineWidth(display, cachedLines[i], emotes, numEmotes);
                    int rightX = (SCREEN_WIDTH - SCROLLBAR_WIDTH - RIGHT_MARGIN) - renderedWidth - BUBBLE_TEXT_INDENT;
                    if (rightX < LEFT_MARGIN)
                        rightX = LEFT_MARGIN;

                    drawStringWithEmotes(display, rightX, lineY, cachedLines[i], emotes, numEmotes);
                } else {
                    drawStringWithEmotes(display, x + BUBBLE_PAD_X + BUBBLE_TEXT_INDENT, lineY, cachedLines[i], emotes,
                                         numEmotes);
                }
            }
        }

        lineY += cachedHeights[i];
    }

    // Draw scrollbar
    drawMessageScrollbar(display, usableHeight, totalHeight, finalScroll, getTextPositions(display)[1]);
    graphics::drawCommonHeader(display, x, y, titleStr);
    graphics::drawCommonFooter(display, x, y);
}

std::vector<std::string> generateLines(OLEDDisplay *display, const char *headerStr, const char *messageBuf, int textWidth)
{
    std::vector<std::string> lines;

    // Only push headerStr if it's not empty (prevents extra blank line after headers)
    if (headerStr && headerStr[0] != '\0') {
        lines.push_back(std::string(headerStr));
    }

    // Helper to check if position starts an emoji - only check at potential emoji start bytes
    auto startsWithEmoji = [](const char *buf, size_t pos, size_t &emojiLen) -> bool {
        unsigned char c = (unsigned char)buf[pos];
        // Emoji are multi-byte UTF-8: skip check for ASCII
        if (c < 0x80)
            return false;
        for (int e = 0; e < numEmotes; ++e) {
            const char *label = emotes[e].label;
            size_t labelLen = strlen(label);
            if (labelLen > 0 && strncmp(buf + pos, label, labelLen) == 0) {
                emojiLen = labelLen;
                return true;
            }
        }
        return false;
    };

    // Simple width helper - use basic string width for most checks, only use
    // emoji-aware width when line contains potential emoji bytes
    auto getLineWidth = [display](const std::string &s) -> int {
        // Quick check: if no high bytes, use fast path
        bool hasHighByte = false;
        for (char c : s) {
            if ((unsigned char)c >= 0x80) {
                hasHighByte = true;
                break;
            }
        }
        if (!hasHighByte) {
#if defined(OLED_UA) || defined(OLED_RU)
            return display->getStringWidth(s.c_str(), s.length(), true);
#else
            return display->getStringWidth(s.c_str());
#endif
        }
        return getRenderedLineWidth(display, s, emotes, numEmotes);
    };

    std::string line, word;
    for (size_t i = 0; messageBuf[i];) {
        unsigned char ch = (unsigned char)messageBuf[i];

        // Handle curly apostrophe → plain apostrophe
        if (ch == 0xE2 && (unsigned char)messageBuf[i + 1] == 0x80 && (unsigned char)messageBuf[i + 2] == 0x99) {
            word += '\'';
            i += 3;
            continue;
        }

        // Check if we're at an emoji - treat emojis as breakable units
        size_t emojiLen = 0;
        if (startsWithEmoji(messageBuf, i, emojiLen)) {
            // Flush current word to line first
            if (!word.empty()) {
                std::string test = line + word;
                if (getLineWidth(test) > textWidth && !line.empty()) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line += word;
                }
                word.clear();
            }

            // Now handle the emoji as its own unit
            std::string emojiStr(messageBuf + i, emojiLen);
            std::string test = line + emojiStr;
            if (getLineWidth(test) > textWidth && !line.empty()) {
                lines.push_back(line);
                line = emojiStr;
            } else {
                line += emojiStr;
            }
            i += emojiLen;
            continue;
        }

        if (ch == '\n') {
            if (!word.empty())
                line += word;
            if (!line.empty())
                lines.push_back(line);
            line.clear();
            word.clear();
            ++i;
        } else if (ch == ' ') {
            line += word + ' ';
            word.clear();
            ++i;
        } else {
            // Regular character - accumulate into word
            size_t charLen = utf8CharLen(ch);
            word.append(messageBuf + i, charLen);

            std::string test = line + word;
            if (getLineWidth(test) > textWidth) {
                if (!line.empty())
                    lines.push_back(line);
                line = word;
                word.clear();
            }
            i += charLen;
        }
    }

    if (!word.empty())
        line += word;
    if (!line.empty())
        lines.push_back(line);

    return lines;
}
std::vector<int> calculateLineHeights(const std::vector<std::string> &lines, const Emote *emotes,
                                      const std::vector<bool> &isHeaderVec)
{
    // Tunables for layout control
    constexpr int HEADER_UNDERLINE_GAP = 0; // space between underline and first body line
    constexpr int HEADER_UNDERLINE_PIX = 1; // underline thickness (1px row drawn)
    constexpr int BODY_LINE_LEADING = -4;   // default vertical leading for normal body lines
    constexpr int EMOTE_PADDING_ABOVE = 4;  // space above emote line (added to line above)
    constexpr int EMOTE_PADDING_BELOW = 3;  // space below emote line (added to emote line)

    std::vector<int> rowHeights;
    rowHeights.reserve(lines.size());

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const auto &line = lines[idx];
        const int baseHeight = FONT_HEIGHT_SMALL;
        int lineHeight = baseHeight;

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

        // Banner logic - respect use_long_node_name setting like NodeListRenderer
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(packet.from);
        char senderName[48] = "";
        if (node && node->has_user) {
            // Respect the same long/short name setting as NodeListRenderer
            const char *preferred = config.display.use_long_node_name ? node->user.long_name : node->user.short_name;
            const char *fallback = config.display.use_long_node_name ? node->user.short_name : node->user.long_name;
            const char *nameToUse = (preferred && preferred[0]) ? preferred : ((fallback && fallback[0]) ? fallback : nullptr);

            if (nameToUse) {
                // Apply emoji replacement to banner name
                std::string processedName = replaceUnknownEmoji(std::string(nameToUse), emotes, numEmotes);
                strncpy(senderName, processedName.c_str(), sizeof(senderName) - 1);
                senderName[sizeof(senderName) - 1] = '\0';
            }
        }
        if (senderName[0] == '\0') {
            // No name available → show NodeID in parentheses
            snprintf(senderName, sizeof(senderName), "(%08x)", packet.from);
        }

        // Truncate to fit banner width, respecting UTF-8 and emoji boundaries
        int availWidth = display->getWidth() - ((currentResolution == ScreenResolution::High) ? 40 : 20);
        if (availWidth < 0)
            availWidth = 0;

        size_t origLen = strlen(senderName);
        size_t bytePos = origLen;
        // Use emoji-aware width calculation
        while (bytePos > 0 && getRenderedLineWidth(display, std::string(senderName), emotes, numEmotes) > availWidth) {
            // Back up to the start of the previous UTF-8 character
            do {
                --bytePos;
            } while (bytePos > 0 && (senderName[bytePos] & 0xC0) == 0x80); // Skip continuation bytes
            senderName[bytePos] = '\0';
        }
        // Add ellipsis if truncated (ensure room for "...")
        if (bytePos < origLen && bytePos > 0) {
            size_t capForText = sizeof(senderName) - 1;
            if (bytePos <= capForText - 3) {
                strcat(senderName, "...");
            }
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
            if (senderName[0])
                snprintf(banner, sizeof(banner), "Alert Received from\n%s", senderName);
            else
                strcpy(banner, "Alert Received");
        } else {
            // Skip muted channels unless it's an alert
            if (isChannelMuted)
                return;

            if (senderName[0]) {
                if (currentResolution == ScreenResolution::UltraLow) {
                    strcpy(banner, "New Message");
                } else {
                    snprintf(banner, sizeof(banner), "New Message from\n%s", senderName);
                }
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

        // Shorter banner if already in a conversation (Channel or Direct)
        bool inThread = (getThreadMode() != ThreadMode::ALL);

        if (shouldWakeOnReceivedMessage()) {
            screen->setOn(true);
        }

        screen->showSimpleBanner(banner, inThread ? 1000 : 3000);
    }

    // Always focus into the correct conversation thread when a message with real text arrives
    const char *msgText = MessageStore::getText(sm);
    if (msgText && msgText[0] != '\0') {
        setThreadFor(sm, packet);
    }

    // Reset scroll for a clean start
    resetScrollState();
}

void setThreadFor(const StoredMessage &sm, const meshtastic_MeshPacket &packet)
{
    if (packet.to == 0 || packet.to == NODENUM_BROADCAST) {
        setThreadMode(ThreadMode::CHANNEL, sm.channelIndex);
    } else {
        uint32_t localNode = nodeDB->getNodeNum();
        uint32_t peer = (sm.sender == localNode) ? packet.to : sm.sender;
        setThreadMode(ThreadMode::DIRECT, -1, peer);
    }
}

} // namespace MessageRenderer
} // namespace graphics
#endif