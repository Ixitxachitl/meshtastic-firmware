#pragma once

#include "configuration.h"
#include <OLEDDisplay.h>
#include <functional>
#include <string>

namespace graphics
{

enum VirtualKeyType { VK_CHAR, VK_BACKSPACE, VK_ENTER, VK_SHIFT, VK_ESC, VK_SPACE, VK_EMOTE };

struct VirtualKey {
    char character;
    VirtualKeyType type;
    uint8_t x;
    uint8_t y;
    uint8_t width;
    uint8_t height;
};

class VirtualKeyboard
{
  public:
    VirtualKeyboard();
    ~VirtualKeyboard();

    void draw(OLEDDisplay *display, int16_t offsetX, int16_t offsetY);
    void setInputText(const std::string &text);
    std::string getInputText() const;
    void setHeader(const std::string &header);
    void setHeaderFocused(bool focused); // Set whether header is focused/highlighted
    void setCallback(std::function<void(const std::string &)> callback);

    // Navigation methods for encoder input
    void moveCursorUp();
    void moveCursorDown();
    void moveCursorLeft();
    void moveCursorRight();
    void setCursorPosition(uint8_t row, uint8_t col);  // Set cursor to specific position
    uint8_t getCursorRow() const { return cursorRow; } // Get current cursor row
    void handlePress();
    void handleLongPress();

    // Timeout management
    void resetTimeout();
    bool isTimedOut() const;
    void disableTimeout();

  private:
    static const uint8_t KEYBOARD_ROWS = 4;
    static const uint8_t KEYBOARD_COLS = 12; // Row 2 has 12 columns (chars + space), row 3 has 12 (chars + emote + ESC)
    static const uint8_t KEY_WIDTH = 9;
    static const uint8_t KEY_HEIGHT = 9;        // Compressed to fit 5 rows on displays
    static const uint8_t KEYBOARD_START_Y = 26; // Start just below input box bottom

    VirtualKey keyboard[KEYBOARD_ROWS][KEYBOARD_COLS];

    std::string inputText;
    std::string headerText;
    bool headerFocused; // Whether the header is focused/highlighted
    bool shiftActive;   // Whether shift mode is active (shows uppercase/modifiers)
    std::function<void(const std::string &)> onTextEntered;

    uint8_t cursorRow;
    uint8_t cursorCol;

    // Timeout management for auto-exit
    uint32_t lastActivityTime;
    static const uint32_t TIMEOUT_MS = 0; // Disabled timeout for trackball devices
    bool timeoutDisabled;

    void initializeKeyboard();
    void drawKey(OLEDDisplay *display, const VirtualKey &key, bool selected, int16_t x, int16_t y, uint8_t w, uint8_t h,
                 bool isLastCol);
    void drawInputArea(OLEDDisplay *display, int16_t offsetX, int16_t offsetY, int16_t keyboardStartY);

    // Unified cursor movement helper
    void moveCursorDelta(int dRow, int dCol);

    char getCharForKey(const VirtualKey &key, bool isLongPress = false);
    void insertCharacter(char c);
    void deleteCharacter();
    void submitText();
};

} // namespace graphics
