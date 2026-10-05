#include "configuration.h"

#if HAS_SCREEN
#include "DisplayFormatters.h"
#include "NodeDB.h"
#include "NotificationRenderer.h"
#include "UIRenderer.h"
#include "UptimeClock.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TFTColorRegions.h"
#if BASEUI_NATIVE_RGB565
#include "graphics/TFTDisplay.h"
#endif
#include "graphics/TFTPalette.h"
#include "graphics/images.h"
#include "input/RotaryEncoderInterruptImpl1.h"
#include "input/UpDownInterruptImpl1.h"
#include "mesh/Throttle.h"
#if HAS_BUTTON
#include "input/ButtonThread.h"
#endif
#include "main.h"
#if defined(USE_VIRTUAL_KEYBOARD)
#include "modules/CannedMessageModule.h"
#endif
#include <algorithm>
#include <string>
#include <vector>
#if HAS_TRACKBALL
#include "input/TrackballInterruptImpl1.h"
#endif

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#endif

using namespace meshtastic;

#if HAS_BUTTON
// Global button thread pointer defined in main.cpp
extern ::ButtonThread *UserButtonThread;
#endif

// External references to global variables from Screen.cpp
extern std::vector<std::string> functionSymbol;
extern std::string functionSymbolString;
extern bool hasUnreadMessage;

namespace graphics
{
int bannerSignalBars = -1;
InputEvent NotificationRenderer::inEvent;
int16_t NotificationRenderer::curSelected = 0;
char NotificationRenderer::alertBannerMessage[256] = {0};
uint32_t NotificationRenderer::alertBannerUntil = 0;   // 0 is a special case meaning forever
uint16_t NotificationRenderer::alertBannerOptions = 0; // last x lines are selectable options
const char **NotificationRenderer::optionsArrayPtr = nullptr;
const int *NotificationRenderer::optionsEnumPtr = nullptr;
std::function<void(int)> NotificationRenderer::alertBannerCallback = NULL;
bool NotificationRenderer::pauseBanner = false;
notificationTypeEnum NotificationRenderer::current_notification_type = notificationTypeEnum::none;
uint32_t NotificationRenderer::numDigits = 0;
uint32_t NotificationRenderer::currentNumber = 0;
char NotificationRenderer::alphanumericValue[16] = {0};
VirtualKeyboard *NotificationRenderer::virtualKeyboard = nullptr;
std::function<void(const std::string &)> NotificationRenderer::textInputCallback = nullptr;
std::function<void()> NotificationRenderer::textPromptCancelCallback = nullptr;
std::function<void(const std::string &)> NotificationRenderer::textPromptChangedCallback = nullptr;
std::function<void(int)> NotificationRenderer::textPromptPickCallback = nullptr;

char graphics::NotificationRenderer::alertBannerLines[MAX_LINES + 1][64] = {};
uint8_t graphics::NotificationRenderer::alertBannerLineCount = 0;
graphics::NotificationRenderer::BannerFont graphics::NotificationRenderer::alertBannerLineFonts[MAX_LINES + 1] = {};

static inline graphics::NotificationRenderer::BannerFont parseFontTagPrefix(const char *&p)
{
    // Tags must be at the start of the line:
    // [S] small, [M] medium, [L] large
    if (p && p[0] == '[' && p[1] != '\0' && p[2] == ']') {
        char t = p[1];
        if (t == 'S') {
            p += 3;
            return graphics::NotificationRenderer::BANNER_FONT_SMALL;
        }
        if (t == 'M') {
            p += 3;
            return graphics::NotificationRenderer::BANNER_FONT_MEDIUM;
        }
        if (t == 'L') {
            p += 3;
            return graphics::NotificationRenderer::BANNER_FONT_LARGE;
        }
    }
    return graphics::NotificationRenderer::BANNER_FONT_DEFAULT;
}

static inline const uint8_t *fontForBannerLine(graphics::NotificationRenderer::BannerFont f)
{
    switch (f) {
    case graphics::NotificationRenderer::BANNER_FONT_SMALL:
        return FONT_SMALL;
    case graphics::NotificationRenderer::BANNER_FONT_MEDIUM:
        return FONT_MEDIUM;
    case graphics::NotificationRenderer::BANNER_FONT_LARGE:
        return FONT_LARGE;
    case graphics::NotificationRenderer::BANNER_FONT_DEFAULT:
    default:
        return FONT_SMALL;
    }
}

static inline uint8_t effectiveLineHeightForBannerLine(graphics::NotificationRenderer::BannerFont f)
{
    uint8_t height = FONT_HEIGHT_SMALL;
    switch (f) {
    case graphics::NotificationRenderer::BANNER_FONT_MEDIUM:
        height = FONT_HEIGHT_MEDIUM;
        break;
    case graphics::NotificationRenderer::BANNER_FONT_LARGE:
        height = FONT_HEIGHT_LARGE;
        break;
    case graphics::NotificationRenderer::BANNER_FONT_SMALL:
    case graphics::NotificationRenderer::BANNER_FONT_DEFAULT:
    default:
        height = FONT_HEIGHT_SMALL;
        break;
    }
    return (height > 3) ? (height - 3) : height;
}

const char *graphics::NotificationRenderer::resolveBannerLine(uint16_t lineIndex, const char *rawLine, BannerFont &lineFont)
{
    lineFont = BANNER_FONT_DEFAULT;
    bool tagAware = (current_notification_type == notificationTypeEnum::text_banner ||
                     current_notification_type == notificationTypeEnum::pairing_pin) &&
                    alertBannerOptions == 0;
    if (!tagAware)
        return rawLine;
    if (lineIndex < alertBannerLineCount) {
        lineFont = alertBannerLineFonts[lineIndex];
        return alertBannerLines[lineIndex];
    }
    // The parsed-line cache doesn't cover this line (the banner text was stored without a
    // re-parse, or a draw raced the parse from another task): strip the tag here too, so it
    // acts as a font change and never renders as literal text - the BLE pair PIN banner
    // prefixes its PIN line with [M].
    lineFont = parseFontTagPrefix(rawLine);
    return rawLine;
}

void graphics::NotificationRenderer::parseBannerMessageWithFonts(const char *message)
{
    alertBannerLineCount = 0;
    for (uint8_t i = 0; i < (MAX_LINES + 1); i++) {
        alertBannerLines[i][0] = '\0';
        alertBannerLineFonts[i] = BANNER_FONT_DEFAULT;
    }

    if (!message || !message[0]) {
        return;
    }

    const char *p = message;

    while (*p && alertBannerLineCount < (MAX_LINES + 1)) {
        const char *lineStart = p;
        while (*p && *p != '\n') {
            p++;
        }

        char tmp[64] = {0};
        size_t len = (size_t)(p - lineStart);
        if (len > (sizeof(tmp) - 1)) {
            len = sizeof(tmp) - 1;
        }
        memcpy(tmp, lineStart, len);
        tmp[len] = '\0';

        // Tag at start
        const char *tp = tmp;
        BannerFont f = parseFontTagPrefix(tp);
        alertBannerLineFonts[alertBannerLineCount] = f;

        // Store stripped text
        strncpy(alertBannerLines[alertBannerLineCount], tp, sizeof(alertBannerLines[0]) - 1);
        alertBannerLines[alertBannerLineCount][sizeof(alertBannerLines[0]) - 1] = '\0';
        alertBannerLineCount++;

        if (*p == '\n') {
            p++;
        }
    }
}

// Used on boot when a certificate is being created
void NotificationRenderer::drawSSLScreen(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(FONT_SMALL);
    display->drawString(64 + x, y, "Creating SSL certificate");

#ifdef ARCH_ESP32
    yield();
    esp_task_wdt_reset();
#endif

    display->setFont(FONT_SMALL);
    if ((millis() / 1000) % 2) {
        display->drawString(64 + x, FONT_HEIGHT_SMALL + y + 2, "Please wait . . .");
    } else {
        display->drawString(64 + x, FONT_HEIGHT_SMALL + y + 2, "Please wait . .  ");
    }
}

void NotificationRenderer::resetBanner()
{
    notificationTypeEnum previousType = current_notification_type;

    alertBannerMessage[0] = '\0';
    current_notification_type = notificationTypeEnum::none;

    inEvent.inputEvent = INPUT_BROKER_NONE;
    inEvent.kbchar = 0;
    curSelected = 0;
    alertBannerOptions = 0; // last x lines are selectable options
    optionsArrayPtr = nullptr;
    optionsEnumPtr = nullptr;
    alertBannerCallback = NULL;
    pauseBanner = false;
    numDigits = 0;
    currentNumber = 0;

    nodeDB->pause_sort(false);

    // If we're exiting from text_input (virtual keyboard), stop module and trigger frame update
    // to ensure any messages received during keyboard use are now displayed
    if (previousType == notificationTypeEnum::text_input && screen) {
        OnScreenKeyboardModule::instance().stop(false);
        screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
    }
}

// Split alertBannerMessage at '\n' into at most MAX_LINES line starts; returns the line count.
static uint16_t splitBannerMessageLines(const char *lineStarts[MAX_LINES + 1])
{
    char *message = NotificationRenderer::alertBannerMessage;
    char *alertEnd = message + strnlen(message, sizeof(NotificationRenderer::alertBannerMessage));
    uint16_t lineCount = 0;
    lineStarts[0] = message;
    while ((lineCount < MAX_LINES) && (lineStarts[lineCount] < alertEnd)) {
        lineStarts[lineCount + 1] = std::find((char *)lineStarts[lineCount], alertEnd, '\n');
        if (lineStarts[lineCount + 1][0] == '\n')
            lineStarts[lineCount + 1] += 1;
        lineCount++;
    }
    return lineCount;
}

void NotificationRenderer::drawBannercallback(OLEDDisplay *display, OLEDDisplayUiState *state)
{
    // Handle text_input notifications first - they have their own timeout/banner logic
    if (current_notification_type == notificationTypeEnum::text_input) {
        // Check for timeout and reset if needed for text input
        if (alertBannerUntil > 0 && Throttle::deadlinePassed(alertBannerUntil)) {
            resetBanner();
            return;
        }
        drawTextInput(display, state);
        return;
    }

    // 0 means "no deadline set", and reads as long expired - test it first.
    if (alertBannerUntil > 0 && Throttle::deadlinePassed(alertBannerUntil)) {
        resetBanner();
    }

    // Exit if no banner is showing or banner is paused
    if (!isOverlayBannerShowing() || pauseBanner) {
        return;
    }
#if BASEUI_MENU_BACKDROP
    // Brackets every return below: the bands this menu draws into are the ones its next redraw restores.
    struct BackdropCover {
        OLEDDisplay *display;
        bool active;
        ~BackdropCover()
        {
            if (active)
                graphics::menuBackdropEndBanner(display);
        }
    } backdropCover{display, graphics::menuBackdropBeginBanner(display, state)};
#endif

    // Compact panels: DOWN cancels menus instead of scrolling (covers every picker below).
    if (graphics::isCompactPanel(display) && inEvent.inputEvent == INPUT_BROKER_DOWN) {
        inEvent.inputEvent = INPUT_BROKER_CANCEL;
    }

    switch (current_notification_type) {
    case notificationTypeEnum::none:
        // Do nothing - no notification to display
        break;
    case notificationTypeEnum::text_input:
        // Already handled above with dedicated logic (early return). Keep a case here to satisfy -Wswitch.
        break;
    case notificationTypeEnum::text_banner:
    case notificationTypeEnum::selection_picker:
    case notificationTypeEnum::pairing_pin:
        // pairing_pin is rendered the same as text_banner - it's just a
        // text banner. The split type exists only so the lockdown UI
        // short-circuit in Screen.cpp can recognise the BLE pair-PIN
        // banner as the one safe banner to composite over the LOCKED
        // frame.
        drawAlertBannerOverlay(display, state);
        break;
    case notificationTypeEnum::node_picker:
        drawNodePicker(display, state);
        break;
    case notificationTypeEnum::number_picker:
    case notificationTypeEnum::hex_picker:
    case notificationTypeEnum::alphanumeric_picker:
        drawCharPicker(display, state);
        break;
    case notificationTypeEnum::text_prompt:
        drawTextPrompt(display, state);
        break;
    }
}

namespace
{
// text_prompt's field. ASCII only: every input source that feeds it types ASCII, and it keeps cursor maths byte-wise.
char promptText[128]; // a waypoint description is up to 99
uint8_t promptLength = 0, promptCursor = 0, promptMax = 0;

// Suggestions the caller offers under the field (address search's live results). -1 selects the field itself.
constexpr int kMaxPromptSuggestions = 5;
char promptSuggestions[kMaxPromptSuggestions][48];
int promptSuggestionCount = 0, promptSuggestionSel = -1;
int16_t suggestLeft = 0, suggestWidth = 0, suggestTop = 0, suggestRowH = 0; // where the last draw put them, for taps

#if defined(USE_VIRTUAL_KEYBOARD)
// No keyboard on these boards, so a small touch keyboard sits under the popup. Four rows of characters, then actions.
const char *const kPromptRows[] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,.'"};
constexpr int kPromptRowCount = sizeof(kPromptRows) / sizeof(kPromptRows[0]);
enum PromptAction : char { PromptEsc = 1, PromptShift, PromptSpace, PromptDelete, PromptOk };
struct PromptKey {
    int16_t x, y, w, h;
    char key; // a character, or a PromptAction
};
PromptKey promptKeys[kPromptRowCount * 10 + 5];
int promptKeyCount = 0;
bool promptShift = false;

char promptKeyAt(int16_t x, int16_t y)
{
    for (int i = 0; i < promptKeyCount; i++) {
        const PromptKey &k = promptKeys[i];
        if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h)
            return k.key;
    }
    return 0;
}
#endif

void promptInsert(char c)
{
    if (promptLength >= promptMax)
        return;
    memmove(promptText + promptCursor + 1, promptText + promptCursor, promptLength - promptCursor);
    promptText[promptCursor++] = c;
    promptText[++promptLength] = '\0';
}

void promptDelete()
{
    if (promptCursor == 0)
        return;
    memmove(promptText + promptCursor - 1, promptText + promptCursor, promptLength - promptCursor);
    promptCursor--;
    promptText[--promptLength] = '\0';
}
} // namespace

void NotificationRenderer::startTextPrompt(const char *initialText, uint8_t maxLength)
{
    promptMax = std::min<uint8_t>(maxLength ? maxLength : sizeof(promptText) - 1, sizeof(promptText) - 1);
    promptLength = 0;
    for (const char *p = initialText; p && *p && promptLength < promptMax; p++) {
        if (*p >= 0x20 && *p <= 0x7E)
            promptText[promptLength++] = *p;
    }
    promptText[promptLength] = '\0';
    promptCursor = promptLength;
    promptSuggestionCount = 0;
    promptSuggestionSel = -1;
#if defined(USE_VIRTUAL_KEYBOARD)
    promptShift = false;
    promptKeyCount = 0;
#endif
}

// Keys go into the field the moment they arrive, not at the next draw: a frame behind the popup can be slow (a 3D map
// re-rendering), and the banner holds only one pending event, so keys typed during one would be lost or lag behind.
void NotificationRenderer::setTextPromptSuggestions(const char *const *labels, int count)
{
    promptSuggestionCount = count < kMaxPromptSuggestions ? count : kMaxPromptSuggestions;
    for (int i = 0; i < promptSuggestionCount; i++) {
        strncpy(promptSuggestions[i], labels[i], sizeof(promptSuggestions[i]) - 1);
        promptSuggestions[i][sizeof(promptSuggestions[i]) - 1] = '\0';
    }
    if (promptSuggestionSel >= promptSuggestionCount)
        promptSuggestionSel = promptSuggestionCount - 1;
}

bool NotificationRenderer::handleTextPromptInput(const InputEvent &event)
{
    if (current_notification_type != notificationTypeEnum::text_prompt)
        return false;
    bool submit = false, cancel = false;
    int picked = -1;
    const uint8_t lengthBefore = promptLength;
    const std::string textBefore(promptText, promptLength);
    const bool touch = event.touchX != 0 || event.touchY != 0;
    // A tap on a suggestion picks it, keyboard or not.
    if (touch && event.inputEvent == INPUT_BROKER_USER_PRESS && promptSuggestionCount > 0 && suggestRowH > 0 &&
        event.touchX >= suggestLeft && event.touchX < suggestLeft + suggestWidth && event.touchY >= suggestTop &&
        event.touchY < suggestTop + suggestRowH * promptSuggestionCount) {
        picked = (event.touchY - suggestTop) / suggestRowH;
    } else if (touch) {
#if defined(USE_VIRTUAL_KEYBOARD)
        if (event.inputEvent == INPUT_BROKER_USER_PRESS && cannedMessageModule) {
            // The composer's keyboard, as the message screen types on it.
            const String key = cannedMessageModule->keyForCoordinates(event.touchX, event.touchY);
            if (key == "ESC") {
                cancel = true;
            } else if (key == "\u21b5") { // enter
                if (promptSuggestionSel >= 0)
                    picked = promptSuggestionSel;
                else
                    submit = true;
            } else if (key == "\u21e7") { // shift
                cannedMessageModule->setKeyboardShift(!cannedMessageModule->keyboardShift());
            } else if (key == "\u232b") { // backspace
                cannedMessageModule->flashKeyboardKey(key);
                promptDelete();
            } else if (key == "SPACE" || key == " ") {
                cannedMessageModule->flashKeyboardKey(key);
                promptInsert(' ');
            } else if (key.length() == 1) {
                cannedMessageModule->flashKeyboardKey(key);
                promptInsert(cannedMessageModule->keyboardChar(key));
                cannedMessageModule->setKeyboardShift(false);
            }
        } else if (event.inputEvent == INPUT_BROKER_USER_PRESS) {
            const char key = promptKeyAt(event.touchX, event.touchY);
            if (key == PromptEsc) {
                cancel = true;
            } else if (key == PromptOk) {
                submit = true;
            } else if (key == PromptShift) {
                promptShift = !promptShift;
            } else if (key == PromptDelete) {
                promptDelete();
            } else if (key == PromptSpace) {
                promptInsert(' ');
            } else if (key) {
                promptInsert((promptShift && key >= 'a' && key <= 'z') ? (char)(key - 'a' + 'A') : key);
                promptShift = false;
            }
        }
#endif
    } else {
        switch (event.inputEvent) {
        case INPUT_BROKER_ANYKEY:
            if (event.kbchar == 0x08)
                promptDelete();
            else if (event.kbchar == '\r' || event.kbchar == '\n')
                submit = true;
            else if (event.kbchar == 0x1B)
                cancel = true;
            else if (event.kbchar >= 0x20 && event.kbchar <= 0x7E)
                promptInsert((char)event.kbchar);
            break;
        case INPUT_BROKER_BACK:
            promptDelete();
            break;
        case INPUT_BROKER_LEFT:
            if (promptCursor > 0)
                promptCursor--;
            break;
        case INPUT_BROKER_RIGHT:
            if (promptCursor < promptLength)
                promptCursor++;
            break;
        case INPUT_BROKER_DOWN:
            if (promptSuggestionSel + 1 < promptSuggestionCount)
                promptSuggestionSel++;
            break;
        case INPUT_BROKER_UP:
            if (promptSuggestionSel >= 0)
                promptSuggestionSel--;
            break;
        case INPUT_BROKER_SELECT:
            if (promptSuggestionSel >= 0)
                picked = promptSuggestionSel;
            else
                submit = true;
            break;
        case INPUT_BROKER_CANCEL:
        case INPUT_BROKER_ALT_LONG:
            cancel = true;
            break;
        default:
            break;
        }
        // Enter typed as a character submits the field, or the suggestion that is selected.
        if (submit && promptSuggestionSel >= 0 && event.inputEvent == INPUT_BROKER_ANYKEY) {
            submit = false;
            picked = promptSuggestionSel;
        }
    }
    if (event.inputEvent != INPUT_BROKER_NONE)
        alertBannerUntil = Time::timerEndsAtMillis(300000); // any key keeps it open

    if (picked >= 0 && !textPromptPickCallback)
        picked = -1; // suggestions without a taker: nothing to pick
    if (submit || cancel || picked >= 0) {
        auto callback = textInputCallback;
        auto onCancel = textPromptCancelCallback;
        auto onPick = textPromptPickCallback;
        const std::string text(promptText, promptLength);
        textInputCallback = nullptr;
        textPromptCancelCallback = nullptr;
        textPromptChangedCallback = nullptr;
        textPromptPickCallback = nullptr;
        resetBanner();
        if (picked >= 0)
            onPick(picked);
        else if (submit && callback)
            callback(text);
        else if (cancel && onCancel)
            onCancel();
        return true;
    }
    // Typing goes back to the field, and tells whoever offers suggestions what there is now.
    if (promptLength != lengthBefore || textBefore.compare(0, std::string::npos, promptText, promptLength) != 0) {
        promptSuggestionSel = -1;
        if (textPromptChangedCallback)
            textPromptChangedCallback(std::string(promptText, promptLength));
    }
    return true;
}

void NotificationRenderer::drawTextPrompt(OLEDDisplay *display, OLEDDisplayUiState *state)
{
    (void)state;
    inEvent.inputEvent = INPUT_BROKER_NONE; // keys were taken as they arrived, by handleTextPromptInput()
    inEvent.kbchar = 0;

    // Layout: title, then the field; a key hint under it, or the touch keyboard below the whole popup.
    const int16_t screenW = display->getWidth(), screenH = display->getHeight();
    display->setFont(FONT_SMALL);
    const int16_t lineH = FONT_HEIGHT_SMALL;
    constexpr int16_t pad = 4;
    const int16_t boxW = std::min<int16_t>(screenW - 12, 360);
    const int16_t boxLeft = (screenW - boxW) / 2;
    const int16_t fieldH = lineH + 4;
    const int16_t rowH = lineH + 2;
    const int16_t suggestionsH = promptSuggestionCount ? promptSuggestionCount * rowH + 3 : 0;
#if defined(USE_VIRTUAL_KEYBOARD)
    const int16_t boxH = pad + lineH + 3 + fieldH + suggestionsH + pad;
    const int16_t boxTop = pad;
#else
    const int16_t boxH = pad + lineH + 3 + fieldH + suggestionsH + 2 + lineH + pad;
    const int16_t boxTop = (screenH - boxH) / 2;
#endif
    drawBannerPanel(display, boxLeft, boxTop, boxW, boxH);

    display->setColor(WHITE);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(boxLeft + boxW / 2, boxTop + pad, alertBannerMessage);

    // The field scrolls sideways to keep the cursor in view.
    const int16_t fieldX = boxLeft + pad, fieldY = boxTop + pad + lineH + 3, fieldW = boxW - 2 * pad;
    const int16_t textRoom = fieldW - 6;
    display->drawRect(fieldX, fieldY, fieldW, fieldH);
    int first = 0;
    while (first < promptCursor && display->getStringWidth(promptText + first, promptCursor - first) > textRoom)
        first++;
    int last = first;
    while (last < promptLength && display->getStringWidth(promptText + first, last + 1 - first) <= textRoom)
        last++;
    char visible[sizeof(promptText)];
    memcpy(visible, promptText + first, last - first);
    visible[last - first] = '\0';
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(fieldX + 3, fieldY + 2, visible);
    const int16_t cursorX = fieldX + 3 + display->getStringWidth(promptText + first, promptCursor - first);
    if (promptSuggestionSel < 0) // the caret shows where keys go; with a suggestion selected they pick instead
        display->drawLine(cursorX, fieldY + 2, cursorX, fieldY + fieldH - 3);

    // Suggestions under the field, the selected one inverted. Each is cut to the width, on a character.
    suggestLeft = fieldX;
    suggestWidth = fieldW;
    suggestTop = fieldY + fieldH + 3;
    suggestRowH = rowH;
    for (int i = 0; i < promptSuggestionCount; i++) {
        char row[sizeof(promptSuggestions[0])];
        strncpy(row, promptSuggestions[i], sizeof(row) - 1);
        row[sizeof(row) - 1] = '\0';
        for (size_t n = strlen(row); n > 0 && display->getStringWidth(row) > fieldW - 6;) {
            char dropped;
            do {
                dropped = row[--n];
                row[n] = '\0';
            } while (n > 0 && (dropped & 0xC0) == 0x80);
        }
        const int16_t y = suggestTop + i * rowH;
        if (i == promptSuggestionSel) {
            display->setColor(WHITE);
            display->fillRect(fieldX, y, fieldW, rowH);
            display->setColor(BLACK);
        }
        display->drawString(fieldX + 3, y + 1, row);
        display->setColor(WHITE);
    }

#if defined(USE_VIRTUAL_KEYBOARD)
    // The message screen's own keyboard, so typing looks and works the same everywhere; this board has no other.
    if (cannedMessageModule) {
        display->setColor(BLACK);
        display->fillRect(0, boxTop + boxH + 2, screenW, screenH - (boxTop + boxH + 2));
        display->setColor(WHITE);
        cannedMessageModule->drawKeyboardKeys(display, 0, 0, false); // plain text: no emote key
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        return;
    }
    // Without the composer, a small keyboard of its own: five rows under the popup, up to a comfortable fingertip.
    const int16_t kbBottom = screenH - 2;
    const int16_t keyRowH = std::min<int16_t>(44, (kbBottom - (boxTop + boxH + pad)) / (kPromptRowCount + 1));
    const int16_t kbTop = kbBottom - keyRowH * (kPromptRowCount + 1);
    display->setColor(BLACK);
    display->fillRect(0, kbTop - 2, screenW, screenH - kbTop + 2);
    display->setColor(WHITE);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    promptKeyCount = 0;
    auto addKey = [&](int16_t x0, int16_t x1, int16_t y, char key, const char *label) {
        const PromptKey k{(int16_t)(x0 + 1), (int16_t)(y + 1), (int16_t)(x1 - x0 - 2), (int16_t)(keyRowH - 2), key};
        promptKeys[promptKeyCount++] = k;
        display->drawRect(k.x, k.y, k.w, k.h);
        display->drawString(k.x + k.w / 2, k.y + (k.h - lineH) / 2, label);
    };
    const int16_t kbLeft = 2, kbWidth = screenW - 4;
    for (int r = 0; r < kPromptRowCount; r++) {
        const int n = strlen(kPromptRows[r]);
        for (int i = 0; i < n; i++) {
            char c = kPromptRows[r][i];
            if (promptShift && c >= 'a' && c <= 'z')
                c = (char)(c - 'a' + 'A');
            const char label[2] = {c, '\0'};
            addKey(kbLeft + i * kbWidth / n, kbLeft + (i + 1) * kbWidth / n, kbTop + r * keyRowH, kPromptRows[r][i], label);
        }
    }
    // Esc | Shift | space | Del | OK, in tenths of the row
    static const struct {
        uint8_t from, to;
        char key;
        const char *label;
    } kActions[] = {{0, 15, PromptEsc, "Esc"},
                    {15, 30, PromptShift, "Aa"},
                    {30, 70, PromptSpace, "space"},
                    {70, 85, PromptDelete, "Del"},
                    {85, 100, PromptOk, "OK"}};
    const int16_t actionY = kbTop + kPromptRowCount * keyRowH;
    for (const auto &a : kActions)
        addKey(kbLeft + a.from * kbWidth / 100, kbLeft + a.to * kbWidth / 100, actionY, a.key, a.label);
#else
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(boxLeft + boxW / 2, fieldY + fieldH + suggestionsH + 2,
                        promptSuggestionCount ? "Down: suggestions   Enter: OK" : "Enter: OK   Esc: cancel");
#endif
    display->setTextAlignment(TEXT_ALIGN_LEFT);
}

// Number, hex and alphanumeric pickers share one flow: UP/DOWN cycle the character under the cursor
// through its charset, SELECT/RIGHT/LEFT move the cursor, a typed character is entered directly, and
// stepping past the last position confirms.
void NotificationRenderer::drawCharPicker(OLEDDisplay *display, OLEDDisplayUiState *state)
{
    static const char HEX_CHARS[] = "0123456789ABCDEF";
    static const char ALPHANUMERIC_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    const bool alphanumeric = current_notification_type == notificationTypeEnum::alphanumeric_picker;
    const char *charset = alphanumeric ? ALPHANUMERIC_CHARS : HEX_CHARS;
    const uint8_t base =
        alphanumeric ? sizeof(ALPHANUMERIC_CHARS) - 1 : (current_notification_type == notificationTypeEnum::hex_picker ? 16 : 10);
    const uint8_t positions = std::min<uint32_t>(numDigits, sizeof(alphanumericValue) - 1);
    auto findChar = [&](char c) { return static_cast<const char *>(memchr(charset, c, base)); };

    // Number and hex pickers keep their value packed in currentNumber; unpack it to one character per position.
    char value[sizeof(alphanumericValue)];
    if (alphanumeric) {
        memcpy(value, alphanumericValue, positions);
    } else {
        uint32_t packed = currentNumber;
        for (int i = positions - 1; i >= 0; i--) {
            value[i] = HEX_CHARS[packed % base];
            packed /= base;
        }
    }

    // Handle input
    const input_broker_event event = inEvent.inputEvent;
    if (curSelected < static_cast<int8_t>(positions)) {
        const char *current = findChar(value[curSelected]);
        const int index = current ? current - charset : 0;
        if (event == INPUT_BROKER_UP || event == INPUT_BROKER_ALT_PRESS || event == INPUT_BROKER_UP_LONG) {
            value[curSelected] = charset[(index + 1) % base];
        } else if (event == INPUT_BROKER_DOWN || event == INPUT_BROKER_USER_PRESS || event == INPUT_BROKER_DOWN_LONG) {
            value[curSelected] = charset[(index + base - 1) % base];
        } else if (event == INPUT_BROKER_ANYKEY) {
            char k = inEvent.kbchar;
            if (k >= 'a' && k <= 'z')
                k = static_cast<char>(k - 'a' + 'A');
            if (findChar(k)) { // direct keyboard entry
                value[curSelected] = k;
                curSelected++;
            }
        }
    }
    if (event == INPUT_BROKER_SELECT || event == INPUT_BROKER_RIGHT) {
        curSelected++;
    } else if (event == INPUT_BROKER_LEFT) {
        curSelected--;
    } else if ((event == INPUT_BROKER_CANCEL || event == INPUT_BROKER_ALT_LONG) && alertBannerUntil != 0) {
        resetBanner();
        return;
    }
    if (curSelected < 0)
        curSelected = 0;

    if (alphanumeric) {
        memcpy(alphanumericValue, value, positions);
    } else {
        uint32_t packed = 0;
        for (uint8_t i = 0; i < positions; i++)
            packed = packed * base + (findChar(value[i]) - charset);
        currentNumber = packed;
    }

    if (curSelected >= static_cast<int8_t>(positions)) {
        if (alphanumeric) {
            auto callback = textInputCallback; // capture before clearing to avoid re-entrancy surprises
            std::string result(alphanumericValue, positions);
            textInputCallback = nullptr;
            resetBanner();
            if (callback)
                callback(result);
        } else {
            if (alertBannerCallback)
                alertBannerCallback(currentNumber);
            resetBanner();
        }
        return;
    }

    inEvent.inputEvent = INPUT_BROKER_NONE;
    if (alertBannerMessage[0] == '\0')
        return;

    // Message lines, then " 1 2 3 " with a " ^ _ _ " cursor row under it, then the nullptr terminator.
    const char *linePointers[MAX_LINES + 3] = {0};
    uint16_t lineCount = splitBannerMessageLines(linePointers);
    char cells[2 * sizeof(alphanumericValue) + 1];
    char cursor[sizeof(cells)];
    cells[0] = cursor[0] = ' ';
    for (uint8_t i = 0; i < positions; i++) {
        cells[1 + 2 * i] = value[i];
        cursor[1 + 2 * i] = (i == curSelected) ? '^' : '_';
        cells[2 + 2 * i] = cursor[2 + 2 * i] = ' ';
    }
    cells[1 + 2 * positions] = cursor[1 + 2 * positions] = '\0';
    linePointers[lineCount] = cells;
    linePointers[lineCount + 1] = cursor;
    linePointers[lineCount + 2] = nullptr;

    drawNotificationBox(display, state, linePointers, lineCount + 2, 0);
}

void NotificationRenderer::drawNodePicker(OLEDDisplay *display, OLEDDisplayUiState *state)
{
    static uint32_t selectedNodenum = 0;

    // === Layout Configuration ===
    constexpr uint16_t vPadding = 2;
    alertBannerOptions = nodeDB->getNumMeshNodes() - 1;

    // let the box drawing function calculate the widths?

    const char *lineStarts[MAX_LINES + 1] = {0};
    uint16_t lineCount = splitBannerMessageLines(lineStarts);

    // Handle input
    if (inEvent.inputEvent == INPUT_BROKER_UP || inEvent.inputEvent == INPUT_BROKER_LEFT ||
        inEvent.inputEvent == INPUT_BROKER_ALT_PRESS || inEvent.inputEvent == INPUT_BROKER_UP_LONG) {
        curSelected--;
    } else if (inEvent.inputEvent == INPUT_BROKER_DOWN || inEvent.inputEvent == INPUT_BROKER_RIGHT ||
               inEvent.inputEvent == INPUT_BROKER_USER_PRESS || inEvent.inputEvent == INPUT_BROKER_DOWN_LONG) {
        curSelected++;
    } else if (inEvent.inputEvent == INPUT_BROKER_SELECT) {
        alertBannerCallback(selectedNodenum);
        resetBanner();
        return;
    } else if ((inEvent.inputEvent == INPUT_BROKER_CANCEL || inEvent.inputEvent == INPUT_BROKER_ALT_LONG) &&
               alertBannerUntil != 0) {
        resetBanner();
        return;
    }

    if (curSelected == -1)
        curSelected = alertBannerOptions - 1;
    if (curSelected == alertBannerOptions)
        curSelected = 0;

    inEvent.inputEvent = INPUT_BROKER_NONE;
    if (alertBannerMessage[0] == '\0')
        return;

    uint16_t totalLines = lineCount + alertBannerOptions;
    uint16_t screenHeight = display->height();
    uint8_t effectiveLineHeight = FONT_HEIGHT_SMALL - 3;
    uint8_t visibleTotalLines = (uint8_t)std::min<int>(totalLines, (screenHeight - vPadding * 2) / effectiveLineHeight);
    uint8_t linesShown = lineCount;
    const char *linePointers[visibleTotalLines + 1] = {0}; // this is sort of a dynamic allocation

    // copy the linestarts to display to the linePointers holder
    for (int i = 0; i < lineCount; i++) {
        linePointers[i] = lineStarts[i];
    }
    char scratchLineBuffer[visibleTotalLines - lineCount][64];

    uint16_t firstOptionToShow = 0;
    if (curSelected > 1 && alertBannerOptions > visibleTotalLines - lineCount) {
        if (curSelected > alertBannerOptions - visibleTotalLines + lineCount)
            firstOptionToShow = alertBannerOptions - visibleTotalLines + lineCount;
        else
            firstOptionToShow = curSelected - 1;
    } else {
        firstOptionToShow = 0;
    }
    int scratchLineNum = 0;
    for (int i = firstOptionToShow; i < alertBannerOptions && linesShown < visibleTotalLines; i++, linesShown++) {
        char tempName[48] = {0};
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i + 1);
        if (nodeInfoLiteHasUser(node)) {
            const char *rawName = node->long_name[0] ? node->long_name : (node->short_name[0] ? node->short_name : nullptr);
            if (rawName) {
                const int arrowWidth = (currentResolution == ScreenResolution::High)
                                           ? UIRenderer::measureStringWithEmotes(display, ">  <")
                                           : UIRenderer::measureStringWithEmotes(display, "><");
                const bool compactPanel = graphics::isCompactPanel(display);
                // Compact panels: box spans the full width, so just a small edge margin.
                const int margin = compactPanel ? 4 : 28;
                const int maxTextWidth = std::max(0, display->getWidth() - margin - arrowWidth);
                UIRenderer::truncateStringWithEmotes(display, rawName, tempName, sizeof(tempName), maxTextWidth,
                                                     compactPanel ? "" : "...");
            }
        }
        if (!tempName[0]) {
            snprintf(tempName, sizeof(tempName), "(%04X)", (uint16_t)(node ? (node->num & 0xFFFF) : 0));
        }
        const char *format = "%s";
        if (i == curSelected) {
            selectedNodenum = node ? node->num : 0;
            format = (currentResolution == ScreenResolution::High) ? "> %s <" : ">%s<";
        }
        snprintf(scratchLineBuffer[scratchLineNum], sizeof(scratchLineBuffer[scratchLineNum]), format, tempName);
        linePointers[linesShown] = scratchLineBuffer[scratchLineNum++];
    }
    drawNotificationBox(display, state, linePointers, totalLines, firstOptionToShow);
}

void NotificationRenderer::drawAlertBannerOverlay(OLEDDisplay *display, OLEDDisplayUiState *state)
{
    // === Layout Configuration ===
    constexpr uint16_t vPadding = 2;

    uint16_t optionWidths[alertBannerOptions] = {0};
    uint16_t maxWidth = 0;
    uint16_t arrowsWidth = display->getStringWidth(">  <", 4, true);
    uint16_t lineWidths[MAX_LINES] = {0};
    uint16_t lineLengths[MAX_LINES] = {0};
    const char *lineStarts[MAX_LINES + 1] = {0};
    uint16_t lineCount = 0;
    char lineBuffer[40] = {0};
    bool useTaggedTextBanner = ((current_notification_type == notificationTypeEnum::text_banner ||
                                 current_notification_type == notificationTypeEnum::pairing_pin) &&
                                alertBannerOptions == 0 && alertBannerLineCount > 0);

    if (useTaggedTextBanner) {
        lineCount = std::min<uint8_t>(alertBannerLineCount, MAX_LINES);
        for (uint16_t i = 0; i < lineCount; i++) {
            lineStarts[i] = alertBannerLines[i];
            lineLengths[i] = strlen(lineStarts[i]);
            display->setFont(fontForBannerLine(alertBannerLineFonts[i]));
            lineWidths[i] = display->getStringWidth(lineStarts[i], lineLengths[i], true);
            if (lineWidths[i] > maxWidth)
                maxWidth = lineWidths[i];
        }
    } else {
        char *alertEnd = alertBannerMessage + strnlen(alertBannerMessage, sizeof(alertBannerMessage));
        lineStarts[lineCount] = alertBannerMessage;

        while ((lineCount < MAX_LINES) && (lineStarts[lineCount] < alertEnd)) {
            lineStarts[lineCount + 1] = std::find((char *)lineStarts[lineCount], alertEnd, '\n');
            lineLengths[lineCount] = lineStarts[lineCount + 1] - lineStarts[lineCount];
            if (lineStarts[lineCount + 1][0] == '\n')
                lineStarts[lineCount + 1] += 1;
            lineWidths[lineCount] = display->getStringWidth(lineStarts[lineCount], lineLengths[lineCount], true);
            if (lineWidths[lineCount] > maxWidth)
                maxWidth = lineWidths[lineCount];
            lineCount++;
        }
    }

    // Measure option widths
    display->setFont(FONT_SMALL);
    for (int i = 0; i < alertBannerOptions; i++) {
        optionWidths[i] = display->getStringWidth(optionsArrayPtr[i], strlen(optionsArrayPtr[i]), true);
        if (optionWidths[i] > maxWidth)
            maxWidth = optionWidths[i];
        if (optionWidths[i] + arrowsWidth > maxWidth)
            maxWidth = optionWidths[i] + arrowsWidth;
    }

    // Handle input
    if (alertBannerOptions > 0) {
        if (inEvent.inputEvent == INPUT_BROKER_UP || inEvent.inputEvent == INPUT_BROKER_LEFT ||
            inEvent.inputEvent == INPUT_BROKER_ALT_PRESS || inEvent.inputEvent == INPUT_BROKER_UP_LONG) {
            curSelected--;
        } else if (inEvent.inputEvent == INPUT_BROKER_DOWN || inEvent.inputEvent == INPUT_BROKER_RIGHT ||
                   inEvent.inputEvent == INPUT_BROKER_USER_PRESS || inEvent.inputEvent == INPUT_BROKER_DOWN_LONG) {
            curSelected++;
        } else if (inEvent.inputEvent == INPUT_BROKER_SELECT) {
            if (optionsEnumPtr != nullptr) {
                alertBannerCallback(optionsEnumPtr[curSelected]);
                optionsEnumPtr = nullptr;
            } else {
                alertBannerCallback(curSelected);
            }
            resetBanner();
            return;
        } else if ((inEvent.inputEvent == INPUT_BROKER_CANCEL || inEvent.inputEvent == INPUT_BROKER_ALT_LONG) &&
                   alertBannerUntil != 0) {
            // Cancel picks the menu's own Back row where it has one, so a submenu returns to its parent - each Back
            // handler knows where that is - rather than closing everything. Without a Back row it just closes.
            if (alertBannerCallback && optionsArrayPtr && optionsArrayPtr[0] && strcmp(optionsArrayPtr[0], "Back") == 0) {
                if (optionsEnumPtr != nullptr) {
                    alertBannerCallback(optionsEnumPtr[0]);
                    optionsEnumPtr = nullptr;
                } else {
                    alertBannerCallback(0);
                }
            }
            resetBanner();
            return;
        }

        if (curSelected == -1)
            curSelected = alertBannerOptions - 1;
        if (curSelected == alertBannerOptions)
            curSelected = 0;
    } else {
        if (inEvent.inputEvent == INPUT_BROKER_SELECT || inEvent.inputEvent == INPUT_BROKER_ALT_LONG ||
            inEvent.inputEvent == INPUT_BROKER_CANCEL) {
            resetBanner();
            return;
        }
    }

    inEvent.inputEvent = INPUT_BROKER_NONE;
    if (alertBannerMessage[0] == '\0')
        return;

    uint16_t totalLines = lineCount + alertBannerOptions;

    uint16_t screenHeight = display->height();
    uint8_t effectiveLineHeight = FONT_HEIGHT_SMALL - 3;
    // Pairing PIN: pass every line, drawNotificationBox fits them (tiny panels spread them over the full screen).
    uint8_t visibleTotalLines = (current_notification_type == notificationTypeEnum::pairing_pin)
                                    ? totalLines
                                    : (uint8_t)std::min<int>(totalLines, (screenHeight - vPadding * 2) / effectiveLineHeight);
    uint8_t linesShown = lineCount;
    const char *linePointers[visibleTotalLines + 1] = {0}; // this is sort of a dynamic allocation

    // copy the linestarts to display to the linePointers holder
    for (uint16_t i = 0; i < lineCount && i < visibleTotalLines; i++) {
        linePointers[i] = lineStarts[i];
    }

    uint16_t firstOptionToShow = 0;
    if (alertBannerOptions > 0) {
        if (visibleTotalLines - lineCount == 1) {
            firstOptionToShow = curSelected;
        } else if (curSelected > 1 && alertBannerOptions > visibleTotalLines - lineCount) {
            if (curSelected > alertBannerOptions - visibleTotalLines + lineCount)
                firstOptionToShow = alertBannerOptions - visibleTotalLines + lineCount;
            else
                firstOptionToShow = curSelected - 1;
        } else {
            firstOptionToShow = 0;
        }
    }
    // Useful log line for troubleshooting:
    /* LOG_WARN("alertBannerOptions: %u, curSelected: %u, visibleTotalLines: %u, lineCount: %u, firstOptionToShow: %u",
             alertBannerOptions, curSelected, visibleTotalLines, lineCount, firstOptionToShow); */

    for (int i = firstOptionToShow; i < alertBannerOptions && linesShown < visibleTotalLines; i++, linesShown++) {
        if (i == curSelected) {
            snprintf(lineBuffer, sizeof(lineBuffer), (currentResolution == ScreenResolution::High) ? "> %s <" : ">%s<",
                     optionsArrayPtr[i]);
            linePointers[linesShown] = lineBuffer;
        } else {
            linePointers[linesShown] = optionsArrayPtr[i];
        }
    }
    if (alertBannerOptions > 0) {
        drawNotificationBox(display, state, linePointers, totalLines, firstOptionToShow, maxWidth);
    } else {
        drawNotificationBox(display, state, linePointers, totalLines, firstOptionToShow);
    }
}

void NotificationRenderer::drawBannerPanel(OLEDDisplay *display, int16_t boxLeft, int16_t boxTop, int16_t boxWidth,
                                           int16_t boxHeight)
{
    display->setColor(BLACK);
    display->fillRect(boxLeft - 1, boxTop - 1, boxWidth + 2, boxHeight + 2);
    display->fillRect(boxLeft, boxTop - 2, boxWidth, 1);
    display->fillRect(boxLeft, boxTop + boxHeight + 1, boxWidth, 1);
    display->fillRect(boxLeft - 2, boxTop, 1, boxHeight);
    display->fillRect(boxLeft + boxWidth + 1, boxTop, 1, boxHeight);
    display->setColor(WHITE);
    display->drawRect(boxLeft, boxTop, boxWidth, boxHeight);
    display->setColor(BLACK);
    display->fillRect(boxLeft, boxTop, 1, 1);
    display->fillRect(boxLeft + boxWidth - 1, boxTop, 1, 1);
    display->fillRect(boxLeft, boxTop + boxHeight - 1, 1, 1);
    display->fillRect(boxLeft + boxWidth - 1, boxTop + boxHeight - 1, 1, 1);
    display->setColor(WHITE);
#if GRAPHICS_TFT_COLORING_ENABLED
    registerTFTActionMenuRegions(boxLeft, boxTop, boxWidth, boxHeight);
#endif
#if BASEUI_NATIVE_RGB565 && GRAPHICS_TFT_COLORING_ENABLED
    // A solid panel instead of the canvas showing through: the menu's own background tinted toward the header, a
    // little lighter at the top. Explicit colour, so the regions above still colour the text drawn over it.
    uint16_t menuGradientTop = 0, menuGradientBottom = 0;
    getThemeHeaderGradient(menuGradientTop, menuGradientBottom);
    if (boxWidth > 2 && boxHeight > 2) {
        const uint16_t menuBodyBg = getActiveTheme().roles[static_cast<size_t>(TFTColorRole::ActionMenuBody)].offColor;
        const uint16_t panelTop = TFTPalette::mix565(menuBodyBg, menuGradientBottom, 72);
        const uint16_t panelBottom = TFTPalette::mix565(menuBodyBg, menuGradientBottom, 24);
        const int rows = boxHeight - 2;
        for (int row = 0; row < rows; ++row) {
            const uint8_t t = static_cast<uint8_t>(rows > 1 ? row * 255 / (rows - 1) : 0);
            static_cast<TFTDisplay *>(display)->fillRect565(boxLeft + 1, boxTop + 1 + row, boxWidth - 2, 1,
                                                            TFTPalette::mix565(panelTop, panelBottom, t));
        }
    }
#endif
}

// A row is drawn with emotes in the node picker (names), and wherever it holds non-ASCII text - an emote the font
// can't draw. Plain rows keep the cheaper drawString.
static bool rowDrawsEmotes(const char *row)
{
    if (NotificationRenderer::current_notification_type == notificationTypeEnum::node_picker)
        return true;
    for (const char *c = row; *c; c++)
        if ((unsigned char)*c >= 0x80)
            return true;
    return false;
}

void NotificationRenderer::drawNotificationBox(OLEDDisplay *display, OLEDDisplayUiState *state, const char *lines[],
                                               uint16_t totalLines, uint16_t firstOptionToShow, uint16_t maxWidth)
{

    bool is_picker = false;
    uint16_t lineCount = 0;
    // Layout Configuration
    constexpr uint16_t hPadding = 5;
    constexpr uint16_t vPadding = 2;
    bool needs_bell = false;
    uint16_t lineWidths[totalLines] = {0};
    uint16_t lineLengths[totalLines] = {0};
    BannerFont lineFonts[totalLines] = {};
    uint8_t lineEffectiveHeights[totalLines] = {0};
    const char *renderLines[totalLines] = {0};

    if (maxWidth != 0)
        is_picker = true;

    // Setup font and alignment
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Track widest line INCLUDING bars (but don't change per-line widths)
    uint16_t widestLineWithBars = 0;

    while (lines[lineCount] != nullptr) {
        BannerFont lineFont = BANNER_FONT_DEFAULT;
        const char *renderText = resolveBannerLine(lineCount, lines[lineCount], lineFont);
        renderLines[lineCount] = renderText;
        lineFonts[lineCount] = lineFont;
        lineEffectiveHeights[lineCount] = effectiveLineHeightForBannerLine(lineFont);
        display->setFont(fontForBannerLine(lineFont));

        auto newlinePointer = strchr(renderText, '\n');
        if (newlinePointer)
            lineLengths[lineCount] = (newlinePointer - renderText);
        else
            lineLengths[lineCount] = strlen(renderText);

        if (current_notification_type == notificationTypeEnum::node_picker) {
            char measureBuffer[64] = {0};
            strncpy(measureBuffer, renderText, std::min<size_t>(lineLengths[lineCount], sizeof(measureBuffer) - 1));
            lineWidths[lineCount] = UIRenderer::measureStringWithEmotes(display, measureBuffer);
        } else {
            lineWidths[lineCount] = display->getStringWidth(renderText, lineLengths[lineCount], true);
        }

        // Consider extra width for signal bars on lines that contain "Signal:"
        uint16_t potentialWidth = lineWidths[lineCount];
        if (graphics::bannerSignalBars >= 0 && strncmp(renderText, "Signal:", 7) == 0) {
            const int totalBars = 5;
            const int barWidth = 3;
            const int barSpacing = 2;
            const int gap = 6; // space between text and bars
            int barsWidth = totalBars * barWidth + (totalBars - 1) * barSpacing + gap;
            potentialWidth += barsWidth;
        }

        if (potentialWidth > widestLineWithBars)
            widestLineWithBars = potentialWidth;

        if (!is_picker) {
            needs_bell |= (strstr(alertBannerMessage, "Alert Received") != nullptr);
            if (lineWidths[lineCount] > maxWidth)
                maxWidth = lineWidths[lineCount];
        }
        lineCount++;
    }
    // count lines

    // Ensure box accounts for signal bars if present
    if (widestLineWithBars > maxWidth)
        maxWidth = widestLineWithBars;

    uint16_t boxWidth = hPadding * 2 + maxWidth;

    if (needs_bell) {
        if ((currentResolution == ScreenResolution::High) && boxWidth <= 150)
            boxWidth += 26;
        if ((currentResolution == ScreenResolution::Low || currentResolution == ScreenResolution::UltraLow) && boxWidth <= 100)
            boxWidth += 20;
    }

    uint16_t screenHeight = display->height();
    uint8_t effectiveLineHeight = FONT_HEIGHT_SMALL - 3;
    uint8_t visibleTotalLines = 0;
    uint16_t contentHeight = 0;
#if defined(OLED_TINY)
    // Tiny panels: the pairing PIN takes the whole screen, all lines shown and spread evenly over it.
    const bool fullScreenPin = (current_notification_type == notificationTypeEnum::pairing_pin);
#else
    const bool fullScreenPin = false;
#endif
    const uint16_t availableHeight = (screenHeight > (vPadding * 2)) ? (screenHeight - vPadding * 2) : 0;
    for (uint8_t i = 0; i < lineCount; i++) {
        uint8_t thisLineHeight = lineEffectiveHeights[i] ? lineEffectiveHeights[i] : effectiveLineHeight;
        if (!fullScreenPin && contentHeight + thisLineHeight > availableHeight) {
            break;
        }
        contentHeight += thisLineHeight;
        visibleTotalLines++;
    }
    if (visibleTotalLines == 0 && lineCount > 0) {
        visibleTotalLines = 1;
        contentHeight = lineEffectiveHeights[0] ? lineEffectiveHeights[0] : effectiveLineHeight;
    }
    uint16_t boxHeight = contentHeight + vPadding * 2;
    if (visibleTotalLines == 1) {
        boxHeight += (currentResolution == ScreenResolution::High) ? 4 : 3;
    }

    int16_t boxLeft = (display->width() / 2) - (boxWidth / 2);
    if (totalLines > visibleTotalLines) {
        boxWidth += (currentResolution == ScreenResolution::High) ? 4 : 2;
    }
    int16_t boxTop = (display->height() / 2) - (boxHeight / 2);
    boxHeight += (currentResolution == ScreenResolution::High) ? 2 : 1;
    if (fullScreenPin || graphics::isCompactPanel(display)) {
        boxLeft = 0;
        boxTop = 0;
        boxWidth = display->width();
        boxHeight = display->height();
    } else {
#if defined(OLED_TINY)
        if (visibleTotalLines == 1) {
            boxTop += 25;
        }
        if (alertBannerOptions < 3) {
            int missingLines = 3 - alertBannerOptions;
            int moveUp = missingLines * (effectiveLineHeight / 2);
            boxTop -= moveUp;
            if (boxTop < 0)
                boxTop = 0;
        }
#endif
    }

    // Draw Box
    drawBannerPanel(display, boxLeft, boxTop, boxWidth, boxHeight);
#if BASEUI_NATIVE_RGB565 && GRAPHICS_TFT_COLORING_ENABLED
    uint16_t menuGradientTop = 0, menuGradientBottom = 0; // the title bar below takes the header's gradient too
    getThemeHeaderGradient(menuGradientTop, menuGradientBottom);
#endif

    // Draw Content
    int16_t lineY = boxTop + vPadding;
    for (int i = 0; i < visibleTotalLines; i++) {
        display->setFont(fontForBannerLine(lineFonts[i]));
        int16_t thisLineHeight = lineEffectiveHeights[i] ? lineEffectiveHeights[i] : effectiveLineHeight;
        if (fullScreenPin) {
            // Equal slots over the full height (10 rows each on a 32px panel, glyphs sit in rows 3..9).
            thisLineHeight = boxHeight / visibleTotalLines;
            lineY = i * thisLineHeight;
        }
        int16_t textX = boxLeft + (boxWidth - lineWidths[i]) / 2;
        if (needs_bell && i == 0) {
            int fontHeight = thisLineHeight + 3;
            int bellY = lineY + (fontHeight - bell_alert_height * BASEUI_ICON_SCALE) / 2;
            drawScaledXbm(display, textX - 2 - bell_alert_width * BASEUI_ICON_SCALE, bellY, bell_alert_width, bell_alert_height,
                          bell_alert);
            drawScaledXbm(display, textX + lineWidths[i] + 2, bellY, bell_alert_width, bell_alert_height, bell_alert);
        }
        char lineBuffer[lineLengths[i] + 1];
        strncpy(lineBuffer, renderLines[i], lineLengths[i]);
        lineBuffer[lineLengths[i]] = '\0';
        // Determine if this is a pop-up or a pick list
        if (alertBannerOptions > 0 && i == 0) {
            // Pick List
            display->setColor(WHITE);
            int background_yOffset = 1;
            // Determine if we have low hanging characters
            if (strchr(lineBuffer, 'p') || strchr(lineBuffer, 'g') || strchr(lineBuffer, 'y') || strchr(lineBuffer, 'j')) {
                background_yOffset = -1;
            }
            const int16_t titleBarY = boxTop + 1;
            const int16_t titleBarHeight = effectiveLineHeight - background_yOffset;
            display->fillRect(boxLeft, titleBarY, boxWidth, titleBarHeight);
#if GRAPHICS_TFT_COLORING_ENABLED
            if (alertBannerOptions > 0) {
                const uint16_t titleTextColor =
                    (getActiveTheme().id == ThemeID::DefaultLight) ? TFTPalette::Black : getThemeHeaderText();
                // Keep title role away from border/corner pixels so rounded-corner masks are not remapped to the title text
                // color.
                if (boxWidth > 2 && titleBarHeight > 0) {
                    setAndRegisterTFTColorRole(TFTColorRole::ActionMenuTitle, getThemeHeaderBg(), titleTextColor, boxLeft + 1,
                                               titleBarY, boxWidth - 2, titleBarHeight);
#if BASEUI_NATIVE_RGB565
                    // The header's gradient behind the title; the title role then only colours the glyphs.
                    for (int row = 0; row < titleBarHeight; ++row) {
                        const uint8_t t = static_cast<uint8_t>(titleBarHeight > 1 ? row * 255 / (titleBarHeight - 1) : 0);
                        static_cast<TFTDisplay *>(display)->fillRect565(
                            boxLeft + 1, titleBarY + row, boxWidth - 2, 1,
                            TFTPalette::mix565(menuGradientTop, menuGradientBottom, t));
                    }
#endif
                }
            }
#endif
            display->setColor(BLACK);
            const int yOffset = graphics::isCompactPanel(display) ? 2 : 3;
            if (rowDrawsEmotes(lineBuffer)) {
                UIRenderer::drawStringWithEmotes(display, textX, lineY - yOffset, lineBuffer, FONT_HEIGHT_SMALL, 1, false);
            } else {
                display->drawString(textX, lineY - yOffset, lineBuffer);
            }
            display->setColor(WHITE);
            lineY += (thisLineHeight - 2 - background_yOffset);
        } else {
            // Pop-up
            // If this is the Signal line, center text + bars as one group
            bool isSignalLine = (graphics::bannerSignalBars >= 0 && strstr(lineBuffer, "Signal:") != nullptr);
            if (isSignalLine) {
                const int totalBars = 5;
                const int barWidth = 3;
                const int barSpacing = 2;
                const int barHeightStep = 2;
                const int gap = 6;
                const int maxBarHeight = totalBars * barHeightStep;

                int textWidth = display->getStringWidth(lineBuffer, strlen(lineBuffer), true);
                int barsWidth = totalBars * barWidth + (totalBars - 1) * barSpacing + gap;
                int totalWidth = textWidth + barsWidth;
                int groupStartX = boxLeft + (boxWidth - totalWidth) / 2;

                if (rowDrawsEmotes(lineBuffer)) {
                    UIRenderer::drawStringWithEmotes(display, groupStartX, lineY, lineBuffer, FONT_HEIGHT_SMALL, 1, false);
                } else {
                    display->drawString(groupStartX, lineY, lineBuffer);
                }

                int baseX = groupStartX + textWidth + gap;
                int baseY = lineY + effectiveLineHeight - 1;
#if GRAPHICS_TFT_COLORING_ENABLED
                if (graphics::bannerSignalBars > 0) {
                    uint16_t signalBarsColor = TFTPalette::Medium;
                    if (graphics::bannerSignalBars <= 1) {
                        signalBarsColor = TFTPalette::Bad;
                    } else if (graphics::bannerSignalBars >= 4) {
                        signalBarsColor = TFTPalette::Good;
                    }
                    const int activeBars = min(graphics::bannerSignalBars, totalBars);
                    const int regionWidth = activeBars * barWidth + (activeBars - 1) * barSpacing;
                    setAndRegisterTFTColorRole(TFTColorRole::SignalBars, signalBarsColor, TFTPalette::Black, baseX,
                                               baseY - maxBarHeight, regionWidth, maxBarHeight);
                }
#endif
                for (int b = 0; b < totalBars; b++) {
                    int barHeight = (b + 1) * barHeightStep;
                    int x = baseX + b * (barWidth + barSpacing);
                    int y = baseY - barHeight;

                    if (b < graphics::bannerSignalBars) {
                        display->fillRect(x, y, barWidth, barHeight);
                    } else {
                        display->drawRect(x, y, barWidth, barHeight);
                    }
                }
            } else {
                if (rowDrawsEmotes(lineBuffer)) {
                    UIRenderer::drawStringWithEmotes(display, textX, lineY, lineBuffer, FONT_HEIGHT_SMALL, 1, false);
                } else {
                    display->drawString(textX, lineY, lineBuffer);
                }
            }
            lineY += thisLineHeight;
        }
    }

    // Scroll Bar (Thicker, inside box, not over title)
    if (totalLines > visibleTotalLines) {
        const uint8_t scrollBarWidth = 5;
        int16_t scrollBarX = boxLeft + boxWidth - scrollBarWidth - 2;
        int16_t scrollBarY = boxTop + vPadding + effectiveLineHeight;
        uint16_t scrollBarHeight = boxHeight - vPadding * 2 - effectiveLineHeight;

        float ratio = (float)visibleTotalLines / totalLines;
        uint16_t indicatorHeight = std::max((int)(scrollBarHeight * ratio), 4);
        float scrollRatio = (float)(firstOptionToShow + lineCount - visibleTotalLines) / (totalLines - visibleTotalLines);
        uint16_t indicatorY = scrollBarY + scrollRatio * (scrollBarHeight - indicatorHeight);

        display->drawRect(scrollBarX, scrollBarY, scrollBarWidth, scrollBarHeight);
        display->fillRect(scrollBarX + 1, indicatorY, scrollBarWidth - 2, indicatorHeight);
    }
}

/// Draw the last text message we received
void NotificationRenderer::drawCriticalFaultFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_MEDIUM);

    char tempBuf[24];
    snprintf(tempBuf, sizeof(tempBuf), "Critical fault #%d", error_code);
    display->drawString(0 + x, 0 + y, tempBuf);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    display->drawString(0 + x, FONT_HEIGHT_MEDIUM + y, "For help, please visit \nmeshtastic.org");
}

void NotificationRenderer::drawFrameFirmware(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(FONT_MEDIUM);
    display->drawString(64 + x, y, "Updating");

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawStringMaxWidth(0 + x, 2 + y + FONT_HEIGHT_SMALL * 2, x + display->getWidth(),
                                "Please be patient and do not power off.");
}

void NotificationRenderer::drawTextInput(OLEDDisplay *display, OLEDDisplayUiState *state)
{
    if (virtualKeyboard) {
        // Check for timeout and auto-exit if needed
        if (virtualKeyboard->isTimedOut()) {
            LOG_INFO("Virtual keyboard timeout - auto-exiting");
            // Cancel virtual keyboard - call callback with empty string to indicate timeout
            auto callback = textInputCallback; // Store callback before clearing

            // Clean up first to prevent re-entry. The keyboard belongs to OnScreenKeyboardModule; only stop()
            // may free it, and it clears virtualKeyboard/textInputCallback for us.
            OnScreenKeyboardModule::instance().stop(false);
            resetBanner();

            // Call callback after cleanup
            if (callback) {
                callback("");
            }

            // Restore normal overlays
            if (screen) {
                screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            }
            return;
        }

        if (inEvent.inputEvent != INPUT_BROKER_NONE) {
            bool handled = OnScreenKeyboardModule::processVirtualKeyboardInput(inEvent, virtualKeyboard);
            if (!handled && inEvent.inputEvent == INPUT_BROKER_CANCEL) {
                auto callback = textInputCallback;
                OnScreenKeyboardModule::instance().stop(false); // sole owner of the keyboard; also clears our aliases
                resetBanner();
                if (callback) {
                    callback("");
                }
                if (screen) {
                    screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
                }
                return;
            }

            // Consume the event after processing for virtual keyboard
            inEvent.inputEvent = INPUT_BROKER_NONE;
        }

        // Re-check pointer before drawing to avoid use-after-free and crashes
        if (!virtualKeyboard) {
            // Ensure we exit text_input state and restore frames
            if (current_notification_type == notificationTypeEnum::text_input) {
                resetBanner();
            }
            if (screen) {
                screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
            }
            // If screen is null, do nothing (safe fallback)
            return;
        }

        // Clear the screen to avoid overlapping with underlying frames or overlays
        display->setColor(BLACK);
        display->fillRect(0, 0, display->getWidth(), display->getHeight());
        display->setColor(WHITE);
        // Draw the virtual keyboard
        virtualKeyboard->draw(display, 0, 0);
    } else {
        // If virtualKeyboard is null, reset the banner to avoid getting stuck
        LOG_INFO("Virtual keyboard is null - resetting banner");
        resetBanner();
    }
}

bool NotificationRenderer::isOverlayBannerShowing()
{
    // Here 0 means "show indefinitely", so it must short-circuit the comparison.
    return strlen(alertBannerMessage) > 0 && (alertBannerUntil == 0 || !Throttle::deadlinePassed(alertBannerUntil));
}

bool NotificationRenderer::isScrollableList()
{
    return isOverlayBannerShowing() && alertBannerOptions > 0 &&
           (current_notification_type == notificationTypeEnum::selection_picker ||
            current_notification_type == notificationTypeEnum::node_picker);
}

// Travel not yet worth a whole row, carried between drag reports.
static float fingerScrollCarry = 0.0f;

void NotificationRenderer::scrollByFingerDelta(float dyPx)
{
    if (!isScrollableList())
        return;
    const float rowHeight = (float)(FONT_HEIGHT_SMALL - 3); // effectiveLineHeight in the drawing code
    // Dragging up (dyPx < 0) pulls later options up under the finger, so it walks down the list. Clamped
    // rather than wrapped: wrapping mid-drag would throw the selection to the far end of the list.
    fingerScrollCarry -= dyPx;
    while (fingerScrollCarry >= rowHeight && curSelected < alertBannerOptions - 1) {
        curSelected++;
        fingerScrollCarry -= rowHeight;
    }
    while (fingerScrollCarry <= -rowHeight && curSelected > 0) {
        curSelected--;
        fingerScrollCarry += rowHeight;
    }
    if ((fingerScrollCarry > 0.0f && curSelected >= alertBannerOptions - 1) || (fingerScrollCarry < 0.0f && curSelected <= 0))
        fingerScrollCarry = 0.0f; // at an end: travel past it must not bank up and fire on the way back
}

void NotificationRenderer::endFingerScroll()
{
    fingerScrollCarry = 0.0f;
}

bool NotificationRenderer::isMenuShowing()
{
    // A menu, picker, keyboard, or pairing-PIN overlay - anything interactive, as opposed to a plain
    // informational text banner (which has no options and type text_banner). Menus don't set a
    // notificationType of their own, so options are the only thing distinguishing them.
    return isOverlayBannerShowing() && (alertBannerOptions > 0 || current_notification_type != notificationTypeEnum::text_banner);
}

} // namespace graphics
#endif
