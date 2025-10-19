#include "configuration.h"
#if HAS_SCREEN
#include "MessageRenderer.h"

// Core includes
#include "MessageStore.h"
#include "NodeDB.h"
#include "UIRenderer.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TimeFormatters.h"
#include "graphics/emotes.h"
#include "main.h"
#include "meshUtils.h"
#include "buzz/buzz.h"
#include <string>
#include <vector>

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
static bool s_dirty = true;          // true = caches must be rebuilt
static inline void markDirty() { s_dirty = true; }

static std::vector<bool>        cachedIsHeader;
static std::vector<bool>        cachedIsMine;
static std::vector<AckStatus>   cachedAckForLine;

// Time metadata for live label (index-aligned with cachedLines)
static std::vector<uint32_t>    cachedMsgTimestamp;   // original message timestamp
static std::vector<bool>        cachedIsBootRelative; // true if timestamp is boot-relative

// C++11-friendly helpers (no generic-lambda params)
template<typename T>
static inline void trim_vec_front(std::vector<T>& v, size_t n)
{
    if (v.empty()) return;
    size_t k = std::min(n, v.size());
    v.erase(v.begin(), v.begin() + k);
}

static bool shedOldest(size_t n)
{
    const size_t before = cachedLines.size();
    if (!before) return false;

    trim_vec_front(cachedLines,     n);
    trim_vec_front(cachedHeights,   n);
    trim_vec_front(cachedIsHeader,  n);
    trim_vec_front(cachedIsMine,    n);
    trim_vec_front(cachedAckForLine,n);

    // request a clean rebuild/relayout; draw code clamps scroll position
    markDirty();
    return cachedLines.size() < before;
}

template<typename F>
static bool retry_on_oom(F &&fn, size_t shedCount = 16, int maxRetries = 3)
{
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        try {
            fn();
            return true;
        } catch (const std::bad_alloc&) {
            if (!shedOldest(shedCount)) return false;
        }
    }
    return false;
}

// ---- Low-RAM guards for line buffers ----
// Absolute caps; tune per device.
static constexpr size_t kMaxTotalLines       = 512;
static constexpr size_t kMaxLinesPerMessage  = 32;
static constexpr size_t kShedBatch           = 64;

// Reserve for the lines vector with clamping and exception safety.
static bool try_reserve_lines(std::vector<std::string>& v, size_t want)
{
    if (want > kMaxTotalLines) want = kMaxTotalLines;
    try {
        // Never request less than current size() to avoid shrink reallocation.
        if (want < v.size()) want = v.size();
        v.reserve(want);
        return true;
    } catch (const std::bad_alloc&) {
        // Caller decides whether to shed caches or continue without reserve.
        return false;
    }
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

// Remove variation selectors (FE0F) and skin tone modifiers from emoji so they match your labels
std::string normalizeEmoji(const std::string &s)
{
    std::string out;
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

void drawStringWithEmotes(OLEDDisplay *display, int x, int y, const std::string &line, const Emote *emotes, int emoteCount)
{
    std::string renderLine;
    for (size_t i = 0; i < line.size();) {
        uint8_t c = (uint8_t)line[i];
        size_t len = utf8CharLen(c);
        if (c == 0xEF && i + 2 < line.size() && (uint8_t)line[i + 1] == 0xB8 && (uint8_t)line[i + 2] == 0x8F) {
            i += 3;
            continue;
        }
        if (c == 0xF0 && i + 3 < line.size() && (uint8_t)line[i + 1] == 0x9F && (uint8_t)line[i + 2] == 0x8F &&
            ((uint8_t)line[i + 3] >= 0xBB && (uint8_t)line[i + 3] <= 0xBF)) {
            i += 4;
            continue;
        }
        renderLine.append(line, i, len);
        i += len;
    }
    int cursorX = x;
    const int fontHeight = FONT_HEIGHT_SMALL;

    // Step 1: Find tallest emote in the line
    int maxIconHeight = fontHeight;
    for (size_t i = 0; i < line.length();) {
        bool matched = false;
        for (int e = 0; e < emoteCount; ++e) {
            size_t emojiLen = strlen(emotes[e].label);
            if (line.compare(i, emojiLen, emotes[e].label) == 0) {
                if (emotes[e].height > maxIconHeight)
                    maxIconHeight = emotes[e].height;
                i += emojiLen;
                matched = true;
                break;
            }
        }
        if (!matched) {
            i += utf8CharLen(static_cast<uint8_t>(line[i]));
        }
    }

    // Step 2: Baseline alignment
    int lineHeight = std::max(fontHeight, maxIconHeight);
    int baselineOffset = (lineHeight - fontHeight) / 2;
    int fontY = y + baselineOffset;

    // Step 3: Render line in segments
    size_t i = 0;
    bool inBold = false;

    while (i < line.length()) {
        // Check for ** start/end for faux bold
        if (line.compare(i, 2, "**") == 0) {
            inBold = !inBold;
            i += 2;
            continue;
        }

        // Look ahead for the next emote match
        size_t nextEmotePos = std::string::npos;
        const Emote *matchedEmote = nullptr;
        size_t emojiLen = 0;

        for (int e = 0; e < emoteCount; ++e) {
            size_t pos = line.find(emotes[e].label, i);
            if (pos != std::string::npos && (nextEmotePos == std::string::npos || pos < nextEmotePos)) {
                nextEmotePos = pos;
                matchedEmote = &emotes[e];
                emojiLen = strlen(emotes[e].label);
            }
        }

        // Render normal text segment up to the emote or bold toggle
        size_t nextControl = std::min(nextEmotePos, line.find("**", i));
        if (nextControl == std::string::npos)
            nextControl = line.length();

        if (nextControl > i) {
            std::string textChunk = line.substr(i, nextControl - i);
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
            // Vertically center emote relative to font baseline (not just midline)
            int iconY = fontY + (fontHeight - matchedEmote->height) / 2;
            display->drawXbm(cursorX, iconY, matchedEmote->width, matchedEmote->height, matchedEmote->bitmap);
            cursorX += matchedEmote->width + 1;
            i += emojiLen;
            continue;
        } else {
            // No more emotes — render the rest of the line
            std::string remaining = line.substr(i);
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
    resetScrollState();   // clears scroll & prepares for a fresh layout
    markDirty();          // forces cache rebuild on next draw
    didReset = false;     // keep existing contract (we reset above)


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
            size_t emojiLen = strlen(emotes[e].label);
            if (normalized.compare(i, emojiLen, emotes[e].label) == 0) {
                totalWidth += emotes[e].width + 1; // +1 spacing
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

void drawTextMessageFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    // Ensure any boot-relative timestamps are upgraded if RTC is valid
    messageStore.upgradeBootRelativeTimestamps();

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
        // Build lines for filtered messages (newest first)
        std::vector<std::string> allLines;
        std::vector<bool>        isMine;     // track alignment
        std::vector<bool>        isHeader;   // track header lines
        std::vector<AckStatus>   ackForLine; // per-header ACK badge

        // Reuse capacity but avoid giant reserves on tiny heaps.
        // Start with a modest estimate; clamp and tolerate failure.
        {
            size_t baseCap   = cachedLines.capacity() ? cachedLines.capacity() : 256;
            size_t estimate  = std::min(baseCap, kMaxTotalLines);
            (void)try_reserve_lines(allLines, estimate);

            // Match side vectors to whatever capacity we actually ended up with.
            size_t cap = allLines.capacity();
            try { isMine.reserve(cap); }   catch (...) {}
            try { isHeader.reserve(cap); } catch (...) {}
            try { ackForLine.reserve(cap);}catch (...) {}
        }

        std::vector<uint32_t> msgTs;
        std::vector<bool>     isBootRel;
        // Side buffers reserve best-effort (don’t throw if they can’t).
        try {
            size_t cap = allLines.capacity();
            msgTs.reserve(cap);
            isBootRel.reserve(cap);
        } catch (...) {}
    
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
            meshtastic_NodeInfoLite* node           = nodeDB->getMeshNode(m.sender);
            meshtastic_NodeInfoLite* node_recipient = nodeDB->getMeshNode(m.dest);

            char senderBuf[48] = "???";
            if (node && node->has_user && node->user.long_name && node->user.long_name[0] != '\0') {
                std::snprintf(senderBuf, sizeof(senderBuf), "%s", node->user.long_name);
            }
            bool mine = (m.sender == nodeDB->getNodeNum());
            if (mine && node_recipient && node_recipient->has_user &&
                node_recipient->user.long_name && node_recipient->user.long_name[0] != '\0') {
                std::snprintf(senderBuf, sizeof(senderBuf), "%s", node_recipient->user.long_name);
            }

            // Compute how much room the sender label has (use timeSlotPx, NOT timeBuf)
            int availWidth = SCREEN_WIDTH
                           - timeSlotPx
                           - display->getStringWidth(chanType)
                           - display->getStringWidth(" @...")
                           - 10;
            if (availWidth < 0) availWidth = 0;

            // Fit sender to available width (from feat/m5stack-cardputer-adv)
            size_t origLen = std::strlen(senderBuf);
            while (senderBuf[0] && display->getStringWidth(senderBuf) > availWidth) {
                senderBuf[std::strlen(senderBuf) - 1] = '\0';
            }

            // If we actually truncated, append "..."
            if (std::strlen(senderBuf) < origLen) {
                strcat(senderBuf, "...");
            }

            // Final header line (no time here; time is drawn live during render)
            char headerStr[96] = "";
            if (mine) std::snprintf(headerStr, sizeof(headerStr), "%s", chanType);
            else      std::snprintf(headerStr, sizeof(headerStr), "@%s %s", senderBuf, chanType);


            // Header line (guard memory)
            if (!retry_on_oom([&]{
                allLines.push_back(std::string(headerStr));
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
            const char* msgText = MessageStore::getText(m);

            std::vector<std::string> wrapped = generateLines(display, "", msgText, textWidth);

            // Cap per-message wrapped lines so one long message can't explode memory.
            if (wrapped.size() > kMaxLinesPerMessage) {
                // keep as many as possible, add ellipsis to last
                wrapped.resize(kMaxLinesPerMessage);
                if (!wrapped.empty()) {
                    // Avoid huge reallocation by appending a tiny suffix
                    wrapped.back() += " \xE2\x80\xA6"; // UTF-8 ellipsis
                }
            }
            for (auto& ln : wrapped) {
                if (!retry_on_oom([&]{
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
        if (!retry_on_oom([&]{
            newHeights = calculateLineHeights(allLines, emotes, isHeader);
        })) {
            // keep existing caches; try again next time we become dirty
            s_dirty = false;
            return;
        }
    
        // swap into persistent caches (fast, avoids realloc)
        cachedLines.clear();           cachedLines.swap(allLines);
        cachedHeights.clear();         cachedHeights.swap(newHeights);
        cachedIsHeader.clear();        cachedIsHeader.swap(isHeader);
        cachedIsMine.clear();          cachedIsMine.swap(isMine);
        cachedAckForLine.clear();      cachedAckForLine.swap(ackForLine);
        cachedMsgTimestamp.clear();    cachedMsgTimestamp.swap(msgTs);
        cachedIsBootRelative.clear();  cachedIsBootRelative.swap(isBootRel);
        s_dirty = false;
    }

    // Scrolling logic (reverse auto-scroll + manual override)
    // Newest-at-bottom is enforced by building lines via filtered.rbegin()->rend()
    int totalHeight = 0;
    for (size_t i = 0; i < cachedHeights.size(); ++i){
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
    int kBottomPadPx = FONT_HEIGHT_SMALL;
    int bottomOffsetOneRow = totalHeight - usableScrollHeight + kBottomPadPx;
    if (bottomOffsetOneRow < 0) bottomOffsetOneRow = 0; // guard small lists

    // snap to bottom pad when first entering (unless user is manually scrolling)
    if (!manualScrollActive && !scrollStarted) {
        scrollY = bottomOffsetOneRow;
    }

    if (totalHeight > usableScrollHeight) {
        // freeze autoscroll briefly after manual input
        if (manualScrollActive && (now - lastManualMs) < 5000) {
            lastTime = now; // keep timebase fresh
        } else {
            if (manualScrollActive) manualScrollActive = false;
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
    // E-Ink: disable autoscroll
    scrollY = 0.0f;
    waitingToReset = false;
    scrollStarted = false;
    lastTime = millis(); // keep timebase sane
#endif

    int scrollOffset = static_cast<int>(scrollY);
    int yOffset = -scrollOffset + getTextPositions(display)[1];

    // Render visible lines (clamp counts defensively)
    const size_t N = std::min({ cachedLines.size(),
                                cachedHeights.size(),
                                cachedIsHeader.size(),
                                cachedIsMine.size(),
                                cachedAckForLine.size() });
    for (size_t i = 0; i < N; ++i) {
        int lineY = yOffset;
        for (size_t j = 0; j < i; ++j)
            lineY += cachedHeights[j];

        if (lineY > -cachedHeights[i] && lineY < (scrollBottom + cachedHeights[i])) {
            if (cachedIsHeader[i]) {
                // ---- LIVE time computation (same logic as before) ----
                uint32_t ts = (i < cachedMsgTimestamp.size())    ? cachedMsgTimestamp[i]   : 0;
                bool isBootRel = (i < cachedIsBootRelative.size())? cachedIsBootRelative[i] : false;

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
                int w = display->getStringWidth(fullHeader.c_str());
                int headerX = cachedIsMine[i] ? (SCREEN_WIDTH - w - 2) : x;
                display->drawString(headerX, lineY, fullHeader.c_str());

                // Draw ACK/NACK mark for our own messages
                if (cachedIsMine[i]) {
                    int markX = headerX - 10;
                    int markY = lineY;
                    if (cachedAckForLine[i] == AckStatus::ACKED)        { drawCheckMark(display, markX, markY, 8); }
                    else if (cachedAckForLine[i] == AckStatus::NACKED
                          || cachedAckForLine[i] == AckStatus::TIMEOUT) { drawXMark(display, markX, markY, 8); }
                    else if (cachedAckForLine[i] == AckStatus::RELAYED) { drawRelayMark(display, markX, markY, 8); }
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

    // Only push headerStr if it's not empty (prevents extra blank line after headers)
    if (headerStr && headerStr[0] != '\0') {
        lines.push_back(std::string(headerStr));
    }

    std::string line, word;
    for (int i = 0; messageBuf[i]; ++i) {
        char ch = messageBuf[i];
        if ((unsigned char)messageBuf[i] == 0xE2 && (unsigned char)messageBuf[i + 1] == 0x80 &&
            (unsigned char)messageBuf[i + 2] == 0x99) {
            ch = '\''; // plain apostrophe
            i += 2;    // skip over the extra UTF-8 bytes
        }
        if (ch == '\n') {
            if (!word.empty())
                line += word;
            if (!line.empty())
                lines.push_back(line);
            line.clear();
            word.clear();
        } else if (ch == ' ') {
            line += word + ' ';
            word.clear();
        } else {
            word += ch;
            std::string test = line + word;
#if defined(OLED_UA) || defined(OLED_RU)
            uint16_t strWidth = display->getStringWidth(test.c_str(), test.length(), true);
#else
            uint16_t strWidth = display->getStringWidth(test.c_str());
#endif
            if (strWidth > textWidth) {
                if (!line.empty())
                    lines.push_back(line);
                line = word;
                word.clear();
            }
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
    for (int h : cachedHeights) totalHeight += h;
    int usableScrollHeight = (lastUsableScrollHeight > 0) ? lastUsableScrollHeight : (FONT_HEIGHT_SMALL * 6);
    if (cachedHeights.empty()) return 0;
    // bottom-aligned offset so newest sits one row above bottom
    int kBottomPadPx = FONT_HEIGHT_SMALL;
    int bottomOffsetOneRow = totalHeight - usableScrollHeight + kBottomPadPx;
    return std::max(0, bottomOffsetOneRow);
}

void scrollUp()
{
    manualScrollActive = true;
    lastManualMs = millis();
    waitingToReset = false; pauseStart = 0; scrollStarted = true;
    scrollY -= FONT_HEIGHT_SMALL;
    if (scrollY < 0) scrollY = 0;
    lastTime = millis();
    powerFSM.trigger(EVENT_PRESS);
    screen->forceDisplay(true);
    playChirp();
}

void scrollDown()
{
    manualScrollActive = true;
    lastManualMs = millis();
    waitingToReset = false; pauseStart = 0; scrollStarted = true;
    scrollY += FONT_HEIGHT_SMALL;
    int maxScroll = computeMaxScroll();
    if (scrollY > maxScroll) scrollY = maxScroll;
    lastTime = millis();
    powerFSM.trigger(EVENT_PRESS);
    screen->forceDisplay(true);
    playChirp();
}

} // namespace MessageRenderer
} // namespace graphics
#endif