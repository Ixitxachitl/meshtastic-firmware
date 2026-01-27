#include "I2CBuzzer.h"

#if !MESHTASTIC_EXCLUDE_I2C

#include "detect/ScanI2CTwoWire.h"
#include "main.h"
#include <cstring>

// Global instance
I2CBuzzer *i2cBuzzer = nullptr;

bool I2CBuzzer::begin(const ScanI2C::FoundDevice &device)
{
    if (device.type != ScanI2C::DeviceType::I2C_BUZZER) {
        LOG_WARN("I2CBuzzer: Invalid device type");
        return false;
    }

    if (device.address.address == 0 || device.address.port == ScanI2C::I2CPort::NO_I2C) {
        LOG_WARN("I2CBuzzer: Invalid address or port");
        return false;
    }

    address = device.address.address;
    wire = ScanI2CTwoWire::fetchI2CBus(device.address);

    if (!wire) {
        LOG_WARN("I2CBuzzer: Could not get I2C bus");
        return false;
    }

    available = true;
    LOG_INFO("I2CBuzzer initialized at address 0x%02X", address);
    return true;
}

void I2CBuzzer::tone(uint32_t frequency, uint32_t duration)
{
    if (!available) {
        return;
    }

    if (!writeCommand(frequency, duration)) {
        LOG_WARN("I2CBuzzer: Failed to send tone command");
    }
}

void I2CBuzzer::noTone()
{
    if (!available) {
        return;
    }

    // Send zeros to stop any playing tone
    writeCommand(0, 0);
}

bool I2CBuzzer::writeCommand(uint32_t frequency, uint32_t duration)
{
    if (!wire || !available) {
        return false;
    }

    // Modulino protocol: 8 bytes
    // - Bytes 0-3: frequency (little-endian)
    // - Bytes 4-7: duration in ms (little-endian)
    uint8_t buf[8];
    memcpy(&buf[0], &frequency, 4);
    memcpy(&buf[4], &duration, 4);

    wire->beginTransmission(address);
    wire->write(buf, 8);
    uint8_t result = wire->endTransmission();

    if (result != 0) {
        LOG_DEBUG("I2CBuzzer: I2C transmission error %d", result);
        return false;
    }

    return true;
}

// ==============================================================================
// RTTTL Playback Implementation
// ==============================================================================

I2CBuzzer::I2CBuzzer()
    : wire(nullptr), address(0), available(false), rtttlBuffer(nullptr), rtttlFirstNote(nullptr), rtttlPlaying(false),
      rtttlDefaultDur(4), rtttlDefaultOct(5), rtttlBpm(63), rtttlWholenote(0), rtttlNoteEndTime(0)
{
}

void I2CBuzzer::beginRtttl(const char *rtttlSong)
{
    if (!available || !rtttlSong) {
        return;
    }

    rtttlBuffer = rtttlSong;
    rtttlPlaying = true;
    rtttlNoteEndTime = 0;
    rtttlDefaultDur = 4;
    rtttlDefaultOct = 5;
    rtttlBpm = 63;

    // Parse RTTTL header: name:d=N,o=N,b=NNN:notes

    // Skip name
    while (*rtttlBuffer && *rtttlBuffer != ':')
        rtttlBuffer++;
    if (*rtttlBuffer)
        rtttlBuffer++; // skip ':'

    // Parse default duration
    if (*rtttlBuffer == 'd') {
        rtttlBuffer += 2; // skip "d="
        int num = 0;
        while (*rtttlBuffer && isdigit(*rtttlBuffer)) {
            num = (num * 10) + (*rtttlBuffer++ - '0');
        }
        if (num > 0)
            rtttlDefaultDur = num;
        if (*rtttlBuffer == ',')
            rtttlBuffer++;
    }

    // Parse default octave
    if (*rtttlBuffer == 'o') {
        rtttlBuffer += 2; // skip "o="
        int num = *rtttlBuffer++ - '0';
        if (num >= 3 && num <= 7)
            rtttlDefaultOct = num;
        if (*rtttlBuffer == ',')
            rtttlBuffer++;
    }

    // Parse BPM
    if (*rtttlBuffer == 'b') {
        rtttlBuffer += 2; // skip "b="
        int num = 0;
        while (*rtttlBuffer && isdigit(*rtttlBuffer)) {
            num = (num * 10) + (*rtttlBuffer++ - '0');
        }
        if (num > 0)
            rtttlBpm = num;
        if (*rtttlBuffer == ':')
            rtttlBuffer++;
    }

    // Calculate whole note duration in ms
    rtttlWholenote = (60 * 1000L / rtttlBpm) * 4;
    rtttlFirstNote = rtttlBuffer;

    LOG_DEBUG("I2CBuzzer: RTTTL started, bpm=%d, wholenote=%ld", rtttlBpm, rtttlWholenote);

    // Play first note immediately
    if (rtttlBuffer && *rtttlBuffer != '\0') {
        rtttlNextNote();
    }
}

void I2CBuzzer::playRtttl()
{
    if (!rtttlPlaying || !available) {
        return;
    }

    unsigned long now = millis();

    // Wait for current note to finish
    if (now < rtttlNoteEndTime) {
        return;
    }

    // Check if song is finished
    if (!rtttlBuffer || *rtttlBuffer == '\0') {
        rtttlPlaying = false;
        return;
    }

    rtttlNextNote();
}

void I2CBuzzer::rtttlNextNote()
{
    // Parse duration
    int num = 0;
    while (*rtttlBuffer && isdigit(*rtttlBuffer)) {
        num = (num * 10) + (*rtttlBuffer++ - '0');
    }

    long duration;
    if (num > 0)
        duration = rtttlWholenote / num;
    else
        duration = rtttlWholenote / rtttlDefaultDur;

    // Parse note
    uint8_t note = 0;
    switch (*rtttlBuffer) {
    case 'c':
        note = 1;
        break;
    case 'd':
        note = 3;
        break;
    case 'e':
        note = 5;
        break;
    case 'f':
        note = 6;
        break;
    case 'g':
        note = 8;
        break;
    case 'a':
        note = 10;
        break;
    case 'b':
        note = 12;
        break;
    case 'p':
    default:
        note = 0;
        break;
    }
    if (*rtttlBuffer)
        rtttlBuffer++;

    // Check for sharp
    if (*rtttlBuffer == '#') {
        note++;
        rtttlBuffer++;
    }

    // Check for dotted note (before octave)
    if (*rtttlBuffer == '.') {
        duration += duration / 2;
        rtttlBuffer++;
    }

    // Parse octave
    uint8_t scale = rtttlDefaultOct;
    if (*rtttlBuffer && isdigit(*rtttlBuffer)) {
        scale = *rtttlBuffer++ - '0';
    }

    // Check for dotted note (after octave)
    if (*rtttlBuffer == '.') {
        duration += duration / 2;
        rtttlBuffer++;
    }

    // Skip comma
    if (*rtttlBuffer == ',')
        rtttlBuffer++;

    // Calculate frequency and play
    if (note > 0) {
        // RTTTL note formula
        unsigned int frequency = (unsigned int)(261.63 * pow(2.0, (note - 1) / 12.0 + (scale - 4)));
        tone(frequency, duration);
    }
    // For rests (note == 0), we just wait without playing

    // Schedule next note after this one finishes
    rtttlNoteEndTime = millis() + duration;
}

void I2CBuzzer::stopRtttl()
{
    rtttlPlaying = false;
    noTone();
}

#endif // !MESHTASTIC_EXCLUDE_I2C
