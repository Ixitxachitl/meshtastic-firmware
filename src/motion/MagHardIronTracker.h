#pragma once

#include <cstdint>

// Keeps a magnetometer's hard-iron centre fitted while the device moves: samples spread over the field sphere are
// fitted at the calibrated field radius, and a fit is taken only when the samples sit cleanly on it.
class MagHardIronTracker
{
  public:
    // Starts over from a calibration's centre and field radius (G); a radius outside 0.1-2 G leaves tracking off.
    // Detected axis signs are kept: they describe this boot's sensor, not the calibration.
    void reset(float centreX, float centreY, float centreZ, float fieldRadius);

    // One raw magnetometer sample (G) and the accelerometer sample (g) taken with it; hemisphere is +1 north, -1 south,
    // 0 unknown. True when a fit was applied.
    bool addSample(float magX, float magY, float magZ, float accelX, float accelY, float accelZ, uint32_t nowMs,
                   int8_t hemisphere);

    float centreX() const { return centre[0]; }
    float centreY() const { return centre[1]; }
    float centreZ() const { return centre[2]; }
    // +1 or -1 per axis, applied after subtracting the centre. A reversed axis still fits the sphere, so it is found
    // from the field's dip instead, which stays fixed against gravity only with the right signs.
    float signX() const { return axisSign[0]; }
    float signY() const { return axisSign[1]; }
    float signZ() const { return axisSign[2]; }
    float lastRms() const { return fitRms; }
    uint32_t fitCount() const { return fits; }
    uint32_t signChangeCount() const { return signChanges; }
    uint8_t sampleCount() const { return count; }

  private:
    static constexpr uint8_t kSlots = 24;

    struct Sample {
        float field[3];
        float down[3];
        uint32_t atMs;
        bool used;
    };

    bool refine(float c[3], const float prior[3], float &rms) const;
    bool fit(int8_t hemisphere);
    void checkAxisSigns(int8_t hemisphere);

    Sample samples[kSlots] = {};
    float centre[3] = {0.0f, 0.0f, 0.0f};
    float axisSign[3] = {1.0f, 1.0f, 1.0f};
    float radius = 0.0f;
    float fitRms = 0.0f;
    uint32_t fits = 0;
    uint32_t signChanges = 0;
    uint8_t count = 0;
    uint8_t newSinceFit = 0;
    int8_t pendingSigns = -1;
    uint8_t pendingSignFits = 0;
    bool enabled = false;
};
