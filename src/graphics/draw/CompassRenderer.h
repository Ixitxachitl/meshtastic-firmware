#pragma once

#include "graphics/Screen.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

struct Quat;

namespace graphics
{

/// Forward declarations
class Screen;

/**
 * @brief Compass and navigation drawing functions
 *
 * Contains all functions related to drawing compass elements, headings,
 * navigation arrows, and location-based UI components.
 */
namespace CompassRenderer
{
// Compass drawing functions
void drawCompassNorth(OLEDDisplay *display, int16_t compassX, int16_t compassY, float myHeading, int16_t radius);
void drawNodeHeading(OLEDDisplay *display, int16_t compassX, int16_t compassY, uint16_t compassDiam, float headingRadian);
void drawArrowToNode(OLEDDisplay *display, int16_t x, int16_t y, int16_t size, float bearing);
// Centered "needle": one front point, two rear branching points (3D-aware)
void drawCenterNeedle3D(OLEDDisplay *display, int16_t cx, int16_t cy, uint16_t radius, const Quat &attitude, float bearingRad,
                        float elevRad = 0.0f);
// Simple needle (runtime selection between 3D and simple based on display)
void drawCenterNeedle3D(OLEDDisplay *display, int16_t cx, int16_t cy, uint16_t radius, float bearingRad, float elevRad = 0.0f);

// Navigation and location functions
float estimatedHeading(double lat, double lon);
uint16_t getCompassDiam(uint32_t displayWidth, uint32_t displayHeight);

// Spherical compass renderer (globe-like, with lat/long grid)
void drawCompassSphere(OLEDDisplay *display, int16_t cx, int16_t cy, uint16_t radius, const Quat &attitude);
// Simple compass sphere (runtime selection between 3D and simple based on display)
void drawCompassSphere(OLEDDisplay *display, int16_t cx, int16_t cy, uint16_t radius);

// Control functions for compass rendering mode
void setTopDownView(bool enable);

// Draw magnetometer calibration indicator icon
void drawMagnetIndicator(OLEDDisplay *display, int16_t x, int16_t y);

} // namespace CompassRenderer

} // namespace graphics
