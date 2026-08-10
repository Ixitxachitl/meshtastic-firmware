#include "configuration.h"
#if ARCH_PORTDUINO
#include "PortduinoGlue.h"
#endif
#if HAS_SCREEN
#include "CannedMessageModule.h"
#include "Channels.h"
#include "FSCommon.h"
#include "MeshRadio.h"
#include "MeshService.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "buzz.h"
#include "detect/ScanI2C.h"
#include "gps/RTC.h"
#include "graphics/EmoteRenderer.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/MessageRenderer.h"
#include "graphics/draw/NotificationRenderer.h"
#include "graphics/draw/UIRenderer.h"
#include "graphics/emotes.h"
#include "graphics/images.h"
#include "input/SerialKeyboard.h"
#include "input/TouchScreenImpl1.h" // for BASEUI_HAS_TOUCH_DRAG
#include "main.h"                   // for cardkb_found
#include "mesh/generated/meshtastic/cannedmessages.pb.h"
#include "modules/AdminModule.h"
#include "modules/ExternalNotificationModule.h" // for buzzer control
extern MessageStore messageStore;
#if HAS_TRACKBALL
#include "input/TrackballInterruptImpl1.h"
#endif
#if !MESHTASTIC_EXCLUDE_GPS
#include "GPS.h"
#endif
#if defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS) && !defined(MESHTASTIC_INCLUDE_INKHUD)
#include "graphics/BaseUIEInkDisplay.h" // NicheGraphics-backed BaseUI e-ink adapter
#elif defined(USE_EINK) && defined(USE_EINK_DYNAMICDISPLAY)
#include "graphics/EInkDynamicDisplay.h" // To select between full and fast refresh on E-Ink displays
#endif

#ifndef INPUTBROKER_MATRIX_TYPE
#define INPUTBROKER_MATRIX_TYPE 0
#endif

#include "graphics/ScreenFonts.h"
#include <Throttle.h>

// Remove Canned message screen if no action is taken for some milliseconds
#define INACTIVATE_AFTER_MS 20000

// Emote button drawn over the composer on devices that type on hardware keys but still have a
// touchscreen - without it there is no way to reach the picker except a keyboard that has no emote
// key. Sized from the artwork it holds, so it grows with the variant's icon scale.
#define EMOTE_BUTTON_SIZE (24 * BASEUI_ICON_SCALE)
#define EMOTE_BUTTON_MARGIN 2
// The same absolute radius the virtual keyboard gives a key cap, not the same proportion. What
// makes a corner read as round is the arc's size in pixels: drawRoundedRect() plots a Bresenham
// arc, and below about 7px it degenerates into a flat 45 degree chamfer with no curvature left.
// The keyboard's 7 sits on a ~56px cap, so copying its 1/8 ratio onto a 24px button would mean a
// 3px corner - squarer, not rounder. Scaled with the box so it holds at any BASEUI_ICON_SCALE.
#define EMOTE_BUTTON_RADIUS (7 * BASEUI_ICON_SCALE)

// One condition for the button's artwork and its hit box, so the two can't disagree about whether
// it exists. A virtual keyboard carries its own emote key, EXCLUDE_EMOJI builds have no artwork to
// show, and without a pointer there is nothing to press. ARCH_PORTDUINO joins the hardware-keyboard
// boards because the native/SDL build types on the host keyboard and reports mouse input as touch -
// the same shape as a T-Deck, and the same TouchScreenImpl1.h gate BASEUI_HAS_TOUCH_DRAG uses.
#ifndef CANNED_MESSAGE_HAS_EMOTE_BUTTON
#if !defined(EXCLUDE_EMOJI) && !defined(USE_VIRTUAL_KEYBOARD) &&                                                                 \
    ((defined(HAS_PHYSICAL_KEYBOARD) && HAS_TOUCHSCREEN) || defined(ARCH_PORTDUINO))
#define CANNED_MESSAGE_HAS_EMOTE_BUTTON 1
#else
#define CANNED_MESSAGE_HAS_EMOTE_BUTTON 0
#endif
#endif

// Insets around the virtual keyboard, as a percentage of the display. Zero by default - fourteen
// columns want every pixel they can get - but a display whose edges the user cannot comfortably
// reach (rounded corners, a raised bezel, a watch strap in the way) needs the outer keys pulled in.
// Expressed as percentages rather than pixels so a variant states the intent once, whatever its
// resolution. Kept apart from BASEUI_BODY_LR_MARGIN: that insets text, this insets touch targets,
// and a device can want very different amounts of each.
// How wide the key grid is drawn, as a percentage of the space between the side insets. Above
// 100 the keyboard is wider than the screen and pans horizontally with a finger drag, which buys
// bigger keys on a small touchscreen. 100 keeps every existing board exactly as it was.
#ifndef BASEUI_KEYBOARD_ZOOM_PCT
#define BASEUI_KEYBOARD_ZOOM_PCT 100
#endif
// Key row height as a percentage of what the available space would otherwise give. Above 100 the
// block grows upward from the bottom inset, taking room from the draft above it.
#ifndef BASEUI_KEYBOARD_KEY_HEIGHT_PCT
#define BASEUI_KEYBOARD_KEY_HEIGHT_PCT 100
#endif
#ifndef BASEUI_KEYBOARD_LR_MARGIN_PCT
#define BASEUI_KEYBOARD_LR_MARGIN_PCT 0
#endif
#ifndef BASEUI_KEYBOARD_BOTTOM_MARGIN_PCT
#define BASEUI_KEYBOARD_BOTTOM_MARGIN_PCT 0
#endif

namespace graphics
{
extern int bannerSignalBars;
}
extern ScanI2C::DeviceAddress cardkb_found;
extern bool osk_found;

static const char *cannedMessagesConfigFile = "/prefs/cannedConf.proto";
static NodeNum lastDest = NODENUM_BROADCAST;
static uint8_t lastChannel = 0;
static bool lastDestSet = false;

meshtastic_CannedMessageModuleConfig cannedMessageModuleConfig;

CannedMessageModule *cannedMessageModule;

CannedMessageModule::CannedMessageModule()
    : SinglePortModule("canned", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("CannedMessage")
{
    this->loadProtoForModule();
    if ((this->splitConfiguredMessages() <= 0) && (cardkb_found.address == 0x00) && !INPUTBROKER_MATRIX_TYPE) {
        LOG_INFO("CannedMessage: none configured, disabled");
        this->updateState(CANNED_MESSAGE_RUN_STATE_DISABLED);
        disable();
    } else {
        LOG_INFO("CannedMessageModule is enabled");
        moduleConfig.canned_message.enabled = true;
        this->inputObserver.observe(inputBroker);
    }
}

void CannedMessageModule::LaunchWithDestination(NodeNum newDest, uint8_t newChannel)
{
    // Do NOT override explicit broadcast replies

    if (newDest == 0) {
        dest = NODENUM_BROADCAST;
    } else {
        dest = newDest;
    }
    channel = newChannel;

    lastDest = dest;
    lastChannel = channel;
    lastDestSet = true;

    // Upon activation, highlight "[Select Destination]"
    int selectDestination = 0;
    for (int i = 0; i < messagesCount; ++i) {
        if (strcmp(messages[i], "[Select Destination]") == 0) {
            selectDestination = i;
            break;
        }
    }
    currentMessageIndex = selectDestination;

    // This triggers the canned message list
    updateState(CANNED_MESSAGE_RUN_STATE_ACTIVE, true);
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    notifyObservers(&e);

    LOG_DEBUG("[CannedMessage] LaunchWithDestination dest=0x%08x ch=%d", dest, channel);
}

void CannedMessageModule::LaunchFreetextWithDestination(NodeNum newDest, uint8_t newChannel)
{
    // Do NOT override explicit broadcast replies

    if (newDest == 0) {
        dest = NODENUM_BROADCAST;
    } else {
        dest = newDest;
    }
    channel = newChannel;

    lastDest = dest;
    lastChannel = channel;
    lastDestSet = true;

    updateState(CANNED_MESSAGE_RUN_STATE_FREETEXT, true);
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    notifyObservers(&e);

    LOG_DEBUG("[CannedMessage] LaunchFreetextWithDestination dest=0x%08x ch=%d", dest, channel);
}

static bool returnToCannedList = false;
bool hasKeyForNode(const meshtastic_NodeInfoLite *node)
{
    return nodeInfoLiteHasUser(node) && node->public_key.size > 0;
}
/**
 * @brief Items in array this->messages will be set to be pointing on the right
 *     starting points of the string this->messageStore
 *
 * @return int Returns the number of messages found.
 */

int CannedMessageModule::splitConfiguredMessages()
{
    int i = 0;

    String canned_messages = cannedMessageModuleConfig.messages;

    // Copy all message parts into the buffer
    strncpy(this->messageBuffer, canned_messages.c_str(), sizeof(this->messageBuffer));

    // Temporary array to allow for insertion
    const char *tempMessages[CANNED_MESSAGE_MODULE_MESSAGE_MAX_COUNT + 3] = {0};
    int tempCount = 0;
    // Insert at position 0 (top)
    tempMessages[tempCount++] = "[Select Destination]";
#if defined(USE_VIRTUAL_KEYBOARD)
    // Add a "Free Text" entry at the top if using a touch screen virtual keyboard
    tempMessages[tempCount++] = "[-- Free Text --]";
#else
    // A detected physical keyboard takes priority over the trackball/rotary on-screen keyboard:
    // those devices reach freetext through the kb_found-gated menu entries instead (same as
    // T-Deck), which land directly in the plain physical-keyboard text box.
    if (osk_found && !kb_found && screen) {
        tempMessages[tempCount++] = "[-- Free Text --]";
    }
#endif

    // First message always starts at buffer start
    tempMessages[tempCount++] = this->messageBuffer;
    int upTo = strlen(this->messageBuffer) - 1;

    // Walk buffer, splitting on '|'
    while (i < upTo) {
        if (this->messageBuffer[i] == '|') {
            this->messageBuffer[i] = '\0'; // End previous message
            if (tempCount >= CANNED_MESSAGE_MODULE_MESSAGE_MAX_COUNT - 1)
                break;
            tempMessages[tempCount++] = (this->messageBuffer + i + 1);
        }
        i += 1;
    }

    // Add [Exit] as the last entry
    tempMessages[tempCount++] = "[Exit]";

    // Copy to the member array
    for (int k = 0; k < tempCount; ++k) {
        this->messages[k] = (char *)tempMessages[k];
    }
    this->messagesCount = tempCount;

    return this->messagesCount;
}
void CannedMessageModule::drawHeader(OLEDDisplay *display, int16_t x, int16_t y, char *buffer)
{
    (void)buffer;

    char header[96];
    if (this->dest == NODENUM_BROADCAST) {
        const char *channelName = channels.getName(this->channel);
        snprintf(header, sizeof(header), "To: #%s", channelName ? channelName : "?");
    } else {
        snprintf(header, sizeof(header), "To: @%s", getNodeName(this->dest));
    }

    // First row of text: inset horizontally by the header L/R margin and pushed down by the header margin
    const int headerX = x + BASEUI_HEADER_LR_MARGIN;
    const int headerY = y + BASEUI_HEADER_MARGIN;
    const int maxWidth = std::max(0, display->getWidth() - headerX - BASEUI_HEADER_LR_MARGIN);
    char truncatedHeader[96];
    graphics::UIRenderer::truncateStringWithEmotes(display, header, truncatedHeader, sizeof(truncatedHeader), maxWidth);
    graphics::UIRenderer::drawStringWithEmotes(display, headerX, headerY, truncatedHeader, FONT_HEIGHT_SMALL, 1, false);
}

void CannedMessageModule::resetSearch()
{
    int previousDestIndex = destIndex;

    searchQuery = "";
    updateDestinationSelectionList();

    // Adjust scrollIndex so previousDestIndex is still visible
    int totalEntries = activeChannelIndices.size() + filteredNodes.size();
    this->visibleRows = (displayHeight - FONT_HEIGHT_SMALL * 2) / FONT_HEIGHT_SMALL;
    if (this->visibleRows < 1)
        this->visibleRows = 1;
    int maxScrollIndex = std::max(0, totalEntries - visibleRows);
    scrollIndex = std::min(std::max(previousDestIndex - (visibleRows / 2), 0), maxScrollIndex);

    lastUpdateMillis = millis();
    requestFocus();
}
void CannedMessageModule::updateDestinationSelectionList()
{
    static size_t lastNumMeshNodes = 0;
    static String lastSearchQuery = "";

    size_t numMeshNodes = nodeDB->getNumMeshNodes();
    bool nodesChanged = (numMeshNodes != lastNumMeshNodes);
    lastNumMeshNodes = numMeshNodes;

    // Early exit if nothing changed
    if (searchQuery == lastSearchQuery && !nodesChanged)
        return;
    lastSearchQuery = searchQuery;
    needsUpdate = false;

    this->filteredNodes.clear();
    this->activeChannelIndices.clear();

    NodeNum myNodeNum = nodeDB->getNodeNum();
    String lowerSearchQuery = searchQuery;
    lowerSearchQuery.toLowerCase();

    // Preallocate space to reduce reallocation
    this->filteredNodes.reserve(numMeshNodes);

    for (size_t i = 0; i < numMeshNodes; ++i) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node || node->num == myNodeNum || !nodeInfoLiteHasUser(node) || node->public_key.size != 32)
            continue;

        const String &nodeName = node->long_name;

        if (searchQuery.length() == 0) {
            this->filteredNodes.push_back({node, sinceLastSeen(node)});
        } else {
            // Avoid unnecessary lowercase conversion if already matched
            String lowerNodeName = nodeName;
            lowerNodeName.toLowerCase();

            if (lowerNodeName.indexOf(lowerSearchQuery) != -1) {
                this->filteredNodes.push_back({node, sinceLastSeen(node)});
            }
        }
    }

    // Populate active channels
    std::vector<String> seenChannels;
    seenChannels.reserve(channels.getNumChannels());
    for (uint8_t i = 0; i < channels.getNumChannels(); ++i) {
        String name = channels.getName(i);
        if (name.length() > 0 && std::find(seenChannels.begin(), seenChannels.end(), name) == seenChannels.end()) {
            this->activeChannelIndices.push_back(i);
            seenChannels.push_back(name);
        }
    }

    scrollIndex = 0; // Show first result at the top
    destIndex = 0;   // Highlight the first entry
    if (nodesChanged && runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION) {
        LOG_INFO("Nodes changed, forcing UI refresh");
        screen->forceDisplay();
    }
}

static int getRowHeightForEmoteText(const char *text, int minimumHeight, int emoteSpacing = 2)
{
    // Grow the row only when an emote is taller than the font.
    const auto metrics =
        graphics::EmoteRenderer::analyzeLine(nullptr, text ? text : "", 0, graphics::emotes, graphics::numEmotes, emoteSpacing);
    return std::max(minimumHeight, metrics.tallestHeight + 2);
}

static void drawCenteredEmoteText(OLEDDisplay *display, int x, int y, int rowHeight, const char *text, int emoteSpacing = 2)
{
    // Center mixed text and emotes inside the row height.
    const auto metrics = graphics::EmoteRenderer::analyzeLine(nullptr, text ? text : "", FONT_HEIGHT_SMALL, graphics::emotes,
                                                              graphics::numEmotes, emoteSpacing);
    const int contentHeight = std::max(FONT_HEIGHT_SMALL, metrics.tallestHeight);
    const int drawY = y + ((rowHeight - contentHeight) / 2);
    graphics::EmoteRenderer::drawStringWithEmotes(display, x, drawY, text ? text : "", FONT_HEIGHT_SMALL, graphics::emotes,
                                                  graphics::numEmotes, emoteSpacing, false);
}

static size_t firstWrappedTokenLen(const char *text)
{
    // Fall back to one full emote or one UTF-8 glyph when width is tiny.
    if (!text || !*text)
        return 0;

    const size_t textLen = strlen(text);
    size_t matchLen = 0;
    if (graphics::EmoteRenderer::findEmoteAt(text, textLen, 0, matchLen, graphics::emotes, graphics::numEmotes))
        return matchLen;

    return graphics::EmoteRenderer::utf8CharLen(static_cast<uint8_t>(text[0]));
}

#if defined(USE_VIRTUAL_KEYBOARD) || CANNED_MESSAGE_HAS_EMOTE_BUTTON
// Rounded key caps. drawRect() reads as a grid of boxes at the sizes a full QWERTY layout forces;
// rounding the corners is what lets a key still look like a key once it is only ~25px wide. The
// emote button borrows the same outline so a lone button still reads as one of these key caps.
static void drawRoundedRect(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r)
{
    r = std::min<int16_t>(r, std::min(w, h) / 2);
    if (r < 1) {
        display->drawRect(x, y, w, h);
        return;
    }

    display->drawHorizontalLine(x + r, y, w - r * 2);
    display->drawHorizontalLine(x + r, y + h - 1, w - r * 2);
    display->drawVerticalLine(x, y + r, h - r * 2);
    display->drawVerticalLine(x + w - 1, y + r, h - r * 2);
    // Quadrant bitmask: 1 = top-right, 2 = top-left, 4 = bottom-left, 8 = bottom-right
    display->drawCircleQuads(x + r, y + r, r, 2);
    display->drawCircleQuads(x + w - 1 - r, y + r, r, 1);
    display->drawCircleQuads(x + r, y + h - 1 - r, r, 4);
    display->drawCircleQuads(x + w - 1 - r, y + h - 1 - r, r, 8);
}
#endif // USE_VIRTUAL_KEYBOARD || CANNED_MESSAGE_HAS_EMOTE_BUTTON

#if defined(USE_VIRTUAL_KEYBOARD)
// Filled counterpart, for the cap under the finger. Only the keyboard highlights a key this way.
static void fillRoundedRect(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r)
{
    r = std::min<int16_t>(r, std::min(w, h) / 2);
    if (r < 1) {
        display->fillRect(x, y, w, h);
        return;
    }

    display->fillRect(x + r, y, w - r * 2, h);
    display->fillRect(x, y + r, r, h - r * 2);
    display->fillRect(x + w - r, y + r, r, h - r * 2);
    display->fillCircle(x + r, y + r, r);
    display->fillCircle(x + w - 1 - r, y + r, r);
    display->fillCircle(x + r, y + h - 1 - r, r);
    display->fillCircle(x + w - 1 - r, y + h - 1 - r, r);
}

// Second glyph a key produces while shift is held. Returns 0 for keys that only case-shift.
static char shiftedSymbol(char c)
{
    switch (c) {
    case '`':
        return '~';
    case '1':
        return '!';
    case '2':
        return '@';
    case '3':
        return '#';
    case '4':
        return '$';
    case '5':
        return '%';
    case '6':
        return '^';
    case '7':
        return '&';
    case '8':
        return '*';
    case '9':
        return '(';
    case '0':
        return ')';
    case '-':
        return '_';
    case '=':
        return '+';
    case '[':
        return '{';
    case ']':
        return '}';
    case '\\':
        return '|';
    case ';':
        return ':';
    case '\'':
        return '"';
    case ',':
        return '<';
    case '.':
        return '>';
    case '/':
        return '?';
    default:
        return 0;
    }
}
#endif // USE_VIRTUAL_KEYBOARD

static void drawWrappedEmoteText(OLEDDisplay *display, int x, int y, const char *text, int maxWidth, int minimumRowHeight,
                                 int emoteSpacing = 2)
{
    // Wrap onto multiple rows without splitting emotes.
    if (!display || !text || maxWidth <= 0)
        return;

    constexpr size_t kLineBufferSize = 256;
    char lineBuffer[kLineBufferSize];
    const size_t textLen = strlen(text);
    size_t offset = 0;
    int yCursor = y;

    while (offset < textLen) {
        size_t copied = graphics::EmoteRenderer::truncateToWidth(display, text + offset, lineBuffer, sizeof(lineBuffer), maxWidth,
                                                                 "", graphics::emotes, graphics::numEmotes, emoteSpacing);
        size_t consumed = copied;

        if (copied == 0) {
            consumed = firstWrappedTokenLen(text + offset);
            if (consumed == 0)
                break;

            const size_t fallbackLen = std::min(consumed, sizeof(lineBuffer) - 1);
            memcpy(lineBuffer, text + offset, fallbackLen);
            lineBuffer[fallbackLen] = '\0';
            consumed = fallbackLen;
        } else if (text[offset + copied] != '\0') {
            // Prefer wrapping at the last space when a full line does not fit.
            size_t lastSpace = copied;
            while (lastSpace > 0 && lineBuffer[lastSpace - 1] != ' ')
                --lastSpace;

            if (lastSpace > 0) {
                consumed = lastSpace;
                while (consumed > 0 && lineBuffer[consumed - 1] == ' ')
                    --consumed;
                lineBuffer[consumed] = '\0';
            }
        }

        if (lineBuffer[0]) {
            const int rowHeight = getRowHeightForEmoteText(lineBuffer, minimumRowHeight, emoteSpacing);
            drawCenteredEmoteText(display, x, yCursor, rowHeight, lineBuffer, emoteSpacing);
            yCursor += rowHeight;
        }

        offset += std::max<size_t>(consumed, 1);
        while (offset < textLen && text[offset] == ' ')
            ++offset;
    }
}
/**
 * Main input event dispatcher for CannedMessageModule.
 * Routes keyboard/button/touch input to the correct handler based on the current runState.
 * Only one handler (per state) processes each event, eliminating redundancy.
 */
#if BASEUI_HAS_TOUCH_DRAG
static bool keyboardPanDragUpdate(const InputEvent *event, CannedMessageModule *module);
static bool keyboardPanDragEnd();
#endif

int CannedMessageModule::handleInputEvent(const InputEvent *event)
{
    // Block ALL input if an alert banner is active
    if (screen && screen->isOverlayBannerShowing()) {
        return 0;
    }

    // Tab key: Always allow switching between canned/destination screens
    if (event->kbchar == INPUT_BROKER_MSG_TAB && handleTabSwitch(event))
        return 1;

    // Matrix keypad: If matrix key, trigger action select for canned message
    if (event->inputEvent == INPUT_BROKER_MATRIXKEY) {
        updateState(CANNED_MESSAGE_RUN_STATE_ACTION_SELECT, true);
        payload = INPUT_BROKER_MATRIXKEY;
        currentMessageIndex = event->kbchar - 1;
        lastTouchMillis = millis();
        return 1;
    }

    // Always normalize navigation/select buttons for further handlers
    bool isUp = isUpEvent(event);
    bool isDown = isDownEvent(event);
    bool isSelect = isSelectEvent(event);

    // Route event to handler for current UI state (no double-handling)
    switch (runState) {
    // Node/Channel destination selection mode: Handles character search, arrows, select, cancel, backspace
    case CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION:
        if (handleDestinationSelectionInput(event, isUp, isDown, isSelect))
            return 1;
        return 0; // prevent fall-through to selector input

    // Free text input mode: Handles character input, cancel, backspace, select, etc.
    case CANNED_MESSAGE_RUN_STATE_FREETEXT:
        return handleFreeTextInput(event); // All allowed input for this state

    // Virtual keyboard mode: Show virtual keyboard and handle input

    // If sending, block all input except global/system (handled above)
    case CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE:
        return 1;

    // If sending, block all input except global/system (handled above)
    case CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER:
        return handleEmotePickerInput(event);

    case CANNED_MESSAGE_RUN_STATE_INACTIVE:
        if (event->inputEvent == INPUT_BROKER_ALT_LONG) {
            LaunchWithDestination(NODENUM_BROADCAST);
            return 1;
        }
        // Printable char (ASCII) opens free text compose
        if (event->kbchar >= 32 && event->kbchar <= 126) {
            updateState(CANNED_MESSAGE_RUN_STATE_FREETEXT, true);
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e);
            // Immediately process the input in the new state (freetext)
            return handleFreeTextInput(event);
        }
        return 0;
        break;

    // (Other states can be added here as needed)
    default:
        break;
    }

    // If no state handler above processed the event, let the message selector try to handle it
    // (Handles up/down/select on canned message list, exit/return)
    if (handleMessageSelectorInput(event, isUp, isDown, isSelect))
        return 1;

    // Default: event not handled by canned message system, allow others to process
    return 0;
}

void CannedMessageModule::updateState(cannedMessageModuleRunState newState, bool shouldRequestFocus)
{
    // Opening the picker keeps the emote that was last chosen but drops where the grid was
    // scrolled to; out of range is the signal for the next draw to recentre on the selection.
    if (newState == CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER && runState != CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER)
        emoteScrollOffset = -1;

#if defined(USE_VIRTUAL_KEYBOARD)
    // Shift is a property of the on-screen keyboard, so it must not outlive the composer.
    // Leaving by anything other than the ESC key used to leave it latched, and it was then
    // still latched - and still inverted - on the next visit.
    if (newState != CANNED_MESSAGE_RUN_STATE_FREETEXT && runState == CANNED_MESSAGE_RUN_STATE_FREETEXT)
        shift = false;
#endif

    runState = newState;
    if (runState == CANNED_MESSAGE_RUN_STATE_FREETEXT) {
        inputBroker->menuMode =
            false; // Allow any key input to be sent to the message composer instead of being interpreted as menu navigation
    } else {
        inputBroker->menuMode = true; // Re-enable menu navigation for destination selection
    }
    if (shouldRequestFocus) {
        requestFocus();
    }
}

bool CannedMessageModule::isUpEvent(const InputEvent *event)
{
    // The emote picker is deliberately absent: it is a grid, so it reads the raw directions and
    // needs LEFT to mean left rather than a second way to say up.
    return event->inputEvent == INPUT_BROKER_UP ||
           ((runState == CANNED_MESSAGE_RUN_STATE_ACTIVE || runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION) &&
            (event->inputEvent == INPUT_BROKER_LEFT || event->inputEvent == INPUT_BROKER_ALT_PRESS));
}
bool CannedMessageModule::isDownEvent(const InputEvent *event)
{
    return event->inputEvent == INPUT_BROKER_DOWN ||
           ((runState == CANNED_MESSAGE_RUN_STATE_ACTIVE || runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION) &&
            (event->inputEvent == INPUT_BROKER_RIGHT || event->inputEvent == INPUT_BROKER_USER_PRESS));
}
bool CannedMessageModule::isSelectEvent(const InputEvent *event)
{
    return event->inputEvent == INPUT_BROKER_SELECT;
}

bool CannedMessageModule::handleTabSwitch(const InputEvent *event)
{
    if (event->kbchar != 0x09)
        return false;

    const cannedMessageModuleRunState targetState = (runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION)
                                                        ? CANNED_MESSAGE_RUN_STATE_FREETEXT
                                                        : CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION;

    destIndex = 0;
    scrollIndex = 0;
    if (targetState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION)
        updateDestinationSelectionList();

    updateState(targetState, true);

    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    notifyObservers(&e);
    screen->forceDisplay();
    return true;
}

int CannedMessageModule::handleDestinationSelectionInput(const InputEvent *event, bool isUp, bool isDown, bool isSelect)
{
    // Override isDown and isSelect ONLY for destination selector behavior
    if (runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION) {
        if (event->inputEvent == INPUT_BROKER_USER_PRESS) {
            isDown = true;
        } else if (event->inputEvent == INPUT_BROKER_SELECT) {
            isSelect = true;
        }
    }

    if (event->kbchar >= 32 && event->kbchar <= 126 && !isUp && !isDown && event->inputEvent != INPUT_BROKER_LEFT &&
        event->inputEvent != INPUT_BROKER_RIGHT && event->inputEvent != INPUT_BROKER_SELECT) {
        this->searchQuery += (char)event->kbchar;
        needsUpdate = true;
        if ((millis() - lastFilterUpdate) > filterDebounceMs) {
            runOnce(); // update filter immediately
            lastFilterUpdate = millis();
        }
        return 1;
    }

    size_t numMeshNodes = filteredNodes.size();
    int totalEntries = numMeshNodes + activeChannelIndices.size();
    int columns = 1;
    int totalRows = totalEntries;
    int maxScrollIndex = std::max(0, totalRows - visibleRows);
    scrollIndex = clamp(scrollIndex, 0, maxScrollIndex);

    // Handle backspace
    if (event->inputEvent == INPUT_BROKER_BACK) {
        if (searchQuery.length() > 0) {
            searchQuery.remove(searchQuery.length() - 1);
            needsUpdate = true;
            runOnce();
        }
        if (searchQuery.length() == 0) {
            resetSearch();
            needsUpdate = false;
        }
        return 1;
    }

    if (isUp) {
        if (destIndex > 0) {
            destIndex--;
        } else if (totalEntries > 0) {
            destIndex = totalEntries - 1;
        }

        if ((destIndex / columns) < scrollIndex)
            scrollIndex = destIndex / columns;
        else if ((destIndex / columns) >= (scrollIndex + visibleRows))
            scrollIndex = (destIndex / columns) - visibleRows + 1;

        screen->forceDisplay(true);
        return 1;
    }

    if (isDown) {
        if (destIndex + 1 < totalEntries) {
            destIndex++;
        } else if (totalEntries > 0) {
            destIndex = 0;
            scrollIndex = 0;
        }

        if ((destIndex / columns) >= (scrollIndex + visibleRows))
            scrollIndex = (destIndex / columns) - visibleRows + 1;

        screen->forceDisplay(true);
        return 1;
    }

    // SELECT
    if (isSelect) {
        if (destIndex < static_cast<int>(activeChannelIndices.size())) {
            dest = NODENUM_BROADCAST;
            channel = activeChannelIndices[destIndex];
            lastDest = dest;
            lastChannel = channel;
            lastDestSet = true;
        } else {
            int nodeIndex = destIndex - static_cast<int>(activeChannelIndices.size());
            if (nodeIndex >= 0 && nodeIndex < static_cast<int>(filteredNodes.size())) {
                const meshtastic_NodeInfoLite *selectedNode = filteredNodes[nodeIndex].node;
                if (selectedNode) {
                    dest = selectedNode->num;
                    channel = selectedNode->channel;
                    // Already saves here, but for clarity, also:
                    lastDest = dest;
                    lastChannel = channel;
                    lastDestSet = true;
                }
            }
        }

        updateState(returnToCannedList ? CANNED_MESSAGE_RUN_STATE_ACTIVE : CANNED_MESSAGE_RUN_STATE_FREETEXT, true);
        returnToCannedList = false;
        screen->forceDisplay(true);
        return 1;
    }

    // CANCEL
    if (event->inputEvent == INPUT_BROKER_CANCEL || event->inputEvent == INPUT_BROKER_ALT_LONG) {
        updateState(returnToCannedList ? CANNED_MESSAGE_RUN_STATE_ACTIVE : CANNED_MESSAGE_RUN_STATE_FREETEXT, true);
        returnToCannedList = false;
        searchQuery = "";

        // UIFrameEvent e;
        // e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        // notifyObservers(&e);
        screen->forceDisplay(true);
        return 1;
    }

    return 0;
}

bool CannedMessageModule::handleMessageSelectorInput(const InputEvent *event, bool isUp, bool isDown, bool isSelect)
{
    // Override isDown and isSelect ONLY for canned message list behavior
    if (runState == CANNED_MESSAGE_RUN_STATE_ACTIVE) {
        if (event->inputEvent == INPUT_BROKER_USER_PRESS) {
            isDown = true;
        } else if (event->inputEvent == INPUT_BROKER_SELECT) {
            isSelect = true;
        }
    }

    if (runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION)
        return false;

    // Handle Cancel key: go inactive, clear UI state
    if (runState != CANNED_MESSAGE_RUN_STATE_INACTIVE &&
        (event->inputEvent == INPUT_BROKER_CANCEL || event->inputEvent == INPUT_BROKER_ALT_LONG)) {
        updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
        freetext = "";
        cursor = 0;
        payload = 0;
        currentMessageIndex = -1;

        // Notify UI that we want to redraw/close this screen
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        screen->forceDisplay();
        return true;
    }

    bool handled = false;

    // Handle up/down navigation
    if (isUp && messagesCount > 0) {
        updateState(CANNED_MESSAGE_RUN_STATE_ACTION_UP);
        handled = true;
    } else if (isDown && messagesCount > 0) {
        updateState(CANNED_MESSAGE_RUN_STATE_ACTION_DOWN);
        handled = true;
    } else if (isSelect) {
        const char *current = messages[currentMessageIndex];

        // [Select Destination] triggers destination selection UI
        if (strcmp(current, "[Select Destination]") == 0) {
            returnToCannedList = true;
            updateState(CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION, true);
            destIndex = 0;
            scrollIndex = 0;
            updateDestinationSelectionList(); // Make sure list is fresh
            screen->forceDisplay();
            return true;
        }

        // [Exit] returns to the main/inactive screen
        if (strcmp(current, "[Exit]") == 0) {
            // Set runState to inactive so we return to main UI
            updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
            currentMessageIndex = -1;

            // Notify UI to regenerate frame set and redraw
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e);
            screen->forceDisplay();
            return true;
        }

        // [Free Text] triggers the free text input (virtual keyboard)
#if defined(USE_VIRTUAL_KEYBOARD)
        if (strcmp(current, "[-- Free Text --]") == 0) {
            updateState(CANNED_MESSAGE_RUN_STATE_FREETEXT, true);
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e);
            return true;
        }
#else
        if (strcmp(current, "[-- Free Text --]") == 0) {
            if (osk_found && !kb_found && screen) {
                char headerBuffer[64];
                if (this->dest == NODENUM_BROADCAST) {
                    snprintf(headerBuffer, sizeof(headerBuffer), "To: #%s", channels.getName(this->channel));
                } else {
                    snprintf(headerBuffer, sizeof(headerBuffer), "To: @%s", getNodeName(this->dest));
                }
                screen->showTextInput(headerBuffer, "", 300000, [this](const std::string &text) {
                    if (!text.empty()) {
                        this->freetext = text.c_str();
                        this->payload = CANNED_MESSAGE_RUN_STATE_FREETEXT;
                        updateState(CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE);
                        currentMessageIndex = -1;

                        UIFrameEvent e;
                        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
                        this->notifyObservers(&e);
                        screen->forceDisplay();

                        setIntervalFromNow(500);
                        return;
                    } else {
                        // Don't delete virtual keyboard immediately - it might still be executing
                        // Instead, just clear the callback and reset banner to stop input processing
                        graphics::NotificationRenderer::textInputCallback = nullptr;
                        graphics::NotificationRenderer::resetBanner();

                        // Return to inactive state
                        this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
                        this->currentMessageIndex = -1;
                        this->freetext = "";
                        this->cursor = 0;

                        // Force display update to show normal screen
                        UIFrameEvent e;
                        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
                        this->notifyObservers(&e);
                        screen->forceDisplay();

                        // Schedule cleanup for next loop iteration to ensure safe deletion
                        setIntervalFromNow(50);
                        return;
                    }
                });

                return true;
            }
            // Physical keyboard available: skip the trackball-style on-screen keyboard and go
            // straight to the plain freetext text box, same as the dedicated menu entries.
            LaunchFreetextWithDestination(dest, channel);
            return true;
        }
#endif

        // Normal canned message selection
        if (runState == CANNED_MESSAGE_RUN_STATE_INACTIVE || runState == CANNED_MESSAGE_RUN_STATE_DISABLED) {
        } else {
#if CANNED_MESSAGE_ADD_CONFIRMATION
            const int savedIndex = currentMessageIndex;
            graphics::menuHandler::showConfirmationBanner("Send message?", [this, savedIndex]() {
                this->currentMessageIndex = savedIndex;
                this->payload = this->runState;
                this->updateState(CANNED_MESSAGE_RUN_STATE_ACTION_SELECT);
                this->setIntervalFromNow(0);
            });
#else
            payload = runState;
            updateState(CANNED_MESSAGE_RUN_STATE_ACTION_SELECT);
#endif
            // Do not immediately set runState; wait for confirmation
            handled = true;
        }
    }

    if (handled) {
        requestFocus();
        if (runState == CANNED_MESSAGE_RUN_STATE_ACTION_SELECT)
            setIntervalFromNow(0);
        else
            runOnce();
    }

    return handled;
}
bool CannedMessageModule::handleFreeTextInput(const InputEvent *event)
{
    // Always process only if in FREETEXT mode
    if (runState != CANNED_MESSAGE_RUN_STATE_FREETEXT)
        return false;

#if HAS_TOUCHSCREEN
    // Tapping the "To: ..." row opens the destination picker - the same thing Tab does on a
    // hardware keyboard, and on a touch-only device the only way to reach it at all. Sits above
    // both keyboard paths because the header is drawn the same way for each.
    if ((event->touchX != 0 || event->touchY != 0) && event->touchY < BASEUI_HEADER_MARGIN + FONT_HEIGHT_SMALL) {
        destIndex = 0;
        scrollIndex = 0;
        currentMessageIndex = -1;
        updateDestinationSelectionList();
        updateState(CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION, true);
        screen->forceDisplay();
        return true;
    }
#endif

#if defined(USE_VIRTUAL_KEYBOARD)
    // Cancel (dismiss freetext screen). A swipe classified by the touch layer arrives as
    // INPUT_BROKER_LEFT too, so on a board whose keyboard pans horizontally that gesture would
    // close the composer instead of sliding the keys. Only a physical left key cancels there;
    // the touch route out is the ESC key, which is on the keyboard itself.
    if (event->inputEvent == INPUT_BROKER_LEFT &&
        !(BASEUI_KEYBOARD_ZOOM_PCT > 100 && event->source && strcmp(event->source, "touchscreen1") == 0)) {
        updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
        freetext = "";
        cursor = 0;
        payload = 0;
        currentMessageIndex = -1;

        // Notify UI that we want to redraw/close this screen
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        screen->forceDisplay();
        return true;
    }
#if BASEUI_HAS_TOUCH_DRAG
    // Drag reports are swallowed whole while the composer is open, not just once the pan has
    // committed to an axis. Screen's finger-tracked frame transition anchors on the same first
    // report and locks its axis at the same 10px, so leaving the early reports to fall through
    // let it start paging out from under the keyboard - and once this handler did claim the
    // gesture, that half-run transition was left stranded mid-slide. Claiming from the first
    // report means no frame transition can begin behind the keyboard at all.
    //
    // Panning itself still only happens on a horizontal gesture, and only when the grid is
    // actually wider than the screen; a vertical drag is simply consumed and ignored.
    if (event->inputEvent == INPUT_BROKER_TOUCH_DRAG) {
        if (keyboardPanDragUpdate(event, this))
            // runNow() rather than forceDisplay(): the pan needs the transition framerate held
            // for the whole gesture, not a single redraw per report. screenDragOwnsFramerate()
            // keeps Screen from demoting it again while the finger is still steering.
            screen->runNow();
        return true;
    }
    if (event->inputEvent == INPUT_BROKER_TOUCH_DRAG_END) {
        keyboardPanDragEnd();
        return true;
    }
#endif

    // Touch input (virtual keyboard) handling
    // Only handle if touch coordinates present (CardKB won't set these)
    if (event->touchX != 0 || event->touchY != 0) {
        // A pan that ends over a key would otherwise be taken as a tap on it.
        if (isKeyboardPanFingerSteering())
            return true;
        String keyTapped = keyForCoordinates(event->touchX, event->touchY);
        bool valid = false;

#ifndef EXCLUDE_EMOJI
        if (keyTapped == "\U0001F60A") {
            highlight = "";
            payload = 0x00;
            updateState(CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER, true);
            screen->forceDisplay();
            return true;
        }
#endif
        if (keyTapped == "⇧") {
            highlight = "";
            payload = 0x00;
            shift = !shift;
            valid = true;
        } else if (keyTapped == "⌫") {
#ifndef RAK14014
            highlight = keyTapped;
#endif
            payload = 0x08;
            shift = false;
            valid = true;
        } else if (keyTapped == "ESC") {
            updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
            freetext = "";
            cursor = 0;
            payload = 0;
            currentMessageIndex = -1;
            shift = false;

            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e);
            screen->forceDisplay();
            return true;
        } else if (keyTapped == "SPACE" || keyTapped == " ") {
#ifndef RAK14014
            highlight = keyTapped;
#endif
            payload = ' ';
            shift = false;
            valid = true;
        }
        // Touch enter/submit
        else if (keyTapped == "↵") {
            updateState(CANNED_MESSAGE_RUN_STATE_ACTION_SELECT); // Send the message!
            payload = CANNED_MESSAGE_RUN_STATE_FREETEXT;
            currentMessageIndex = -1;
            shift = false;
            valid = true;
        } else if (!(keyTapped == "")) {
#ifndef RAK14014
            highlight = keyTapped;
#endif
            // Letters case-shift; punctuation and digits reach their second glyph instead
            const char c = keyTapped[0];
            const char shifted = shift ? shiftedSymbol(c) : 0;
            if (shifted)
                payload = shifted;
            else
                payload = shift ? c : std::tolower(c);
            shift = false;
            valid = true;
        }

        if (valid) {
            lastTouchMillis = millis();
            runOnce();
            payload = 0;
            return true; // STOP: We handled a VKB touch
        }

        // Touch landed in the gap between keys. Still ours: the keyboard covers the screen, so
        // letting it through would page the frame or open a long-press menu behind the composer.
        return true;
    }
#endif // USE_VIRTUAL_KEYBOARD

    // Devices that type on hardware keys but still have a pointer (T-Deck and friends, plus the
    // native/SDL build). The composer is otherwise entirely keyboard-driven, so the emote button
    // drawn in drawFrame() is the only touch target left below the header; everything else is
    // swallowed rather than left to open a long-press menu over the draft.
#if CANNED_MESSAGE_HAS_EMOTE_BUTTON
    if (event->touchX != 0 || event->touchY != 0) {
        if (graphics::numEmotes > 0 && event->touchX >= displayWidth - EMOTE_BUTTON_SIZE - EMOTE_BUTTON_MARGIN &&
            event->touchY >= displayHeight - EMOTE_BUTTON_SIZE - EMOTE_BUTTON_MARGIN) {
            updateState(CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER, true);
            screen->forceDisplay();
            return true;
        }

        return true;
    }
#endif // CANNED_MESSAGE_HAS_EMOTE_BUTTON

    // All hardware keys fall through to here (CardKB, physical, etc.)

    if (event->kbchar == INPUT_BROKER_MSG_EMOTE_LIST) {
        if (graphics::numEmotes > 0) { // no picker on EXCLUDE_EMOJI builds (empty emotes[])
            updateState(CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER);
            screen->forceDisplay();
        }
        return true;
    }
    // Confirm select (Enter)
    bool isSelect = isSelectEvent(event);
    if (isSelect) {
        LOG_DEBUG("[SELECT] handleFreeTextInput: runState=%d, dest=%u, channel=%d, freetext='%s'", (int)runState, dest, channel,
                  freetext.c_str());
        if (dest == 0)
            dest = NODENUM_BROADCAST;
        // Defensive: If channel isn't valid, pick the first available channel
        if (channel >= channels.getNumChannels())
            channel = 0;

        payload = CANNED_MESSAGE_RUN_STATE_FREETEXT;
        currentMessageIndex = -1;
        updateState(CANNED_MESSAGE_RUN_STATE_ACTION_SELECT);
        lastTouchMillis = millis();
        runOnce();
        return true;
    }

    // Backspace
    if (event->inputEvent == INPUT_BROKER_BACK && this->freetext.length() > 0) {
        payload = 0x08;
        lastTouchMillis = millis();
        requestFocus();
        runOnce();
        return true;
    }

    // Move cursor left
    if (event->inputEvent == INPUT_BROKER_LEFT) {
        payload = INPUT_BROKER_LEFT;
        lastTouchMillis = millis();
        requestFocus();
        runOnce();
        return true;
    }
    // Move cursor right
    if (event->inputEvent == INPUT_BROKER_RIGHT) {
        payload = INPUT_BROKER_RIGHT;
        lastTouchMillis = millis();
        requestFocus();
        runOnce();
        return true;
    }

    // Cancel (dismiss freetext screen)
    if (event->inputEvent == INPUT_BROKER_CANCEL || event->inputEvent == INPUT_BROKER_ALT_LONG ||
        (event->inputEvent == INPUT_BROKER_BACK && this->freetext.length() == 0)) {
        updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
        freetext = "";
        cursor = 0;
        payload = 0;
        currentMessageIndex = -1;

        // Notify UI that we want to redraw/close this screen
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
        screen->forceDisplay();
        return true;
    }

    // Tab (switch destination)
    if (event->kbchar == INPUT_BROKER_MSG_TAB) {
        return handleTabSwitch(event); // Reuse tab logic
    }

    // Printable ASCII (add char to draft)
    if (event->kbchar >= 32 && event->kbchar <= 126) {
        payload = event->kbchar;
        lastTouchMillis = millis();
        runOnce();
        return true;
    }

    return false;
}

// emotes[] maps several Unicode labels onto one bitmap - seven hearts, two snowmen, two sunrises.
// A list showing labels beside the artwork made that legible; a grid showing artwork alone just
// looks like it repeats, so collapse to first occurrences. The table is const, so this runs once.
static const std::vector<uint16_t> &uniqueEmoteIndices()
{
    static std::vector<uint16_t> unique;
    static bool built = false;

    if (!built) {
        built = true;
        for (int i = 0; i < graphics::numEmotes; ++i) {
            bool duplicate = false;
            for (uint16_t seen : unique) {
                if (graphics::emotes[seen].bitmap == graphics::emotes[i].bitmap) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                unique.push_back((uint16_t)i);
        }
    }
    return unique;
}

// Scroll offset (in rows) that puts `row` in the middle of the viewport, clamped to the grid's ends.
static float emoteScrollToCenter(int row, int rows, int totalRows)
{
    const float offset = row - rows / 2.0f;
    const float maxOffset = (float)std::max(0, totalRows - rows);
    return std::min(std::max(offset, 0.0f), maxOffset);
}

// Touch scroll - moves the grid by an exact finger displacement in screen pixels, for hardware that
// reports a continuous drag (BASEUI_HAS_TOUCH_DRAG). Pass the delta between consecutive drag
// reports, not the offset from where the finger landed.
//
// The grid follows the finger, which is the opposite sense to the arrow keys: a press means "move
// the selection that way", a finger means "drag the grid that way". Same as MapRenderer's pan and
// the message list's scroll. Deliberately leaves the selection alone - scrolling past the
// highlighted emote is normal, and it is the next tap that picks a different one.
void CannedMessageModule::scrollEmotesByFingerDelta(float dyPx)
{
    if (dyPx == 0.0f || emoteCellSize <= 0 || emoteGridCols <= 0)
        return;

    const int numUnique = (int)uniqueEmoteIndices().size();
    const int totalRows = (numUnique + emoteGridCols - 1) / emoteGridCols;
    const float maxScrollOffset = (float)std::max(0, totalRows - emoteGridRows);

    // Screen y grows downward, so dragging down (dyPx > 0) walks back towards the first row
    const float next = emoteScrollOffset - dyPx / emoteCellSize;
    emoteScrollOffset = std::min(std::max(next, 0.0f), maxScrollOffset);
}

void CannedMessageModule::panKeyboardByFingerDelta(float dxPx)
{
    if (dxPx == 0.0f || keyboardMinPanX >= 0)
        return; // not zoomed: there is nothing off-screen to pan to

    const float next = keyboardPanX + dxPx;
    keyboardPanX = std::min(std::max(next, (float)keyboardMinPanX), 0.0f);
}

#if BASEUI_HAS_TOUCH_DRAG
// ---- Finger-tracked keyboard panning -----------------------------------------------------------
//
// Same driver as the emote grid below, mirrored: the keyboard is wider than the screen rather than
// taller, so this one claims the horizontal half. Only active when the grid is actually zoomed -
// otherwise a horizontal drag still belongs to the frame transition, as it does everywhere else.
static bool kbDragAnchorValid = false;
static uint16_t kbDragAnchorX = 0;
static uint16_t kbDragAnchorY = 0;
static uint16_t kbDragLastX = 0;
static uint32_t kbDragLastMs = 0;
static int8_t kbDragAxis = 0; // 0 undecided, 1 horizontal (ours), -1 vertical (not ours)

#define KB_DRAG_AXIS_LOCK_PX 10
#define KB_DRAG_ANCHOR_STALE_MS 1000

static bool keyboardPanDragUpdate(const InputEvent *event, CannedMessageModule *module)
{
    const uint32_t now = millis();
    if (!kbDragAnchorValid || (now - kbDragLastMs) > KB_DRAG_ANCHOR_STALE_MS) {
        kbDragAnchorValid = true;
        kbDragAnchorX = event->touchX;
        kbDragAnchorY = event->touchY;
        kbDragLastX = event->touchX;
        kbDragLastMs = now;
        kbDragAxis = 0;
        return false; // this report only establishes where the finger started
    }
    kbDragLastMs = now;

    if (kbDragAxis == 0) {
        const int32_t dx = (int32_t)event->touchX - (int32_t)kbDragAnchorX;
        const int32_t dy = (int32_t)event->touchY - (int32_t)kbDragAnchorY;
        if (abs(dx) < KB_DRAG_AXIS_LOCK_PX && abs(dy) < KB_DRAG_AXIS_LOCK_PX)
            return false; // too early to tell which way this gesture is going
        kbDragAxis = (abs(dx) > abs(dy)) ? 1 : -1;
    }
    if (kbDragAxis < 0)
        return false; // vertical: not ours

    const float dx = (float)((int32_t)event->touchX - (int32_t)kbDragLastX);
    kbDragLastX = event->touchX;
    module->panKeyboardByFingerDelta(dx);
    return true;
}

// When the gesture that just ended was a pan. The touch layer still classifies a tap on release,
// so without a window here letting go over a key typed it.
static uint32_t kbPanEndedMs = 0;
#define KB_PAN_TAP_SUPPRESS_MS 400

static bool keyboardPanDragEnd()
{
    const bool claimed = kbDragAnchorValid && kbDragAxis > 0;
    if (claimed)
        kbPanEndedMs = millis();
    kbDragAnchorValid = false;
    kbDragAxis = 0;
    return claimed;
}
#endif // BASEUI_HAS_TOUCH_DRAG

bool isKeyboardPanFingerSteering()
{
#if BASEUI_HAS_TOUCH_DRAG
    // Includes a short tail after the finger lifts: keyboardPanDragEnd() clears the anchor
    // immediately, but the tap the touch layer classifies on release arrives after that.
    if (kbPanEndedMs && (millis() - kbPanEndedMs) <= KB_PAN_TAP_SUPPRESS_MS)
        return true;
    return kbDragAnchorValid && (millis() - kbDragLastMs) <= KB_DRAG_ANCHOR_STALE_MS;
#else
    return false;
#endif
}

#if BASEUI_HAS_TOUCH_DRAG
// ---- Finger-tracked emote grid scrolling -------------------------------------------------------
//
// The same driver the map and the message list use (mapPanDragUpdate()/messageScrollDragUpdate() in
// Screen.cpp): _TOUCH_DRAG reports an absolute position rather than a delta, so the first report
// only anchors, the gesture commits to an axis before anything moves, and the grid is then fed the
// displacement between consecutive reports. Like the message list it shares its frame with
// left/right paging, so it claims only the vertical half and leaves horizontal to the transition.
//
// Lives here rather than beside the others in Screen.cpp purely because a module sees input first -
// Screen is the last handler, and never sees a drag we have already claimed.
//
// Constants match Screen.cpp's SCREEN_DRAG_AXIS_LOCK_PX and DRAG_ANCHOR_STALE_MS: how far a finger
// travels before a drag commits should not depend on which frame it lands on.
#define EMOTE_DRAG_AXIS_LOCK_PX 10
#define EMOTE_DRAG_ANCHOR_STALE_MS 1000

static bool emoteDragAnchorValid = false;
static uint16_t emoteDragAnchorX = 0;
static uint16_t emoteDragAnchorY = 0;
static uint16_t emoteDragLastY = 0;
static uint32_t emoteDragLastMs = 0;
static int8_t emoteDragAxis = 0; // 0 undecided, 1 vertical (ours), -1 horizontal (not ours)

// Returns true if this report belongs to the grid, false to leave it for the frame transition.
static bool emoteScrollDragUpdate(const InputEvent *event, CannedMessageModule *module)
{
    const uint32_t now = millis();
    if (!emoteDragAnchorValid || (now - emoteDragLastMs) > EMOTE_DRAG_ANCHOR_STALE_MS) {
        emoteDragAnchorValid = true;
        emoteDragAnchorX = event->touchX;
        emoteDragAnchorY = event->touchY;
        emoteDragLastY = event->touchY;
        emoteDragLastMs = now;
        emoteDragAxis = 0;
        return false; // this report only establishes where the finger started
    }
    emoteDragLastMs = now;

    if (emoteDragAxis == 0) {
        const int32_t dx = (int32_t)event->touchX - (int32_t)emoteDragAnchorX;
        const int32_t dy = (int32_t)event->touchY - (int32_t)emoteDragAnchorY;
        if (abs(dx) < EMOTE_DRAG_AXIS_LOCK_PX && abs(dy) < EMOTE_DRAG_AXIS_LOCK_PX)
            return false; // too early to tell which way this gesture is going
        emoteDragAxis = (abs(dy) > abs(dx)) ? 1 : -1;
    }
    if (emoteDragAxis < 0)
        return false; // horizontal: the frame transition owns it

    const float dy = (float)((int32_t)event->touchY - (int32_t)emoteDragLastY);
    emoteDragLastY = event->touchY;
    module->scrollEmotesByFingerDelta(dy);
    return true;
}

static bool emoteScrollDragEnd()
{
    const bool claimed = emoteDragAnchorValid && emoteDragAxis > 0;
    emoteDragAnchorValid = false;
    emoteDragAxis = 0;
    return claimed;
}
#endif // BASEUI_HAS_TOUCH_DRAG

bool isEmoteScrollFingerSteering()
{
#if BASEUI_HAS_TOUCH_DRAG
    // Derived from how recently a report arrived rather than a start/end flag, for the same reason
    // Screen.cpp's own anchors are: a gesture can end somewhere we never see, and a flag left stuck
    // true would pin the screen at the drag framerate indefinitely.
    return emoteDragAnchorValid && (millis() - emoteDragLastMs) <= EMOTE_DRAG_ANCHOR_STALE_MS;
#else
    return false;
#endif
}

int CannedMessageModule::handleEmotePickerInput(const InputEvent *event)
{
    const std::vector<uint16_t> &unique = uniqueEmoteIndices();
    const int numUnique = (int)unique.size();
    if (numUnique == 0) { // EXCLUDE_EMOJI: emotes[] is empty, any index would read out of bounds
        updateState(CANNED_MESSAGE_RUN_STATE_FREETEXT, true);
        // forceDisplay() without the flag is a no-op on non-eink, and the picker returns 1 for every
        // event it handles - so Screen, a later observer on the same broker, never sees the input and
        // never speeds itself up. Without this the redraw waits for the 1fps idle tick.
        screen->forceDisplay(true);
        return 1;
    }

    // The grid is laid out by drawEmotePickerScreen(); until it has run once - and normalised the
    // scroll offset that opening the picker invalidated - there is nothing meaningful to navigate,
    // and no touch can have landed on a cell that is not on screen yet.
    if (emoteCellSize == 0 || emoteGridCols == 0 || emoteGridRows == 0 || emoteScrollOffset < 0)
        return 0;

    const int cols = emoteGridCols;
    const int rows = emoteGridRows;
    const int totalRows = (numUnique + cols - 1) / cols;

    // The grid needs LEFT/RIGHT to mean left and right, so read the raw directions rather than
    // isUpEvent()/isDownEvent() - those fold LEFT into "up" for the list-shaped screens.
    const bool isUp = event->inputEvent == INPUT_BROKER_UP;
    const bool isDown = event->inputEvent == INPUT_BROKER_DOWN;
    const bool isLeft = event->inputEvent == INPUT_BROKER_LEFT;
    const bool isRight = event->inputEvent == INPUT_BROKER_RIGHT;
    const bool isSelect = isSelectEvent(event);
    const bool hasTouch = event->touchX != 0 || event->touchY != 0;

#if BASEUI_HAS_TOUCH_DRAG
    // Only swallowed when the grid claimed it; a horizontal drag falls through to page the frame.
    if (event->inputEvent == INPUT_BROKER_TOUCH_DRAG && emoteScrollDragUpdate(event, this)) {
        requestFocus();
        screen->forceDisplay(true);
        return 1;
    }
    if (event->inputEvent == INPUT_BROKER_TOUCH_DRAG_END && emoteScrollDragEnd()) {
        screen->forceDisplay(true);
        return 1;
    }
    // The grid is finger-tracked, so the swipe classified on release must not scroll a step on top
    // of it - nor for a flick too quick to have produced any drag report at all.
    if (hasTouch && (isUp || isDown))
        return 1;
#endif

    // Touch is two-stage: a tap moves the highlight, a long press on it commits. Insert-on-tap
    // would put an emote in the draft every time a finger brushed the grid on the way to scrolling
    // it, and the picker is full of targets barely wider than a fingertip.
    int touchedIdx = -1;
    if (hasTouch && (event->inputEvent == INPUT_BROKER_USER_PRESS || isSelect)) {
        const int topRow = (int)emoteScrollOffset;
        const int pixelOffset = (int)((emoteScrollOffset - topRow) * emoteCellSize);
        const int col = (event->touchX - emoteGridX) / emoteCellSize;
        const int row = (event->touchY - emoteGridTop + pixelOffset) / emoteCellSize;
        if (event->touchX >= emoteGridX && col >= 0 && col < cols && event->touchY >= emoteGridTop &&
            event->touchY < emoteGridBottom) {
            const int idx = (topRow + row) * cols + col;
            if (idx >= 0 && idx < numUnique)
                touchedIdx = idx;
        }
        if (touchedIdx < 0)
            return 1; // tap on the header or past the last emote: swallow it, don't page the frame

        // Highlight only - and leave the scroll where the finger left it, rather than recentring
        // and sliding the grid out from under a selection the user can already see.
        if (!isSelect) {
            if (touchedIdx != emotePickerIndex) {
                emotePickerIndex = touchedIdx;
                requestFocus();
                screen->forceDisplay(true);
            }
            return 1;
        }
    }

    // Single-button devices have no direction to give, so a short press steps to the next emote -
    // the same meaning it carried in the list-shaped picker this grid replaced.
    if (!hasTouch && event->inputEvent == INPUT_BROKER_USER_PRESS) {
        if (emotePickerIndex < numUnique - 1) {
            emotePickerIndex++;
            emoteScrollOffset = emoteScrollToCenter(emotePickerIndex / cols, rows, totalRows);
            requestFocus();
            screen->forceDisplay(true);
        }
        return 1;
    }

    // Directional events carrying touch coordinates are the swipe classified on release. The grid
    // already followed the finger, so acting on them too would scroll a second time.
    if (!hasTouch && (isUp || isDown || isLeft || isRight)) {
        const int currentCol = emotePickerIndex % cols;
        int target = emotePickerIndex;

        if (isUp && emotePickerIndex >= cols) {
            target = emotePickerIndex - cols;
        } else if (isDown) {
            // On the last row, fall to the final emote rather than refusing to move
            const int below = emotePickerIndex + cols;
            target = (below < numUnique) ? below : std::max(emotePickerIndex, numUnique - 1);
        } else if (isLeft && currentCol > 0) {
            target = emotePickerIndex - 1;
        } else if (isRight && currentCol < cols - 1 && emotePickerIndex < numUnique - 1) {
            target = emotePickerIndex + 1;
        }

        if (target != emotePickerIndex) {
            emotePickerIndex = target;
            emoteScrollOffset = emoteScrollToCenter(emotePickerIndex / cols, rows, totalRows);
            requestFocus();
            screen->forceDisplay(true);
        }
        return 1;
    }

    // Confirm: insert the selected emote's label at the cursor and return to the composer. Reached
    // by a button press, or by a long press on the grid - which commits whatever it landed on, so
    // one long press can both pick and confirm without a separate tap first.
    if (isSelect) {
        if (touchedIdx >= 0)
            emotePickerIndex = touchedIdx;

        String emoteInsert = graphics::emotes[unique[emotePickerIndex]].label;
        if (cursor == freetext.length()) {
            freetext += emoteInsert;
        } else {
            freetext = freetext.substring(0, cursor) + emoteInsert + freetext.substring(cursor);
        }
        cursor += emoteInsert.length();
        updateState(CANNED_MESSAGE_RUN_STATE_FREETEXT, true);
        screen->forceDisplay(true);
        return 1;
    }

    // Cancel or backspace returns to freetext
    if (event->inputEvent == INPUT_BROKER_CANCEL || event->inputEvent == INPUT_BROKER_ALT_LONG ||
        event->inputEvent == INPUT_BROKER_BACK) {
        updateState(CANNED_MESSAGE_RUN_STATE_FREETEXT, true);
        screen->forceDisplay(true);
        return 1;
    }

    return 0;
}

void CannedMessageModule::sendText(NodeNum dest, ChannelIndex channel, const char *message, bool wantReplies)
{
    lastDest = dest;
    lastChannel = channel;
    lastDestSet = true;

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;
    p->to = dest;
    p->channel = channel;
    p->want_ack = true;
    p->decoded.dest = dest; // Mirror picker: NODENUM_BROADCAST or node->num

    this->lastSentNode = dest;
    this->incoming = dest;

    // Manually find the node by number to check PKI capability
    meshtastic_NodeInfoLite *node = nullptr;
    size_t numMeshNodes = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < numMeshNodes; ++i) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (n && n->num == dest) {
            node = n;
            break;
        }
    }

    NodeNum myNodeNum = nodeDB->getNodeNum();
    if (node && node->num != myNodeNum && nodeInfoLiteHasUser(node) && node->public_key.size == 32) {
        p->pki_encrypted = true;
        p->channel = 0; // force PKI
    }

    // Track this packet’s request ID for matching ACKs
    this->lastRequestId = p->id;

    // Copy payload
    p->decoded.payload.size = strlen(message);
    memcpy(p->decoded.payload.bytes, message, p->decoded.payload.size);

    if (moduleConfig.canned_message.send_bell && p->decoded.payload.size + 1 < meshtastic_Constants_DATA_PAYLOAD_LEN) {
        p->decoded.payload.bytes[p->decoded.payload.size++] = 7;
        p->decoded.payload.bytes[p->decoded.payload.size] = '\0';
    }

    this->waitingForAck = true;

    // Send to mesh (PKI-encrypted if conditions above matched)
    service->sendToMesh(p, RX_SRC_LOCAL, true);

    // Show banner immediately
    if (screen) {
        graphics::BannerOverlayOptions opts;
        opts.message = "Sending...";
        opts.durationMs = 2000;
        screen->showOverlayBanner(opts);
    }

    // Save outgoing message
    StoredMessage sm;

    // Always use our local time, consistent with other paths
    uint32_t nowSecs = getValidTime(RTCQuality::RTCQualityDevice, true);
    sm.timestamp = (nowSecs > 0) ? nowSecs : millis() / 1000;
    sm.isBootRelative = (nowSecs == 0);

    sm.sender = nodeDB->getNodeNum(); // us
    sm.channelIndex = channel;
    size_t len = strnlen(message, MAX_MESSAGE_SIZE - 1);
    sm.textOffset = MessageStore::storeText(message, len);
    sm.textLength = len;

    // Classify broadcast vs DM
    if (dest == NODENUM_BROADCAST) {
        sm.dest = NODENUM_BROADCAST;
        sm.type = MessageType::BROADCAST;
    } else {
        sm.dest = dest;
        sm.type = MessageType::DM_TO_US;
        // Only add as favorite if our role is not router-like (ROUTER, ROUTER_LATE, CLIENT_BASE)
        if (config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER &&
            config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER_LATE &&
            config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_BASE) {
            LOG_INFO("Proactively adding 0x%08x as favorite node", dest);
            nodeDB->set_favorite(true, dest);
        } else {
            LOG_DEBUG("Not favoriting node 0x%08x: router-like role", dest);
        }
    }
    sm.ackStatus = AckStatus::NONE;

    messageStore.addLiveMessage(std::move(sm));

    // Auto-switch thread view on outgoing message
    if (sm.type == MessageType::BROADCAST) {
        graphics::MessageRenderer::setThreadMode(graphics::MessageRenderer::ThreadMode::CHANNEL, sm.channelIndex);
    } else {
        graphics::MessageRenderer::setThreadMode(graphics::MessageRenderer::ThreadMode::DIRECT, -1, sm.dest);
    }

    playComboTune();

    this->updateState(CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE);
    this->payload = wantReplies ? 1 : 0;

    // Tell Screen to switch to TextMessage frame via UIFrameEvent
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::SWITCH_TO_TEXTMESSAGE;
    notifyObservers(&e);
}

int32_t CannedMessageModule::runOnce()
{
    if (this->runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION && needsUpdate) {
        updateDestinationSelectionList();
        needsUpdate = false;
    }

    // If we're in node selection, do nothing except keep alive
    if (this->runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION) {
        return INACTIVATE_AFTER_MS;
    }

    // Normal module disable/idle handling
    if ((this->runState == CANNED_MESSAGE_RUN_STATE_DISABLED) || (this->runState == CANNED_MESSAGE_RUN_STATE_INACTIVE)) {
        // Clean up virtual keyboard if needed when going inactive
        if (graphics::NotificationRenderer::virtualKeyboard && graphics::NotificationRenderer::textInputCallback == nullptr) {
            LOG_INFO("Performing delayed virtual keyboard cleanup");
            graphics::OnScreenKeyboardModule::instance().stop(false);
        }

        return INT32_MAX;
    }

    // Handle delayed virtual keyboard message sending
    if (this->runState == CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE && this->payload == CANNED_MESSAGE_RUN_STATE_FREETEXT) {
        // Virtual keyboard message sending case - text was not empty
        if (this->freetext.length() > 0) {
            LOG_INFO("Delayed vkbd send: '%s'", this->freetext.c_str());
            sendText(this->dest, this->channel, this->freetext.c_str(), true);

            // Clean up virtual keyboard after sending
            if (graphics::NotificationRenderer::virtualKeyboard) {
                LOG_INFO("Vkbd cleanup after send");
                graphics::OnScreenKeyboardModule::instance().stop(false);
                graphics::NotificationRenderer::resetBanner();
            }

            // Clear payload to indicate virtual keyboard processing is complete
            // But keep SENDING_ACTIVE state to show "Sending..." screen for 2 seconds
            this->payload = 0;
        } else {
            // Empty message, just go inactive
            LOG_INFO("Empty freetext, back to inactive");
            this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
        }

        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        this->currentMessageIndex = -1;
        this->freetext = "";
        this->cursor = 0;
        this->notifyObservers(&e);
        return 2000;
    }

    UIFrameEvent e;
    if ((this->runState == CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE && this->payload != 0 &&
         this->payload != CANNED_MESSAGE_RUN_STATE_FREETEXT) ||
        (this->runState == CANNED_MESSAGE_RUN_STATE_ACK_NACK_RECEIVED) ||
        (this->runState == CANNED_MESSAGE_RUN_STATE_MESSAGE_SELECTION)) {
        this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        this->currentMessageIndex = -1;
        this->freetext = "";
        this->cursor = 0;
        this->notifyObservers(&e);
    }
    // Handle SENDING_ACTIVE state transition after virtual keyboard message
    else if (this->runState == CANNED_MESSAGE_RUN_STATE_SENDING_ACTIVE && this->payload == 0) {
        this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
        this->currentMessageIndex = -1;
        this->freetext = "";
        this->cursor = 0;
        return INT32_MAX;
    } else if (((this->runState == CANNED_MESSAGE_RUN_STATE_ACTIVE) || (this->runState == CANNED_MESSAGE_RUN_STATE_FREETEXT)) &&
               !Throttle::isWithinTimespanMs(this->lastTouchMillis, INACTIVATE_AFTER_MS)) {
        // Reset module on inactivity
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        this->currentMessageIndex = -1;
        this->freetext = "";
        this->cursor = 0;
        this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);

        // Clean up virtual keyboard if it exists during timeout
        if (graphics::NotificationRenderer::virtualKeyboard) {
            LOG_INFO("Vkbd cleanup on timeout");
            graphics::OnScreenKeyboardModule::instance().stop(false);
            graphics::NotificationRenderer::resetBanner();
        }

        this->notifyObservers(&e);
    } else if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTION_SELECT) {
        if (this->payload == 0) {
            // [Exit] button pressed - return to inactive state
            LOG_INFO("Exit action, back to inactive");
            this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
        } else if (this->payload == CANNED_MESSAGE_RUN_STATE_FREETEXT) {
            if (this->freetext.length() > 0) {
                sendText(this->dest, this->channel, this->freetext.c_str(), true);

                // Clean up state but *don’t* deactivate yet
                this->currentMessageIndex = -1;
                this->freetext = "";
                this->cursor = 0;

                // Tell Screen to jump straight to the TextMessage frame
                e.action = UIFrameEvent::Action::SWITCH_TO_TEXTMESSAGE;
                this->notifyObservers(&e);

                // Now deactivate this module
                this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);

                return INT32_MAX; // don't fall back into canned list
            } else {
                this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
            }
        } else {
            if (strcmp(this->messages[this->currentMessageIndex], "[Select Destination]") == 0) {
                this->updateState(CANNED_MESSAGE_RUN_STATE_ACTIVE);
                return INT32_MAX;
            }
            if ((this->messagesCount > this->currentMessageIndex) && (strlen(this->messages[this->currentMessageIndex]) > 0)) {
                if (strcmp(this->messages[this->currentMessageIndex], "~") == 0) {
                    return INT32_MAX;
                } else {
                    sendText(this->dest, this->channel, this->messages[this->currentMessageIndex], true);

                    // Clean up state
                    this->currentMessageIndex = -1;
                    this->freetext = "";
                    this->cursor = 0;

                    // Tell Screen to jump straight to the TextMessage frame
                    e.action = UIFrameEvent::Action::SWITCH_TO_TEXTMESSAGE;
                    this->notifyObservers(&e);

                    // Now deactivate this module
                    this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);

                    return INT32_MAX; // don't fall back into canned list
                }
            } else {
                this->updateState(CANNED_MESSAGE_RUN_STATE_INACTIVE);
            }
        }
        // fallback clean-up if nothing above returned
        this->currentMessageIndex = -1;
        this->freetext = "";
        this->cursor = 0;

        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        this->notifyObservers(&e);

        // Immediately stop, don't linger on canned screen
        return INT32_MAX;
    }
    // Highlight [Select Destination] initially when entering the message list
    else if ((this->runState != CANNED_MESSAGE_RUN_STATE_FREETEXT) && (this->currentMessageIndex == -1)) {
        // Only auto-highlight [Select Destination] if we’re ACTIVELY browsing,
        // not when coming back from a sent message.
        if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTIVE) {
            int selectDestination = 0;
            for (int i = 0; i < this->messagesCount; ++i) {
                if (strcmp(this->messages[i], "[Select Destination]") == 0) {
                    selectDestination = i;
                    break;
                }
            }
            this->currentMessageIndex = selectDestination;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        }
    } else if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTION_UP) {
        if (this->messagesCount > 0) {
            this->currentMessageIndex = getPrevIndex();
            this->freetext = "";
            this->cursor = 0;
            this->updateState(CANNED_MESSAGE_RUN_STATE_ACTIVE);
        }
    } else if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTION_DOWN) {
        if (this->messagesCount > 0) {
            this->currentMessageIndex = this->getNextIndex();
            this->freetext = "";
            this->cursor = 0;
            this->updateState(CANNED_MESSAGE_RUN_STATE_ACTIVE);
        }
    } else if (this->runState == CANNED_MESSAGE_RUN_STATE_FREETEXT || this->runState == CANNED_MESSAGE_RUN_STATE_ACTIVE) {
        switch (this->payload) {
        case INPUT_BROKER_LEFT:
            if (this->runState == CANNED_MESSAGE_RUN_STATE_FREETEXT && this->cursor > 0) {
                this->cursor--;
            }
            break;
        case INPUT_BROKER_RIGHT:
            if (this->runState == CANNED_MESSAGE_RUN_STATE_FREETEXT && this->cursor < this->freetext.length()) {
                this->cursor++;
            }
            break;
        default:
            break;
        }
        if (this->runState == CANNED_MESSAGE_RUN_STATE_FREETEXT) {
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            switch (this->payload) {
            case 0x08: // backspace
                if (this->freetext.length() > 0) {
                    if (this->cursor > 0) {
                        if (this->cursor == this->freetext.length()) {
                            this->freetext = this->freetext.substring(0, this->freetext.length() - 1);
                        } else {
                            this->freetext = this->freetext.substring(0, this->cursor - 1) +
                                             this->freetext.substring(this->cursor, this->freetext.length());
                        }
                        this->cursor--;
                    }
                } else {
                }
                break;
            case INPUT_BROKER_MSG_TAB: // Tab key: handled by input handler
                return 0;
            case INPUT_BROKER_LEFT:
            case INPUT_BROKER_RIGHT:
                break;
            default:
                // Only insert ASCII printable characters (32-126)
                if (this->payload >= 32 && this->payload <= 126) {
                    requestFocus();
                    if (this->cursor == this->freetext.length()) {
                        this->freetext += (char)this->payload;
                    } else {
                        this->freetext = this->freetext.substring(0, this->cursor) + (char)this->payload +
                                         this->freetext.substring(this->cursor);
                    }
                    this->cursor++;
                    const uint16_t maxChars = 200 - (moduleConfig.canned_message.send_bell ? 1 : 0);
                    if (this->freetext.length() > maxChars) {
                        this->cursor = maxChars;
                        this->freetext = this->freetext.substring(0, maxChars);
                    }
                }
                break;
            }
        }
        this->lastTouchMillis = millis();
        this->notifyObservers(&e);
        return INACTIVATE_AFTER_MS;
    }

    if (this->runState == CANNED_MESSAGE_RUN_STATE_ACTIVE) {
        this->lastTouchMillis = millis();
        this->notifyObservers(&e);
        return INACTIVATE_AFTER_MS;
    }
    return INT32_MAX;
}

const char *CannedMessageModule::getCurrentMessage()
{
    return this->messages[this->currentMessageIndex];
}
const char *CannedMessageModule::getPrevMessage()
{
    return this->messages[this->getPrevIndex()];
}
const char *CannedMessageModule::getNextMessage()
{
    return this->messages[this->getNextIndex()];
}
const char *CannedMessageModule::getMessageByIndex(int index)
{
    return (index >= 0 && index < this->messagesCount) ? this->messages[index] : "";
}

const char *CannedMessageModule::getNodeName(NodeNum node)
{
    if (node == NODENUM_BROADCAST)
        return "Broadcast";

    meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(node);
    if (nodeInfoLiteHasUser(info) && strlen(info->long_name) > 0) {
        return info->long_name;
    }

    static char fallback[12];
    snprintf(fallback, sizeof(fallback), "0x%08x", node);
    return fallback;
}

bool CannedMessageModule::shouldDraw()
{
    // Only allow drawing when we're in an interactive UI state.
    return (this->runState == CANNED_MESSAGE_RUN_STATE_ACTIVE || this->runState == CANNED_MESSAGE_RUN_STATE_FREETEXT ||
            this->runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION ||
            this->runState == CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER);
}

int CannedMessageModule::getNextIndex()
{
    if (this->currentMessageIndex >= (this->messagesCount - 1)) {
        return 0;
    } else {
        return this->currentMessageIndex + 1;
    }
}

int CannedMessageModule::getPrevIndex()
{
    if (this->currentMessageIndex <= 0) {
        return this->messagesCount - 1;
    } else {
        return this->currentMessageIndex - 1;
    }
}

#if defined(USE_VIRTUAL_KEYBOARD)

String CannedMessageModule::keyForCoordinates(uint x, uint y)
{
    int outerSize = *(&this->keyboard[0] + 1) - this->keyboard[0];

    for (int8_t outerIndex = 0; outerIndex < outerSize; outerIndex++) {
        int innerSize = *(&this->keyboard[0][outerIndex] + 1) - this->keyboard[0][outerIndex];

        for (int8_t innerIndex = 0; innerIndex < innerSize; innerIndex++) {
            Letter letter = this->keyboard[0][outerIndex][innerIndex];

            if (letter.character == "")
                continue;

            if (x > (uint)letter.rectX && x < (uint)(letter.rectX + letter.rectWidth) && y > (uint)letter.rectY &&
                y < (uint)(letter.rectY + letter.rectHeight)) {
                return letter.character;
            }
        }
    }

    return "";
}

void CannedMessageModule::drawKeyboard(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    const int outerSize = *(&this->keyboard[0] + 1) - this->keyboard[0];
    char buffer[50];

    display->setColor(OLEDDISPLAY_COLOR::WHITE);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Destination and remaining characters, laid out like the hardware-keyboard composer
    drawHeader(display, x, y, buffer);
    const uint16_t charsLeft =
        meshtastic_Constants_DATA_PAYLOAD_LEN - this->freetext.length() - (moduleConfig.canned_message.send_bell ? 1 : 0);
    snprintf(buffer, sizeof(buffer), "%d left", charsLeft);
    display->drawString(x + display->getWidth() - display->getStringWidth(buffer) - BASEUI_HEADER_LR_MARGIN,
                        y + BASEUI_HEADER_MARGIN, buffer);

    // Draft below the header, with any emotes already in it drawn as artwork
    const int draftX = x + BASEUI_BODY_LR_MARGIN;
    String draft = this->drawWithCursor(this->freetext, this->cursor);
    drawWrappedEmoteText(display, draftX, y + FONT_HEIGHT_SMALL + BASEUI_HEADER_MARGIN, draft.c_str(),
                         display->getWidth() - draftX - BASEUI_BODY_LR_MARGIN, FONT_HEIGHT_SMALL);

    // Keys fill what is left below the draft, inset by whatever the variant asks for. The key grid
    // deliberately ignores BASEUI_BODY_LR_MARGIN - that one insets text, and fourteen columns cannot
    // spare it - so a device that needs its edges kept clear says so with the keyboard margins.
    const int screenWidth = display->getWidth();
    const int screenHeight = display->getHeight();
    const int sideInset = screenWidth * BASEUI_KEYBOARD_LR_MARGIN_PCT / 100;
    const int visibleKeyAreaWidth = screenWidth - sideInset * 2;
    const int keyAreaWidth = visibleKeyAreaWidth * BASEUI_KEYBOARD_ZOOM_PCT / 100;

    // Publish the pan limit for the drag handler, and keep the current offset legal - the layout
    // can change underneath it (rotation, a variant tweak) while a pan is held.
    this->keyboardMinPanX = std::min(0, visibleKeyAreaWidth - keyAreaWidth);
    if (this->keyboardPanX < (float)this->keyboardMinPanX)
        this->keyboardPanX = (float)this->keyboardMinPanX;
    if (this->keyboardPanX > 0.0f)
        this->keyboardPanX = 0.0f;
    const int panX = (int)lroundf(this->keyboardPanX);
    const int keyAreaBottom = screenHeight - screenHeight * BASEUI_KEYBOARD_BOTTOM_MARGIN_PCT / 100;

    int keyFontHeight;
    if (screenWidth >= 400) {
        display->setFont(FONT_MEDIUM);
        keyFontHeight = FONT_HEIGHT_MEDIUM;
    } else if (screenWidth >= 280) {
        display->setFont(FONT_SMALL);
        keyFontHeight = FONT_HEIGHT_SMALL;
    } else {
        display->setFont(FONT_SMALL_LOCAL);
        keyFontHeight = _fontHeight(FONT_SMALL_LOCAL);
    }

    // Rows are sized to the space between the halfway mark and the bottom inset, then the block is
    // anchored to that inset rather than grown downwards - so the padding is what the user sees, and
    // any height the clamp leaves over goes to the draft above instead of a gap underneath.
    const int baseCellHeight = std::min(45, std::max(20, (keyAreaBottom - screenHeight / 2 - 4) / outerSize));
    // Clamp so a tall setting cannot push the rows up over the draft and header.
    const int maxCellHeight = std::max(20, (keyAreaBottom - FONT_HEIGHT_SMALL * 3) / outerSize);
    const int cellHeight = std::min(maxCellHeight, baseCellHeight * BASEUI_KEYBOARD_KEY_HEIGHT_PCT / 100);
    int yOffset = y + keyAreaBottom - cellHeight * outerSize;
    const int buttonPadding = 1;
    const int buttonRadius = 7;

    // Icon artwork is nominally 15x12; scale it with the screen the same way the font is picked
    const float iconScale = (screenWidth >= 400) ? 1.2f : (screenWidth >= 280) ? 1.0f : 0.8f;
    const float enterIconScale = iconScale * 1.4f;
    const int iconWidth = (int)(15 * iconScale);
    const int iconHeight = (int)(12 * iconScale);

    for (int8_t outerIndex = 0; outerIndex < outerSize; outerIndex++) {
        yOffset += outerIndex > 0 ? cellHeight : 0;

        const int innerSizeBound = *(&this->keyboard[0][outerIndex] + 1) - this->keyboard[0][outerIndex];

        int innerSize = 0;
        for (int8_t innerIndex = 0; innerIndex < innerSizeBound; innerIndex++) {
            if (this->keyboard[0][outerIndex][innerIndex].character != "")
                innerSize++;
        }
        if (innerSize == 0)
            continue;

        const bool isBottomRow = outerIndex == outerSize - 1;

        for (int8_t innerIndex = 0; innerIndex < innerSize; innerIndex++) {
            Letter letter = this->keyboard[0][outerIndex][innerIndex];

            int xOffset;
            int cellWidth;
            if (isBottomRow) {
                // Hand-laid: ESC and the emote key are fixed, space takes three quarters of what is
                // left and enter the rest. An even split would make space no easier to hit than 'q'.
                const int escWidth = std::max(30, keyAreaWidth / 5);
                const int emoteWidth = graphics::numEmotes > 0 ? std::max(24, keyAreaWidth / 9) : 0;
                const int remaining = keyAreaWidth - escWidth - emoteWidth;
                const int spaceWidth = (remaining * 3) / 4;
                // Index 1 is the emote key only while it exists; without it space moves up one slot
                const int slot = (emoteWidth == 0 && innerIndex > 0) ? innerIndex + 1 : innerIndex;

                switch (slot) {
                case 0:
                    xOffset = 0;
                    cellWidth = escWidth;
                    break;
                case 1:
                    xOffset = escWidth;
                    cellWidth = emoteWidth;
                    break;
                case 2:
                    xOffset = escWidth + emoteWidth;
                    cellWidth = spaceWidth;
                    break;
                default:
                    xOffset = escWidth + emoteWidth + spaceWidth;
                    cellWidth = remaining - spaceWidth;
                    break;
                }
                xOffset += x + sideInset + panX;
            } else {
                // Distribute from the running total rather than a fixed width, so rounding never
                // leaves a gap at the right edge
                const int startX = (innerIndex * keyAreaWidth) / innerSize;
                const int endX = ((innerIndex + 1) * keyAreaWidth) / innerSize;
                xOffset = x + sideInset + startX + panX;
                cellWidth = endX - startX;
            }

            Letter updatedLetter = {letter.character, letter.width, xOffset, yOffset, cellWidth, cellHeight};
#ifdef RAK14014 // Optimize the touch range of the virtual keyboard in the bottom row
            if (isBottomRow) {
                updatedLetter.rectHeight = 240 - yOffset;
            }
#endif
            this->keyboard[0][outerIndex][innerIndex] = updatedLetter;

            const int capX = xOffset + buttonPadding;
            const int capY = yOffset + buttonPadding;
            const int capW = cellWidth - buttonPadding * 2;
            const int capH = cellHeight - buttonPadding * 2;
            const int centerX = xOffset + cellWidth / 2;
            const int centerY = yOffset + cellHeight / 2;

            // Shift latches, so it stays inverted; every other key inverts only for the frame after
            // it was tapped, as feedback
            const bool isShiftKey = (letter.character == "⇧");
            const bool inverted =
                isShiftKey ? this->shift : (this->highlight.length() > 0 && this->highlight == letter.character);
            if (inverted) {
                fillRoundedRect(display, capX, capY, capW, capH, buttonRadius);
                display->setColor(OLEDDISPLAY_COLOR::BLACK);
                // Only for the transient tap flash, which clears itself at the end of this
                // function. Shift's inversion latches, so asking for an immediate redraw here
                // re-armed a zero interval on every frame for as long as shift was on - the
                // module then span runOnce(), regenerating the frameset continuously, and the
                // UI stopped responding to touches.
                if (!isShiftKey)
                    setIntervalFromNow(0);
            } else {
                drawRoundedRect(display, capX, capY, capW, capH, buttonRadius);
            }

            if (letter.character == "⇧") {
                drawShiftIcon(display, centerX - iconWidth / 2, centerY - iconHeight / 2, iconScale);
            } else if (letter.character == "⌫") {
                drawBackspaceIcon(display, centerX - iconWidth / 2, centerY - iconHeight / 2, iconScale);
            } else if (letter.character == "↵") {
                drawEnterIcon(display, centerX - (int)(15 * enterIconScale) / 2, centerY - (int)(12 * enterIconScale) / 2,
                              enterIconScale);
#ifndef EXCLUDE_EMOJI
            } else if (letter.character == "\U0001F60A") {
                const graphics::Emote *smiley = graphics::EmoteRenderer::findEmoteByLabel("\U0001F60A");
                if (smiley) {
                    graphics::drawScaledXbm(display, centerX - smiley->width * BASEUI_ICON_SCALE / 2,
                                            centerY - smiley->height * BASEUI_ICON_SCALE / 2, smiley->width, smiley->height,
                                            smiley->bitmap);
                }
#endif
            } else {
                String label = letter.character;
                if (label != "ESC" && label != "SPACE") {
                    const char c = letter.character[0];
                    if (c >= 'A' && c <= 'Z') {
                        label = this->shift ? String(c) : String((char)(c + 32));
                    } else if (this->shift) {
                        const char shifted = shiftedSymbol(c);
                        if (shifted)
                            label = String(shifted);
                    }
                }
                display->setTextAlignment(TEXT_ALIGN_CENTER);
                display->drawString(centerX, centerY - keyFontHeight / 2, label);
                display->setTextAlignment(TEXT_ALIGN_LEFT);
            }

            if (inverted)
                display->setColor(OLEDDISPLAY_COLOR::WHITE);
        }
    }

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    this->highlight = "";
}

void CannedMessageModule::drawShiftIcon(OLEDDisplay *display, int x, int y, float scale)
{
    PointStruct shiftIcon[10] = {{8, 0}, {15, 7}, {15, 8}, {12, 8}, {12, 12}, {4, 12}, {4, 8}, {1, 8}, {1, 7}, {8, 0}};

    int size = 10;

    for (int i = 0; i < size - 1; i++) {
        int x0 = x + (shiftIcon[i].x * scale);
        int y0 = y + (shiftIcon[i].y * scale);
        int x1 = x + (shiftIcon[i + 1].x * scale);
        int y1 = y + (shiftIcon[i + 1].y * scale);

        display->drawLine(x0, y0, x1, y1);
    }
}

void CannedMessageModule::drawBackspaceIcon(OLEDDisplay *display, int x, int y, float scale)
{
    PointStruct backspaceIcon[6] = {{0, 7}, {5, 2}, {15, 2}, {15, 12}, {5, 12}, {0, 7}};

    int size = 6;

    for (int i = 0; i < size - 1; i++) {
        int x0 = x + (backspaceIcon[i].x * scale);
        int y0 = y + (backspaceIcon[i].y * scale);
        int x1 = x + (backspaceIcon[i + 1].x * scale);
        int y1 = y + (backspaceIcon[i + 1].y * scale);

        display->drawLine(x0, y0, x1, y1);
    }

    PointStruct backspaceIconX[4] = {{7, 4}, {13, 10}, {7, 10}, {13, 4}};

    size = 4;

    for (int i = 0; i < size - 1; i++) {
        int x0 = x + (backspaceIconX[i].x * scale);
        int y0 = y + (backspaceIconX[i].y * scale);
        int x1 = x + (backspaceIconX[i + 1].x * scale);
        int y1 = y + (backspaceIconX[i + 1].y * scale);

        display->drawLine(x0, y0, x1, y1);
    }
}

void CannedMessageModule::drawEnterIcon(OLEDDisplay *display, int x, int y, float scale)
{
    PointStruct enterIcon[6] = {{0, 7}, {4, 3}, {4, 11}, {0, 7}, {15, 7}, {15, 0}};

    int size = 6;

    for (int i = 0; i < size - 1; i++) {
        int x0 = x + (enterIcon[i].x * scale);
        int y0 = y + (enterIcon[i].y * scale);
        int x1 = x + (enterIcon[i + 1].x * scale);
        int y1 = y + (enterIcon[i + 1].y * scale);

        display->drawLine(x0, y0, x1, y1);
    }
}

#endif

// Indicate to screen class that module is handling keyboard input specially (at certain times)
// This prevents the left & right keys being used for nav. between screen frames during text entry.
bool CannedMessageModule::interceptingKeyboardInput()
{
    switch (runState) {
    case CANNED_MESSAGE_RUN_STATE_DISABLED:
    case CANNED_MESSAGE_RUN_STATE_INACTIVE:
        return false;
    default:
        return true;
    }
}

// Draw the node/channel selection screen
void CannedMessageModule::drawDestinationSelectionScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    requestFocus();
    display->setColor(WHITE); // Always draw cleanly
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    // Header (first row): pushed down by the header margin; centered, so no L/R inset needed
    int titleY = 2 + BASEUI_HEADER_MARGIN;
    String titleText = "Select Destination";
    titleText += searchQuery.length() > 0 ? " [" + searchQuery + "]" : " [ ]";
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(display->getWidth() / 2, titleY, titleText);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // List Items
    int rowYOffset = titleY + (FONT_HEIGHT_SMALL - 4);
    int numActiveChannels = this->activeChannelIndices.size();
    int totalEntries = numActiveChannels + this->filteredNodes.size();
    int columns = 1;
    this->visibleRows = (display->getHeight() - (titleY + FONT_HEIGHT_SMALL)) / (FONT_HEIGHT_SMALL - 4);
    if (this->visibleRows < 1)
        this->visibleRows = 1;

    // Clamp scrolling
    if (scrollIndex > totalEntries / columns)
        scrollIndex = totalEntries / columns;
    if (scrollIndex < 0)
        scrollIndex = 0;

    for (int row = 0; row < visibleRows; row++) {
        int itemIndex = scrollIndex + row;
        if (itemIndex >= totalEntries)
            break;

        int xOffset = 0;
        int yOffset = row * (FONT_HEIGHT_SMALL - 4) + rowYOffset;
        std::string entryText;

        // Draw Channels First
        if (itemIndex < numActiveChannels) {
            uint8_t channelIndex = this->activeChannelIndices[itemIndex];
            const char *channelName = channels.getName(channelIndex);
            entryText = std::string("#") + (channelName ? channelName : "?");
        }
        // Then Draw Nodes
        else {
            int nodeIndex = itemIndex - numActiveChannels;
            if (nodeIndex >= 0 && nodeIndex < static_cast<int>(this->filteredNodes.size())) {
                meshtastic_NodeInfoLite *node = this->filteredNodes[nodeIndex].node;
                if (node) {
                    if (display->getWidth() <= 64) {
                        entryText = node->short_name;
                    } else if (node->long_name[0]) {
                        entryText = node->long_name;
                    } else {
                        entryText = node->short_name;
                    }
                }

                int availWidth = display->getWidth() - 2 * BASEUI_BODY_LR_MARGIN -
                                 ((graphics::currentResolution == graphics::ScreenResolution::High) ? 40 : 20) -
                                 ((nodeInfoLiteIsFavorite(node)) ? 10 : 0);
                if (availWidth < 0)
                    availWidth = 0;
                char truncatedEntry[96];
                graphics::UIRenderer::truncateStringWithEmotes(display, entryText.c_str(), truncatedEntry, sizeof(truncatedEntry),
                                                               availWidth);
                entryText = truncatedEntry;

                // Prepend "* " if this is a favorite
                if (nodeInfoLiteIsFavorite(node)) {
                    entryText = "* " + entryText;
                }
                graphics::UIRenderer::truncateStringWithEmotes(display, entryText.c_str(), truncatedEntry, sizeof(truncatedEntry),
                                                               availWidth);
                entryText = truncatedEntry;
            }
        }

        if (entryText.empty() || entryText == "Unknown")
            entryText = "?";

        // Highlight background (if selected)
        if (itemIndex == destIndex) {
            int scrollPadding = 8; // Reserve space for scrollbar
            display->fillRect(BASEUI_BODY_LR_MARGIN, yOffset + 2, display->getWidth() - scrollPadding - 2 * BASEUI_BODY_LR_MARGIN,
                              FONT_HEIGHT_SMALL - 5);
            display->setColor(BLACK);
        }

        // Draw entry text
        graphics::UIRenderer::drawStringWithEmotes(display, xOffset + 2 + BASEUI_BODY_LR_MARGIN, yOffset, entryText.c_str(),
                                                   FONT_HEIGHT_SMALL, 1, false);
        display->setColor(WHITE);

        // Draw key icon (after highlight)
        /*
        if (itemIndex >= numActiveChannels) {
            int nodeIndex = itemIndex - numActiveChannels;
            if (nodeIndex >= 0 && nodeIndex < static_cast<int>(this->filteredNodes.size())) {
                const meshtastic_NodeInfoLite *node = this->filteredNodes[nodeIndex].node;
                if (node && hasKeyForNode(node)) {
                    int iconX = display->getWidth() - key_symbol_width - 15;
                    int iconY = yOffset + (FONT_HEIGHT_SMALL - key_symbol_height) / 2;

                    if (itemIndex == destIndex) {
                        display->setColor(INVERSE);
                    } else {
                        display->setColor(WHITE);
                    }
                    display->drawXbm(iconX, iconY, key_symbol_width, key_symbol_height, key_symbol);
                }
            }
        }
        */
    }

    // Scrollbar
    if (totalEntries > visibleRows) {
        int scrollbarHeight = visibleRows * (FONT_HEIGHT_SMALL - 4);
        int totalScrollable = totalEntries;
        int scrollTrackX = display->getWidth() - 6 - BASEUI_BODY_LR_MARGIN;
        display->drawRect(scrollTrackX, rowYOffset, 4, scrollbarHeight);
        int scrollHeight = (scrollbarHeight * visibleRows) / totalScrollable;
        int scrollPos = rowYOffset + (scrollbarHeight * scrollIndex) / totalScrollable;
        display->fillRect(scrollTrackX, scrollPos, 4, scrollHeight);
    }
}

// Smallest cell a fingertip gets aimed at, and the ceiling on how far the artwork may be blown up to
// reach it - past 4x a 16px emote is mostly staircase, and the grid holds too few to be worth
// scrolling. Only consulted on touchscreen builds.
#ifndef EMOTE_MIN_TOUCH_CELL_PX
#define EMOTE_MIN_TOUCH_CELL_PX 44
#endif
// Cells are sized from maxEmoteHeight(), which already multiplies by BASEUI_ICON_SCALE, but the
// artwork is drawn at emoteScale alone - so on a variant with an icon scale above 1 the glyph
// only fills that fraction of its cell. Variants can boost the drawn scale to take up the space
// without changing the grid. 1 leaves every existing board exactly as it was.
#ifndef EMOTE_PICKER_SCALE_BOOST
#define EMOTE_PICKER_SCALE_BOOST 1
#endif
#ifndef EMOTE_MAX_TOUCH_SCALE
#define EMOTE_MAX_TOUCH_SCALE 4
#endif

// graphics::drawScaledXbm() with a vertical clip. Rows scroll under the picker's header, so a
// partially visible emote has to stop at the grid's edge instead of painting over the title.
static void drawClippedScaledXbm(OLEDDisplay *display, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *xbm, int scale,
                                 int16_t clipTop, int16_t clipBottom)
{
    const int16_t bytesPerRow = (w + 7) / 8;
    for (int16_t row = 0; row < h; ++row) {
        const int16_t rowY = y + row * scale;
        const int16_t top = std::max<int16_t>(rowY, clipTop);
        const int16_t bottom = std::min<int16_t>(rowY + scale, clipBottom);
        if (bottom <= top)
            continue;
        const uint8_t *rowPtr = xbm + row * bytesPerRow;
        for (int16_t col = 0; col < w; ++col) {
            if (pgm_read_byte(rowPtr + (col >> 3)) & (1U << (col & 7))) // XBM is LSB-first
                display->fillRect(x + col * scale, top, scale, bottom - top);
        }
    }
}

void CannedMessageModule::drawEmotePickerScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    const std::vector<uint16_t> &unique = uniqueEmoteIndices();
    const int numUnique = (int)unique.size();
    if (numUnique == 0)
        return; // EXCLUDE_EMOJI: emotes[] is empty, there is nothing to pick from

    const int headerFontHeight = FONT_HEIGHT_SMALL;
    const int headerMargin = 2; // extra pixels below the header
    const int cellPadding = 2;

    // Artwork is drawn at the variant's icon scale, bumped further on a touchscreen until a cell is
    // a target a fingertip can actually hit. Encoder and button navigation needs no such minimum.
    const int emoteSize = graphics::EmoteRenderer::maxEmoteHeight();
    int emoteScale = BASEUI_ICON_SCALE;
#if HAS_TOUCHSCREEN
    while (emoteScale < EMOTE_MAX_TOUCH_SCALE && emoteSize * emoteScale + 2 * cellPadding < EMOTE_MIN_TOUCH_CELL_PX)
        emoteScale++;
#endif
    const int cellSize = emoteSize * emoteScale + cellPadding * 2;
    // Grid geometry stays on emoteScale; only the artwork is boosted.
    const int drawScale = emoteScale * EMOTE_PICKER_SCALE_BOOST;

    const int headerY = y + BASEUI_HEADER_MARGIN;
    const int gridX = x + BASEUI_BODY_LR_MARGIN;
    const int gridTop = headerY + headerFontHeight + headerMargin;
    const int availableHeight = display->getHeight() - gridTop - 2;
    const int availableWidth = display->getWidth() - 2 * BASEUI_BODY_LR_MARGIN;
    const int cols = std::max(1, availableWidth / cellSize);
    const int rows = std::max(1, availableHeight / cellSize);
    const int gridBottom = gridTop + rows * cellSize;

    // Hand the layout to handleEmotePickerInput() rather than have it derive its own
    emoteGridX = gridX;
    emoteGridTop = gridTop;
    emoteGridBottom = gridBottom;
    emoteGridCols = cols;
    emoteGridRows = rows;
    emoteCellSize = cellSize;

    if (emotePickerIndex < 0)
        emotePickerIndex = 0;
    if (emotePickerIndex >= numUnique)
        emotePickerIndex = numUnique - 1;

    const int totalRows = (numUnique + cols - 1) / cols;
    const float maxScrollOffset = std::max(0, totalRows - rows);

    // A finger drag owns the scroll position while it is in range; anything that leaves it out of
    // bounds (a resize, a selection moved by the keys) falls back to centring the selection.
    if (emoteScrollOffset < 0 || emoteScrollOffset > maxScrollOffset)
        emoteScrollOffset = emoteScrollToCenter(emotePickerIndex / cols, rows, totalRows);

    const int topRow = (int)emoteScrollOffset;
    const int pixelOffset = (int)((emoteScrollOffset - topRow) * cellSize);

    // +1 row beyond the viewport so a partially scrolled row is drawn rather than popping in
    for (int row = 0; row < rows + 1; ++row) {
        for (int col = 0; col < cols; ++col) {
            const int idx = (topRow + row) * cols + col;
            if (idx >= numUnique)
                break;

            const graphics::Emote &emote = graphics::emotes[unique[idx]];
            const int cellX = gridX + col * cellSize;
            const int cellY = gridTop + row * cellSize - pixelOffset;
            if (cellY >= gridBottom || cellY + cellSize <= gridTop)
                continue;

            if (idx == emotePickerIndex) {
                const int top = std::max(cellY, gridTop);
                const int bottom = std::min(cellY + cellSize, gridBottom);
                display->fillRect(cellX, top, cellSize, bottom - top);
                display->setColor(BLACK);
            }

            drawClippedScaledXbm(display, cellX + (cellSize - emote.width * drawScale) / 2,
                                 cellY + (cellSize - emote.height * drawScale) / 2, emote.width, emote.height, emote.bitmap,
                                 drawScale, gridTop, gridBottom);

            if (idx == emotePickerIndex)
                display->setColor(WHITE);
        }
    }

    // Header last, over its own background, so a row scrolling up passes behind the title
    char headerText[32];
    snprintf(headerText, sizeof(headerText), "Emotes (%d/%d)", emotePickerIndex + 1, numUnique);
    display->setColor(BLACK);
    display->fillRect(x, headerY, display->getWidth(), headerFontHeight);
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(x + display->getWidth() / 2, headerY, headerText);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    if (totalRows > rows) {
        const int scrollTrackX = display->getWidth() - 6 - BASEUI_BODY_LR_MARGIN;
        display->drawRect(scrollTrackX, gridTop, 4, availableHeight);
        const int scrollBarLen = std::max(6, (availableHeight * rows) / totalRows);
        const int scrollBarPos = gridTop + (int)((availableHeight * emoteScrollOffset) / totalRows);
        display->fillRect(scrollTrackX, scrollBarPos, 4, scrollBarLen);
    }
}

void CannedMessageModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    this->displayWidth = display->getWidth();   // Store display size for later use
    this->displayHeight = display->getHeight(); // (the touch hit tests below run without a display)
    char buffer[50];
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    // Never draw if state is outside our UI modes
    if (!(runState == CANNED_MESSAGE_RUN_STATE_ACTIVE || runState == CANNED_MESSAGE_RUN_STATE_FREETEXT ||
          runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION || runState == CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER)) {
        return; // bail if not in a UI state that should render
    }

    // Emote Picker Screen
    if (this->runState == CANNED_MESSAGE_RUN_STATE_EMOTE_PICKER) {
        drawEmotePickerScreen(display, state, x, y); // <-- Call your emote picker drawer here
        return;
    }

    // Destination Selection
    if (this->runState == CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION) {
        drawDestinationSelectionScreen(display, state, x, y);
        return;
    }

    // Disabled Screen
    if (this->runState == CANNED_MESSAGE_RUN_STATE_DISABLED) {
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->setFont(FONT_SMALL);
        display->drawString(10 + x + BASEUI_BODY_LR_MARGIN, y + FONT_HEIGHT_SMALL + BASEUI_HEADER_MARGIN,
                            "Canned Message\nModule disabled.");
        return;
    }

    // Free Text Input Screen
    if (this->runState == CANNED_MESSAGE_RUN_STATE_FREETEXT) {
        requestFocus();
#if defined(USE_EINK) && defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS) && !defined(MESHTASTIC_INCLUDE_INKHUD)
        static_cast<NicheGraphics::BaseUIEInkDisplay *>(display)->enableUnlimitedFastMode();
#elif defined(USE_EINK) && defined(USE_EINK_DYNAMICDISPLAY)
        EInkDynamicDisplay *einkDisplay = static_cast<EInkDynamicDisplay *>(display);
        einkDisplay->enableUnlimitedFastMode();
#endif
#if defined(USE_VIRTUAL_KEYBOARD)
        drawKeyboard(display, state, x, y);
#else
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->setFont(FONT_SMALL);

        // Draw node/channel header at the top
        drawHeader(display, x, y, buffer);

        // Char count right-aligned
        if (runState != CANNED_MESSAGE_RUN_STATE_DESTINATION_SELECTION) {
            uint16_t charsLeft =
                meshtastic_Constants_DATA_PAYLOAD_LEN - this->freetext.length() - (moduleConfig.canned_message.send_bell ? 1 : 0);
            snprintf(buffer, sizeof(buffer), "%d left", charsLeft);
            display->drawString(x + display->getWidth() - display->getStringWidth(buffer) - BASEUI_HEADER_LR_MARGIN,
                                y + BASEUI_HEADER_MARGIN, buffer);
        }

#if INPUTBROKER_SERIAL_TYPE == 1
        // Chatter Modifier key mode label (right side)
        {
            uint8_t mode = globalSerialKeyboard ? globalSerialKeyboard->getShift() : 0;
            const char *label = (mode == 0) ? "a" : (mode == 1) ? "A" : "#";

            display->setFont(FONT_SMALL);
            display->setTextAlignment(TEXT_ALIGN_LEFT);

            const int16_t th = FONT_HEIGHT_SMALL;
            const int16_t tw = display->getStringWidth(label);
            const int16_t padX = 3;
            const int16_t padY = 2;
            const int16_t r = 3;

            const int16_t bw = tw + padX * 2;
            const int16_t bh = th + padY * 2;

            const int16_t bx = x + display->getWidth() - bw - 2;
            const int16_t by = y + display->getHeight() - bh - 2;

            display->setColor(WHITE);
            display->fillRect(bx + r, by, bw - r * 2, bh);
            display->fillRect(bx, by + r, r, bh - r * 2);
            display->fillRect(bx + bw - r, by + r, r, bh - r * 2);
            display->fillCircle(bx + r, by + r, r);
            display->fillCircle(bx + bw - r - 1, by + r, r);
            display->fillCircle(bx + r, by + bh - r - 1, r);
            display->fillCircle(bx + bw - r - 1, by + bh - r - 1, r);

            display->setColor(BLACK);
            display->drawString(bx + padX, by + padY, label);
        }

        // LEFT-SIDE DESTINATION-HINT BOX (“Dest: Shift + ◄”)
        {
            display->setFont(FONT_SMALL);
            display->setTextAlignment(TEXT_ALIGN_LEFT);

            const char *label = "Dest: Shift + ";
            int16_t labelW = display->getStringWidth(label);

            // triangle size visually matches glyph height, not full line height
            const int triH = FONT_HEIGHT_SMALL - 3;
            const int triW = triH * 0.7;

            const int16_t padX = 3;
            const int16_t padY = 2;
            const int16_t r = 3;

            const int16_t bw = labelW + triW + padX * 2 + 2;
            const int16_t bh = FONT_HEIGHT_SMALL + padY * 2;

            const int16_t bx = x + 2;
            const int16_t by = y + display->getHeight() - bh - 2;

            // Rounded white box
            display->setColor(WHITE);
            display->fillRect(bx + r, by, bw - (r * 2), bh);
            display->fillRect(bx, by + r, r, bh - (r * 2));
            display->fillRect(bx + bw - r, by + r, r, bh - (r * 2));
            display->fillCircle(bx + r, by + r, r);
            display->fillCircle(bx + bw - r - 1, by + r, r);
            display->fillCircle(bx + r, by + bh - r - 1, r);
            display->fillCircle(bx + bw - r - 1, by + bh - r - 1, r);

            // Draw text
            display->setColor(BLACK);
            display->drawString(bx + padX, by + padY, label);

            // Perfectly center triangle on text baseline
            int16_t tx = bx + padX + labelW;
            int16_t ty = by + padY + (FONT_HEIGHT_SMALL / 2) - (triH / 2) - 1; // -1 for optical centering

            // ◄ Left-pointing triangle
            display->fillTriangle(tx + triW, ty,       // top-right
                                  tx, ty + triH / 2,   // left center
                                  tx + triW, ty + triH // bottom-right
            );
        }
#endif
        // Draw Free Text input with multi-emote support and proper line wrapping
        display->setColor(WHITE);
        {
            int inputY = y + FONT_HEIGHT_SMALL + BASEUI_HEADER_MARGIN;
            int inputX = x + BASEUI_BODY_LR_MARGIN;
            String msgWithCursor = this->drawWithCursor(this->freetext, this->cursor);
            drawWrappedEmoteText(display, inputX, inputY, msgWithCursor.c_str(),
                                 display->getWidth() - inputX - BASEUI_BODY_LR_MARGIN, FONT_HEIGHT_SMALL);
        }

        // Emote button, bottom right. Matched to the hit box handleFreeTextInput() tests, and drawn
        // with the keyboard's rounded cap so it reads as a key rather than a stray box.
#if CANNED_MESSAGE_HAS_EMOTE_BUTTON
        {
            const int buttonX = x + display->getWidth() - EMOTE_BUTTON_SIZE - EMOTE_BUTTON_MARGIN;
            const int buttonY = y + display->getHeight() - EMOTE_BUTTON_SIZE - EMOTE_BUTTON_MARGIN;
            const graphics::Emote *smiley = graphics::EmoteRenderer::findEmoteByLabel("\U0001F60A");

            display->setColor(WHITE);
            drawRoundedRect(display, buttonX, buttonY, EMOTE_BUTTON_SIZE, EMOTE_BUTTON_SIZE, EMOTE_BUTTON_RADIUS);
            if (smiley) {
                graphics::drawScaledXbm(display, buttonX + (EMOTE_BUTTON_SIZE - smiley->width * BASEUI_ICON_SCALE) / 2,
                                        buttonY + (EMOTE_BUTTON_SIZE - smiley->height * BASEUI_ICON_SCALE) / 2, smiley->width,
                                        smiley->height, smiley->bitmap);
            }
        }
#endif
#endif
        return;
    }

    // Canned Messages List
    if (this->messagesCount > 0) {
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->setFont(FONT_SMALL);

        // Precompute per-row heights based on emotes (centered if present)
        const int baseRowSpacing = FONT_HEIGHT_SMALL - 4;

        int topMsg;
        int _visibleRows;

        // Draw header (To: ...)
        drawHeader(display, x, y, buffer);

        // Shift message list upward by 3 pixels to reduce spacing between header and first message
        // Push the list below the header margin so the body starts clear of the reserved top area
        const int listYOffset = y + FONT_HEIGHT_SMALL - 3 + BASEUI_HEADER_MARGIN;
        _visibleRows = (display->getHeight() - listYOffset) / baseRowSpacing;

        // Figure out which messages are visible and their needed heights
        topMsg = (messagesCount > _visibleRows && currentMessageIndex >= _visibleRows - 1)
                     ? currentMessageIndex - _visibleRows + 2
                     : 0;
        int countRows = std::min(messagesCount, _visibleRows);

        // Draw all message rows with multi-emote support
        int yCursor = listYOffset;
        for (int vis = 0; vis < countRows; vis++) {
            int msgIdx = topMsg + vis;
            int lineY = yCursor;
            const char *msg = getMessageByIndex(msgIdx);
            int rowHeight = getRowHeightForEmoteText(msg, baseRowSpacing);
            bool _highlight = (msgIdx == currentMessageIndex);

            // Vertically center based on rowHeight
            int textYOffset = (rowHeight - FONT_HEIGHT_SMALL) / 2;

#ifdef USE_EINK
            int nextX = x + BASEUI_BODY_LR_MARGIN + (_highlight ? 12 : 0);
            if (_highlight)
                display->drawString(x + BASEUI_BODY_LR_MARGIN, lineY + textYOffset, ">");
#else
            int scrollPadding = 8;
            if (_highlight) {
                display->fillRect(x + BASEUI_BODY_LR_MARGIN, lineY,
                                  display->getWidth() - scrollPadding - 2 * BASEUI_BODY_LR_MARGIN, rowHeight);
                display->setColor(BLACK);
            }
            int nextX = x + BASEUI_BODY_LR_MARGIN + (_highlight ? 2 : 0);
#endif

            if (msg && *msg)
                drawCenteredEmoteText(display, nextX, lineY, rowHeight, msg);
#ifndef USE_EINK
            if (_highlight)
                display->setColor(WHITE);
#endif

            yCursor += rowHeight;
        }

        // Scrollbar
        if (messagesCount > _visibleRows) {
            int scrollHeight = display->getHeight() - listYOffset;
            int scrollTrackX = display->getWidth() - 6 - BASEUI_BODY_LR_MARGIN;
            display->drawRect(scrollTrackX, listYOffset, 4, scrollHeight);
            int barHeight = (scrollHeight * _visibleRows) / messagesCount;
            int scrollPos = listYOffset + (scrollHeight * topMsg) / messagesCount;
            display->fillRect(scrollTrackX, scrollPos, 4, barHeight);
        }
    }
}

// Return SNR limit based on modem preset
static float getSnrLimit(meshtastic_Config_LoRaConfig_ModemPreset preset)
{
    switch (preset) {
    case PRESET(LONG_SLOW):
    case PRESET(LONG_MODERATE):
    case PRESET(LONG_FAST):
        return -6.0f;
    case PRESET(MEDIUM_SLOW):
    case PRESET(MEDIUM_FAST):
    case PRESET(MEDIUM_TURBO):
        return -5.5f;
    case PRESET(SHORT_SLOW):
    case PRESET(SHORT_FAST):
    case PRESET(SHORT_TURBO):
        return -4.5f;
    default:
        return -6.0f;
    }
}

// Return Good/Fair/Bad label and set 1-5 bars based on SNR and RSSI
static const char *getSignalGrade(float snr, int32_t rssi, float snrLimit, int &bars)
{
    // 5-bar logic: strength inside Good/Fair/Bad category
    if (snr > snrLimit && rssi > -10) {
        bars = 5; // very strong good
        return "Good";
    } else if (snr > snrLimit && rssi > -20) {
        bars = 4; // normal good
        return "Good";
    } else if (snr > 0 && rssi > -50) {
        bars = 3; // weaker good (on edge of fair)
        return "Good";
    } else if (snr > -10 && rssi > -100) {
        bars = 2; // fair
        return "Fair";
    } else {
        bars = 1; // bad
        return "Bad";
    }
}

ProcessMessage CannedMessageModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Only process routing ACK/NACK packets that are responses to our own outbound
    if (mp.decoded.portnum == meshtastic_PortNum_ROUTING_APP && waitingForAck && mp.to == nodeDB->getNodeNum() &&
        mp.decoded.request_id == this->lastRequestId) // only ACKs for our last sent packet
    {
        if (mp.decoded.request_id != 0) {
            // Decode the routing response
            meshtastic_Routing decoded = meshtastic_Routing_init_default;
            pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, meshtastic_Routing_fields, &decoded);

            // Determine ACK/NACK status
            bool isAck = (decoded.error_reason == meshtastic_Routing_Error_NONE);
            bool isFromDest = (mp.from == this->lastSentNode);
            bool wasBroadcast = (this->lastSentNode == NODENUM_BROADCAST);

            // Identify the responding node
            if (wasBroadcast && mp.from != nodeDB->getNodeNum()) {
                this->incoming = mp.from; // relayed by another node
            } else {
                this->incoming = this->lastSentNode; // direct reply
            }

            // Final ACK/NACK logic
            if (wasBroadcast) {
                // Any ACK counts for broadcast
                this->ack = isAck;
                waitingForAck = false;
            } else if (isFromDest) {
                // Only ACK from destination counts as final
                this->ack = isAck;
                waitingForAck = false;
            } else if (isAck) {
                // Relay ACK → mark as RELAYED, still no final ACK
                this->ack = false;
                waitingForAck = false;
            } else {
                // Explicit failure
                this->ack = false;
                waitingForAck = false;
            }

            // Update last sent StoredMessage with ACK/NACK/RELAYED result
            if (!messageStore.getMessages().empty()) {
                StoredMessage &last = const_cast<StoredMessage &>(messageStore.getMessages().back());
                if (last.sender == nodeDB->getNodeNum()) { // only update our own messages
                    if (wasBroadcast && isAck) {
                        last.ackStatus = AckStatus::ACKED;
                    } else if (isFromDest && isAck) {
                        last.ackStatus = AckStatus::ACKED;
                    } else if (!isFromDest && isAck) {
                        last.ackStatus = AckStatus::RELAYED;
                    } else {
                        last.ackStatus = AckStatus::NACKED;
                    }
                }
            }

            // Capture radio metrics
            this->lastRxRssi = mp.rx_rssi;
            this->lastRxSnr = mp.rx_snr;

            // Show overlay banner
            if (screen) {
                auto *display = screen->getDisplayDevice();
                graphics::BannerOverlayOptions opts;
                static char buf[128];

                const char *channelName = channels.getName(this->channel);
                const char *src = getNodeName(this->incoming);
                char nodeName[48];
                strncpy(nodeName, src, sizeof(nodeName) - 1);
                nodeName[sizeof(nodeName) - 1] = '\0';

                int availWidth =
                    display->getWidth() - ((graphics::currentResolution == graphics::ScreenResolution::High) ? 60 : 30);
                if (availWidth < 0)
                    availWidth = 0;

                size_t origLen = strlen(nodeName);
                while (nodeName[0] && display->getStringWidth(nodeName) > availWidth) {
                    nodeName[strlen(nodeName) - 1] = '\0';
                }
                if (strlen(nodeName) < origLen) {
                    strcat(nodeName, "...");
                }

                // Calculate signal quality and bars based on preset, SNR, and RSSI
                float snrLimit = getSnrLimit(config.lora.modem_preset);
                int bars = 0;
                const char *qualityLabel = getSignalGrade(this->lastRxSnr, this->lastRxRssi, snrLimit, bars);

                if (this->ack) {
                    if (this->lastSentNode == NODENUM_BROADCAST) {
                        snprintf(buf, sizeof(buf), "Message sent to\n#%s\n\nSignal: %s",
                                 (channelName && channelName[0]) ? channelName : "unknown", qualityLabel);
                    } else {
                        snprintf(buf, sizeof(buf), "DM sent to\n@%s\n\nSignal: %s", nodeName[0] ? nodeName : "unknown",
                                 qualityLabel);
                    }
                } else if (isAck && !isFromDest) {
                    // Relay ACK banner
                    snprintf(buf, sizeof(buf), "DM Relayed\n(Status Unknown)\n%s\n\nSignal: %s",
                             nodeName[0] ? nodeName : "unknown", qualityLabel);
                } else {
                    if (this->lastSentNode == NODENUM_BROADCAST) {
                        snprintf(buf, sizeof(buf), "Message failed to\n#%s",
                                 (channelName && channelName[0]) ? channelName : "unknown");
                    } else {
                        snprintf(buf, sizeof(buf), "DM failed to\n@%s", nodeName[0] ? nodeName : "unknown");
                    }
                }

                opts.message = buf;
                opts.durationMs = 3000;
                graphics::bannerSignalBars = bars; // tell banner renderer how many bars to draw
                screen->showOverlayBanner(opts);   // this triggers drawNotificationBox()
            }
        }
    }

    return ProcessMessage::CONTINUE;
}

void CannedMessageModule::loadProtoForModule()
{
    if (nodeDB->loadProto(cannedMessagesConfigFile, meshtastic_CannedMessageModuleConfig_size,
                          sizeof(meshtastic_CannedMessageModuleConfig), &meshtastic_CannedMessageModuleConfig_msg,
                          &cannedMessageModuleConfig) != LoadFileResult::LOAD_SUCCESS) {
        installDefaultCannedMessageModuleConfig();
    }
}
/**
 * @brief Save the module config to file.
 *
 * @return true On success.
 * @return false On error.
 */
bool CannedMessageModule::saveProtoForModule()
{
    bool okay = true;

#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif

    okay &= nodeDB->saveProto(cannedMessagesConfigFile, meshtastic_CannedMessageModuleConfig_size,
                              &meshtastic_CannedMessageModuleConfig_msg, &cannedMessageModuleConfig);

    return okay;
}

/**
 * @brief Fill configuration with default values.
 */
void CannedMessageModule::installDefaultCannedMessageModuleConfig()
{
    strncpy(cannedMessageModuleConfig.messages, "Hi|Bye|Yes|No|Ok", sizeof(cannedMessageModuleConfig.messages));
}

/**
 * @brief An admin message arrived to AdminModule. We are asked whether we want to handle that.
 *
 * @param mp The mesh packet arrived.
 * @param request The AdminMessage request extracted from the packet.
 * @param response The prepared response
 * @return AdminMessageHandleResult HANDLED if message was handled
 *   HANDLED_WITH_RESULT if a result is also prepared.
 */
AdminMessageHandleResult CannedMessageModule::handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                          meshtastic_AdminMessage *request,
                                                                          meshtastic_AdminMessage *response)
{
    AdminMessageHandleResult result;

    switch (request->which_payload_variant) {
    case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
        LOG_DEBUG("Client getting radio canned messages");
        this->handleGetCannedMessageModuleMessages(mp, response);
        result = AdminMessageHandleResult::HANDLED_WITH_RESPONSE;
        break;

    case meshtastic_AdminMessage_set_canned_message_module_messages_tag:
        LOG_DEBUG("Client getting radio canned messages");
        this->handleSetCannedMessageModuleMessages(request->set_canned_message_module_messages);
        result = AdminMessageHandleResult::HANDLED;
        break;

    default:
        result = AdminMessageHandleResult::NOT_HANDLED;
    }

    return result;
}

void CannedMessageModule::handleGetCannedMessageModuleMessages(const meshtastic_MeshPacket &req,
                                                               meshtastic_AdminMessage *response)
{
    LOG_DEBUG("*** handleGetCannedMessageModuleMessages");
    if (req.decoded.want_response) {
        response->which_payload_variant = meshtastic_AdminMessage_get_canned_message_module_messages_response_tag;
        strncpy(response->get_canned_message_module_messages_response, cannedMessageModuleConfig.messages,
                sizeof(response->get_canned_message_module_messages_response));
    } // Don't send anything if not instructed to. Better than asserting.
}

void CannedMessageModule::handleSetCannedMessageModuleMessages(const char *from_msg)
{
    int changed = 0;

    if (*from_msg) {
        changed |= strcmp(cannedMessageModuleConfig.messages, from_msg);
        strncpy(cannedMessageModuleConfig.messages, from_msg, sizeof(cannedMessageModuleConfig.messages));
        LOG_DEBUG("*** from_msg.text:%s", from_msg);
    }

    if (changed) {
        this->saveProtoForModule();
        if (splitConfiguredMessages()) {
            moduleConfig.canned_message.enabled = true;
        }
    }
}

String CannedMessageModule::drawWithCursor(String text, int cursor)
{
    String result = text.substring(0, cursor) + "_" + text.substring(cursor);
    return result;
}

#endif
