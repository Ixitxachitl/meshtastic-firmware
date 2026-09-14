#include "MagHardIronTracker.h"

#include <cmath>

namespace
{
constexpr uint8_t kMinSamples = 12;
constexpr uint8_t kNewSamplesPerFit = 4;
constexpr uint32_t kSampleMaxAgeMs = 15UL * 60UL * 1000UL;
constexpr float kMinSeparation = 0.15f;  // of the radius: clear of sensor noise, yet a level turn still fills the buffer
constexpr float kPriorWeight = 0.01f;    // per sample; only steers directions the samples leave unconstrained
constexpr float kMaxRmsFraction = 0.15f; // of the radius
constexpr float kMaxRmsFloor = 0.05f;    // G
constexpr float kMirrorMinOffset = 0.3f; // of the radius; below this the dip is too shallow to pick a side
constexpr float kMinGravity = 0.85f;     // g; outside this the device is accelerating and "down" is unreliable
constexpr float kMaxGravity = 1.15f;
constexpr float kSignVarianceRatio = 4.0f;    // a sign change must leave the dip this many times steadier
constexpr float kSignVarianceMargin = 0.012f; // and steadier by at least this, as a fraction of radius squared
constexpr uint8_t kSignConfirmFits = 2;       // consecutive fits that must agree before the signs change
constexpr int kIterations = 10;

float dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

int flippedAxes(int signs)
{
    return (signs & 1) + ((signs >> 1) & 1) + ((signs >> 2) & 1);
}

bool solve3(const float a[3][3], const float b[3], float x[3])
{
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!std::isfinite(det) || std::fabs(det) < 1e-9f)
        return false;
    for (int col = 0; col < 3; ++col) {
        float m[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m[r][c] = (c == col) ? b[r] : a[r][c];
        x[col] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) /
                 det;
    }
    return true;
}
} // namespace

void MagHardIronTracker::reset(float centreX, float centreY, float centreZ, float fieldRadius)
{
    for (Sample &s : samples)
        s.used = false;
    count = 0;
    newSinceFit = 0;
    pendingSigns = -1;
    pendingSignFits = 0;
    centre[0] = centreX;
    centre[1] = centreY;
    centre[2] = centreZ;
    radius = fieldRadius;
    enabled =
        std::isfinite(centreX) && std::isfinite(centreY) && std::isfinite(centreZ) && fieldRadius >= 0.1f && fieldRadius <= 2.0f;
}

bool MagHardIronTracker::addSample(float magX, float magY, float magZ, float accelX, float accelY, float accelZ, uint32_t nowMs,
                                   int8_t hemisphere)
{
    if (!enabled)
        return false;
    const float gravity = std::sqrt(accelX * accelX + accelY * accelY + accelZ * accelZ);
    if (!(gravity > kMinGravity && gravity < kMaxGravity))
        return false;

    // Only samples that add a new point on the sphere are kept, so a device at rest never skews the fit.
    const float field[3] = {magX, magY, magZ};
    const float minSeparation = kMinSeparation * radius;
    int freeSlot = -1;
    int oldestSlot = 0;
    uint32_t oldestAgeMs = 0;
    for (int i = 0; i < kSlots; ++i) {
        Sample &s = samples[i];
        if (s.used && nowMs - s.atMs > kSampleMaxAgeMs) {
            s.used = false;
            --count;
        }
        if (!s.used) {
            if (freeSlot < 0)
                freeSlot = i;
            continue;
        }
        const float d[3] = {field[0] - s.field[0], field[1] - s.field[1], field[2] - s.field[2]};
        if (dot3(d, d) < minSeparation * minSeparation)
            return false;
        const uint32_t ageMs = nowMs - s.atMs;
        if (ageMs >= oldestAgeMs) {
            oldestAgeMs = ageMs;
            oldestSlot = i;
        }
    }

    Sample &slot = samples[freeSlot >= 0 ? freeSlot : oldestSlot];
    if (!slot.used)
        ++count;
    for (int i = 0; i < 3; ++i)
        slot.field[i] = field[i];
    // Accelerometers read +1 g upward at rest, so down is the opposite of the sample.
    slot.down[0] = -accelX / gravity;
    slot.down[1] = -accelY / gravity;
    slot.down[2] = -accelZ / gravity;
    slot.atMs = nowMs;
    slot.used = true;

    if (++newSinceFit < kNewSamplesPerFit || count < kMinSamples)
        return false;
    newSinceFit = 0;
    return fit(hemisphere);
}

// Gauss-Newton on the distance of each sample from the centre, held at the calibrated radius, with a light pull
// towards `prior` so directions the samples don't pin down stay where they were.
bool MagHardIronTracker::refine(float c[3], const float prior[3], float &rms) const
{
    const float lambda = kPriorWeight * count;
    for (int it = 0; it < kIterations; ++it) {
        float a[3][3] = {{lambda, 0.0f, 0.0f}, {0.0f, lambda, 0.0f}, {0.0f, 0.0f, lambda}};
        float b[3] = {-lambda * (c[0] - prior[0]), -lambda * (c[1] - prior[1]), -lambda * (c[2] - prior[2])};
        for (const Sample &s : samples) {
            if (!s.used)
                continue;
            const float d[3] = {s.field[0] - c[0], s.field[1] - c[1], s.field[2] - c[2]};
            const float len = std::sqrt(dot3(d, d));
            if (len < 1e-3f)
                continue;
            const float u[3] = {d[0] / len, d[1] / len, d[2] / len};
            const float residual = len - radius;
            for (int r = 0; r < 3; ++r) {
                b[r] += u[r] * residual;
                for (int k = 0; k < 3; ++k)
                    a[r][k] += u[r] * u[k];
            }
        }
        float step[3];
        if (!solve3(a, b, step))
            return false;
        for (int i = 0; i < 3; ++i)
            c[i] += step[i];
        if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]))
            return false;
        if (dot3(step, step) < 1e-8f)
            break;
    }

    float sum = 0.0f;
    for (const Sample &s : samples) {
        if (!s.used)
            continue;
        const float d[3] = {s.field[0] - c[0], s.field[1] - c[1], s.field[2] - c[2]};
        const float residual = std::sqrt(dot3(d, d)) - radius;
        sum += residual * residual;
    }
    rms = std::sqrt(sum / count);
    return true;
}

bool MagHardIronTracker::fit(int8_t hemisphere)
{
    const float maxRms = std::fmax(kMaxRmsFloor, kMaxRmsFraction * radius);
    float c[3] = {centre[0], centre[1], centre[2]};
    float rms = 0.0f;
    if (!refine(c, centre, rms))
        return false;

    // Coming from far away, the fit can settle on the wrong side of the samples' cap; try the far side of it too.
    if (rms > maxRms) {
        float far[3] = {0.0f, 0.0f, 0.0f};
        for (const Sample &s : samples)
            if (s.used)
                for (int i = 0; i < 3; ++i)
                    far[i] += s.field[i];
        for (int i = 0; i < 3; ++i)
            far[i] = 2.0f * far[i] / count - c[i];
        float farRms = 0.0f;
        if (refine(far, far, farRms) && farRms < rms) {
            for (int i = 0; i < 3; ++i)
                c[i] = far[i];
            rms = farRms;
        }
    }

    // Turned only while level, the samples ring one axis and fit equally well with the centre mirrored across the
    // ring. The field dips downward in the north and upward in the south; with no position, assume north.
    float along = 0.0f;
    float axis[3] = {0.0f, 0.0f, 0.0f};
    for (const Sample &s : samples) {
        if (!s.used)
            continue;
        for (int i = 0; i < 3; ++i) {
            along += axisSign[i] * (s.field[i] - c[i]) * s.down[i];
            axis[i] += axisSign[i] * s.down[i];
        }
    }
    along /= count;
    const float axisLen = std::sqrt(dot3(axis, axis));
    const bool wrongSide = (hemisphere < 0) ? (along > 0.0f) : (along < 0.0f);
    if (wrongSide && axisLen > 1e-3f && std::fabs(along) > kMirrorMinOffset * radius) {
        float mirrored[3];
        for (int i = 0; i < 3; ++i)
            mirrored[i] = c[i] + 2.0f * along * axis[i] / axisLen;
        float mirroredRms = 0.0f;
        if (refine(mirrored, mirrored, mirroredRms) && mirroredRms <= rms * 1.5f + 0.02f) {
            for (int i = 0; i < 3; ++i)
                c[i] = mirrored[i];
            rms = mirroredRms;
        }
    }

    if (rms > maxRms)
        return false;
    for (int i = 0; i < 3; ++i)
        centre[i] = c[i];
    fitRms = rms;
    ++fits;
    checkAxisSigns(hemisphere);
    return true;
}

// With the right signs the field's component along gravity is the same for every sample. Samples taken only while
// level can't separate X from Y, so they never clear the margins and the signs stay put.
void MagHardIronTracker::checkAxisSigns(int8_t hemisphere)
{
    float mean[8] = {};
    float squares[8] = {};
    for (const Sample &s : samples) {
        if (!s.used)
            continue;
        const float d[3] = {s.field[0] - centre[0], s.field[1] - centre[1], s.field[2] - centre[2]};
        for (int signs = 0; signs < 8; ++signs) {
            float vertical = 0.0f;
            for (int k = 0; k < 3; ++k)
                vertical += (((signs >> k) & 1) ? -d[k] : d[k]) * s.down[k];
            mean[signs] += vertical;
            squares[signs] += vertical * vertical;
        }
    }

    int current = 0;
    for (int k = 0; k < 3; ++k)
        if (axisSign[k] < 0.0f)
            current |= 1 << k;
    float variance[8];
    int best = -1;
    for (int signs = 0; signs < 8; ++signs) {
        mean[signs] /= count;
        variance[signs] = squares[signs] / count - mean[signs] * mean[signs];
        // Flipping every axis leaves the spread unchanged. The hemisphere tells the two apart; without one, keep
        // whichever changes fewer axes, so an unknown position never turns the heading round.
        if (hemisphere != 0) {
            if (hemisphere > 0 ? (mean[signs] <= kMirrorMinOffset * radius) : (mean[signs] >= -kMirrorMinOffset * radius))
                continue;
        } else {
            const int changed = flippedAxes(signs ^ current), changedIfOpposite = flippedAxes(signs ^ 7 ^ current);
            if (changed > changedIfOpposite || (changed == changedIfOpposite && signs > (signs ^ 7)))
                continue;
        }
        if (best < 0 || variance[signs] < variance[best])
            best = signs;
    }

    if (best < 0 || best == current || variance[current] < kSignVarianceRatio * variance[best] ||
        variance[current] - variance[best] < kSignVarianceMargin * radius * radius) {
        pendingSigns = -1;
        pendingSignFits = 0;
        return;
    }
    if (best != pendingSigns) {
        pendingSigns = static_cast<int8_t>(best);
        pendingSignFits = 1;
    } else {
        ++pendingSignFits;
    }
    if (pendingSignFits < kSignConfirmFits)
        return;
    for (int k = 0; k < 3; ++k)
        axisSign[k] = ((best >> k) & 1) ? -1.0f : 1.0f;
    ++signChanges;
    pendingSigns = -1;
    pendingSignFits = 0;
}
