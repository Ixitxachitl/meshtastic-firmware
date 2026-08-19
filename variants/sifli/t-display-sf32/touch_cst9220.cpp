#include "touch_cst9220.h"
#include "configuration.h"
#include "variant.h"
#include <Wire.h>

// The register layout and the report format below follow SensorLib's
// TouchDrvCST92xx (MIT, Lewis He), which is the only public description of
// this part's protocol; only the parts this board needs are implemented.

namespace
{
constexpr uint8_t CST9220_ADDR = TOUCH_I2C_ADDR;

// Report register, big-endian on the wire, and the acknowledge byte written
// back to release the report.
constexpr uint16_t REG_READ = 0xD000;
constexpr uint8_t ACK = 0xAB;

constexpr uint8_t MAX_POINTS = 2;
constexpr size_t REPORT_LEN = MAX_POINTS * 5 + 5;

// Report byte 5 counts points; a point's event field reads 0x06 while down.
constexpr uint8_t EVENT_DOWN = 0x06;
} // namespace

void touchInit()
{
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(50);

    pinMode(TOUCH_INT, INPUT_PULLUP);

    Wire.beginTransmission(CST9220_ADDR);
    if (Wire.endTransmission() != 0) {
        LOG_WARN("CST9220 not responding at 0x%02x", CST9220_ADDR);
        return;
    }
    LOG_INFO("CST9220 touch ready");
}

bool readTouch(int16_t *x, int16_t *y)
{
    uint8_t report[REPORT_LEN] = {0};

    // The report has to be fetched with a repeated start. The Wire shim only
    // takes that path when the write is still buffered, so the address bytes
    // are left unsent and requestFrom() issues the combined transfer.
    Wire.beginTransmission(CST9220_ADDR);
    Wire.write((uint8_t)(REG_READ >> 8));
    Wire.write((uint8_t)(REG_READ & 0xFF));
    if (Wire.requestFrom((uint8_t)CST9220_ADDR, (uint8_t)sizeof(report)) != sizeof(report))
        return false;
    for (size_t i = 0; i < sizeof(report); i++) {
        report[i] = (uint8_t)Wire.read();
    }

    // Acknowledge the report so the controller can produce the next one. Done
    // before parsing so a malformed report still releases the controller.
    Wire.beginTransmission(CST9220_ADDR);
    Wire.write((uint8_t)(REG_READ >> 8));
    Wire.write((uint8_t)(REG_READ & 0xFF));
    Wire.write(ACK);
    Wire.endTransmission();

    if (report[0] == ACK || report[0] == 0x00 || report[6] != ACK)
        return false;

    // Bit 7 of byte 4 is the cover-screen gesture, not a coordinate.
    if ((report[4] & 0x80) != 0)
        return false;

    const uint8_t points = report[5] & 0x7F;
    if (points == 0 || points > MAX_POINTS)
        return false;

    // Only the first point matters here; the second one starts two bytes later
    // than a flat stride would put it.
    const uint8_t *p = report;
    if ((p[0] & 0x0F) != EVENT_DOWN)
        return false;

    *x = (int16_t)((p[1] << 4) | (p[3] >> 4));
    *y = (int16_t)((p[2] << 4) | (p[3] & 0x0F));
    return true;
}
