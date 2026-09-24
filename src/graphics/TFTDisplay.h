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

    // Force every unlit body pixel to one solid colour, overriding the theme canvas and any
    // background artwork. The lockscreen uses it; pass false to hand the canvas back to the theme.
    void setCanvasOverride(bool active, uint16_t color565 = 0);

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
    // quiet skips the log line, for callers that ramp the level rather than set it once.
    void setDisplayBrightness(uint8_t, bool quiet = false);

#if defined(ST7789_CS) && !defined(USE_ARDUINO_GFX) && (defined(ST7789_VCOMS) || BASEUI_PANEL_VCOM_TUNING)
#define TFT_HAS_PANEL_VCOM 1
    // ST7789 VCOM (VCOMS, 0xBB): 0.1V plus 0.025V a step. One that doesn't suit the panel leaves a DC bias on the
    // liquid crystal, seen as a faint image of whatever sat on screen. Applied at init and wake; not persisted.
    static void setPanelVcom(uint8_t vcoms);
    static uint8_t panelVcom();
#else
#define TFT_HAS_PANEL_VCOM 0
#endif

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
    // Blit a full-colour image (native-endian RGB565, w*h pixels). Later regions don't recolour it. With
    // zeroIsTransparent, 0x0000 pixels are skipped and whatever is underneath shows through.
    void drawRGB565(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels, bool zeroIsTransparent = false);
    // Fill a rect with one native-endian RGB565 colour as background: unlit, and not recoloured by regions.
    void fillRect565(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    // As fillRect565(), but mixed into what is already there rather than replacing it. alpha is how far
    // toward `color` each pixel travels, 0-255.
    void blendRect565(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t alpha);
    // While set, clear() only resets the lit mask: the caller is about to cover every pixel itself (a slide
    // blitting its snapshots), so painting the background first would be thrown away.
    void setClearCovered(bool covered) { clearCovered = covered; }

    // Writable access, so both count as drawing: the next clear() can no longer be skipped.
    uint16_t *nativePixels()
    {
        nativeClean = false;
        markNativeRowsDirty(0, displayHeight);
        return rgbPixels;
    }
    uint8_t *explicitMask()
    {
        nativeClean = false;
        markNativeRowsDirty(0, displayHeight);
        return explicitBits;
    }
    // Copy `count` pixels of a captured row from column srcX to dstX. Pixels that were the background image where
    // they were captured take it at their new place, so a sliding frame moves over a background that holds still.
    void copySlideRow(int32_t row, const uint16_t *src, int32_t srcX, int32_t dstX, int32_t count);

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

    // Background artwork sampled with wrap, so it need not be the panel's size.
    uint16_t canvasImagePixel(int32_t x, int32_t y) const;
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
    uint16_t legacyBgBe = 0; // the theme's two-tone body background, which canvasBe replaces
    uint16_t canvasBe = 0;
    const uint16_t *canvasImage = nullptr; // panel-sized, native-endian; null for a solid canvas
    bool canvasOverrideActive = false;     // see setCanvasOverride()
    uint16_t canvasOverrideBe = 0;
    void refreshNativeThemeColors();
    // An unlit pixel in the body background takes the canvas instead: its image pixel where there is one.
    uint16_t onCanvas(bool lit, uint16_t be, int32_t x, int32_t y) const;
    uint16_t penOnBe = 0;
    uint16_t penOffBe = 0;
    bool penActive = false;
    bool forceNativePush = true;        // push every row on the next display()
    bool clearCovered = false;          // see setClearCovered()
    bool nativeClean = false;           // nothing drawn since the last clear(), so another can be skipped
    uint32_t cleanRegionGeneration = 0; // the region set that clean clear() resolved against
    int32_t cachedRowY = -1;            // row the shared region row cache currently holds
    uint32_t cachedRowGeneration = 0;
    void nativeBeginRow(int16_t y);

    // One bit per push band drawn into since the last display(), so untouched bands skip the change scan. Rows past
    // the last band always scan.
    static constexpr uint8_t kNativeBandRows = 8;
    static constexpr uint32_t kNativeMaxBands = 256;
    uint32_t nativeDirtyBands[kNativeMaxBands / 32] = {};
    void markNativeRowDirty(int32_t y)
    {
        const uint32_t band = (uint32_t)y / kNativeBandRows;
        if (band < kNativeMaxBands)
            nativeDirtyBands[band >> 5] |= 1u << (band & 31);
    }
    void markNativeRowsDirty(int32_t y0, int32_t y1)
    {
        for (int32_t y = y0 - (y0 % kNativeBandRows); y < y1; y += kNativeBandRows)
            markNativeRowDirty(y);
    }
#endif
};
