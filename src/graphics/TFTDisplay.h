#pragma once

#include "configuration.h" // BASEUI_HAS_TOUCH_CALIBRATION
#include <GpioLogic.h>
#include <OLEDDisplay.h>

#if BASEUI_NATIVE_RGB565 && !defined(OLEDDISPLAY_OVERRIDABLE_DRAW)
#error "BASEUI_NATIVE_RGB565 needs the OLED library built with OLEDDISPLAY_OVERRIDABLE_DRAW"
#endif

/**
 * An adapter class that allows using the LovyanGFX library as if it was an OLEDDisplay implementation.
 *
 * Remaining TODO:
 * optimize display() to only draw changed pixels (see other OLED subclasses for examples)
 * Use the fast NRF52 SPI API rather than the slow standard arduino version
 *
 * turn radio back on - currently with both on spi bus is fucked? or are we leaving chip select asserted?
 */
class TFTDisplay : public OLEDDisplay
{
  public:
    /* constructor
    FIXME - the parameters are not used, just a temporary hack to keep working like the old displays
    */
    TFTDisplay(uint8_t, int, int, OLEDDISPLAY_GEOMETRY, HW_I2C);

    // Destructor to clean up allocated memory
    ~TFTDisplay();

    // Write the buffer to the display memory
    virtual void display() override { display(false); };
    virtual void display(bool fromBlank);
    void sdlLoop();

    // Poll the physical LEFT/RIGHT arrow keys directly (SDL desktop window only), for callers that
    // want continuous "hold to move" input instead of sdlLoop()'s single debounced event per press.
    // Mirrors LinuxJoystick::heldXZone(): -1 = left held, +1 = right held, 0 = neither/not
    // applicable (e.g. no SDL window, or not the active display panel).
    static int heldXZone();

    // Turn the display upside down
    virtual void flipScreenVertically();

    // Touch screen (static handlers)
    static bool hasTouch(void);
    static bool getTouch(int16_t *x, int16_t *y);

#if BASEUI_HAS_TOUCH_CALIBRATION
    // Touch calibration, mirroring what device-ui does from its calibration screen. `parameters` is
    // the raw controller reading at each of the four panel corners (x,y per corner, in the order
    // top-left, bottom-left, top-right, bottom-right) - the same eight uint16 values device-ui
    // stores in uiconfig.calibration_data, so the two UIs can read each other's calibration.

    // Runs the interactive four-corner routine, applies the result, and writes it to `parameters`.
    // Blocks the calling thread until every corner has been tapped. Returns false (leaving the
    // previous calibration in place) if the user gave up or the taps were too degenerate to use.
    static bool calibrateTouch(uint16_t parameters[8]);

    // Apply a previously stored set of parameters without asking the user for anything.
    static void applyTouchCalibration(const uint16_t parameters[8]);

    // Drop back to the driver's built-in mapping, as if nothing had ever been calibrated.
    static void clearTouchCalibration(void);
#endif

    // Functions for changing display brightness
    void setDisplayBrightness(uint8_t);

    /**
     * shim to make the abstraction happy
     *
     */
    void setDetected(uint8_t detected);

    /**
     * This is normally managed entirely by TFTDisplay, but some rare applications (heltec tracker) might need to replace the
     * default GPIO behavior with something a bit more complex.
     *
     * We (cruftily) make it static so that variant.cpp can access it without needing a ptr to the TFTDisplay instance.
     */
    static GpioPin *backlightEnable;

#if BASEUI_NATIVE_RGB565
    // ---- Native RGB565 drawing --------------------------------------------------------------------
    // Every OLEDDisplay primitive writes its colour straight into rgbPixels. The 1-bit buffer stays as
    // the lit mask a colour region repaints from when it is registered after the drawing it tints.

    // Draw subsequent lit/unlit pixels in fixed native-endian RGB565 colours, ignoring theme and
    // regions. clear() drops the pen, so it can't leak into the next frame.
    void setPenColors(uint16_t onColor, uint16_t offColor);
    void clearPen();
    // Blit a full-colour image (native-endian RGB565, w*h pixels). Later regions don't recolour it.
    void drawRGB565(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);

    uint16_t *nativePixels() { return rgbPixels; }
    uint8_t *explicitMask() { return explicitBits; }

    void setPixel(int16_t x, int16_t y) override;
    void setPixelColor(int16_t x, int16_t y, OLEDDISPLAY_COLOR c) override;
    void clearPixel(int16_t x, int16_t y) override;
    void drawHorizontalLine(int16_t x, int16_t y, int16_t length) override;
    void drawVerticalLine(int16_t x, int16_t y, int16_t length) override;
    void clear(void) override;
#endif

  protected:
    // the header size of the buffer used, e.g. for the SPI command header
    virtual int getBufferOffset(void) override { return 0; }

    // Send a command to the display (low level function)
    virtual void sendCommand(uint8_t com) override;

    // Connect to the display
    virtual bool connect() override;

    uint16_t *linePixelBuffer = nullptr;
    uint16_t *repaintChunkBuffer = nullptr;

    // True when both pixel buffers above live in RAM the SPI DMA engine can reach. That is what
    // lets display() ask for a DMA push instead of LovyanGFX's byte-at-a-time PIO fallback.
    bool pixelBuffersAreDmaCapable = false;

    // How many chunk-sized slots repaintChunkBuffer actually holds. Two when the internal heap
    // could spare it, so conversion overlaps transfer; one otherwise.
    uint8_t chunkBufferSlots = 1;

    // Send a block of pre-swapped RGB565 pixels to the panel, by DMA where that is available.
    // Non-const data: TFT_eSPI's pushImage() takes a mutable pointer.
    void pushPixelBlock(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *data);

    // Bracket a run of pushPixelBlock() calls so their transfers can overlap the work that
    // prepares the next one. Must be paired.
    void beginPixelBatch();
    void endPixelBatch();

#if BASEUI_NATIVE_RGB565
    void drawInternal(int16_t xMove, int16_t yMove, int16_t width, int16_t height, const uint8_t *data, uint16_t offset,
                      uint16_t bytesInData) override;

    // Colour one pixel from its lit bit: the pen if one is set, else the regions, else the theme.
    void writeNativePixel(int16_t x, int16_t y);
    // Repaint a just-registered region's rect from the lit mask, skipping explicitly coloured pixels.
    void repaintRegion(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t onColorBe, uint16_t offColorBe);
    static void onColorRegionAdded(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t onColorBe, uint16_t offColorBe);
    static TFTDisplay *nativeInstance;

    uint16_t *rgbPixels = nullptr;   // what the panel should show, big-endian RGB565
    uint16_t *rgbPushed = nullptr;   // what the panel was last sent, for the change scan
    uint8_t *explicitBits = nullptr; // buffer's page layout; set where a pen or image chose the colour
    uint16_t defaultOnBe = 0;
    uint16_t defaultOffBe = 0;
    uint16_t penOnBe = 0;
    uint16_t penOffBe = 0;
    bool penActive = false;
    bool forceNativePush = true; // push every row on the next display()
#endif
};
