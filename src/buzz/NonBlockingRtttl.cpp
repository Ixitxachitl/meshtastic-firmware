// NonBlockingRTTTL - ESP32-C6 compatible port
// Based on https://github.com/end2endzone/NonBlockingRTTTL
// MIT License

#include "NonBlockingRtttl.h"
#include "Arduino.h"

namespace rtttl
{

#define OCTAVE_OFFSET 0

const char *buffer = "";
const char *firstNote = "";
int bufferIndex = -32760;
byte default_dur = 4;
byte default_oct = 5;
int bpm = 63;
long wholenote;
byte pin = 255;
unsigned long noteDelay = 0;
unsigned long loopGap;
byte loopCount;
bool playing = false;

void nextnote();

void begin(byte iPin, const char *iSongBuffer, byte iLoopCount, unsigned long iLoopGap)
{
    // init values
    pin = iPin;
    buffer = iSongBuffer;
    bufferIndex = 0;
    default_dur = 4;
    default_oct = 6;
    bpm = 63;
    playing = true;
    noteDelay = 0;
    loopCount = iLoopCount;
    loopGap = iLoopGap;

    // stop current note
    noTone(pin);

    // read buffer until first note
    int num;

    // format: d=N,o=N,b=NNN:
    // find the start (skip name, etc)

    while (*buffer != ':')
        buffer++; // ignore name
    buffer++;     // skip ':'

    // get default duration
    if (*buffer == 'd') {
        buffer++;
        buffer++; // skip "d="
        num = 0;
        while (isdigit(*buffer)) {
            num = (num * 10) + (*buffer++ - '0');
        }
        if (num > 0)
            default_dur = num;
        buffer++; // skip comma
    }

    // get default octave
    if (*buffer == 'o') {
        buffer++;
        buffer++; // skip "o="
        num = *buffer++ - '0';
        if (num >= 3 && num <= 7)
            default_oct = num;
        buffer++; // skip comma
    }

    // get BPM
    if (*buffer == 'b') {
        buffer++;
        buffer++; // skip "b="
        num = 0;
        while (isdigit(*buffer)) {
            num = (num * 10) + (*buffer++ - '0');
        }
        bpm = num;
        buffer++; // skip colon
    }

    // BPM usually expresses the number of quarter notes per minute
    wholenote = (60 * 1000L / bpm) * 4; // this is the time for whole note (in milliseconds)

    firstNote = buffer;
}

void nextnote()
{
    long duration;
    byte note;
    byte scale;

    // stop current note
    noTone(pin);

    // first, get note duration, if available
    int num = 0;
    while (isdigit(*buffer)) {
        num = (num * 10) + (*buffer++ - '0');
    }

    if (num)
        duration = wholenote / num;
    else
        duration = wholenote / default_dur; // we will need to check if we are a dotted note after

    // now get the note
    note = 0;

    switch (*buffer) {
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
    }
    buffer++;

    // now, get optional '#' sharp
    if (*buffer == '#') {
        note++;
        buffer++;
    }

    // now, get optional '.' dotted note
    if (*buffer == '.') {
        duration += duration / 2;
        buffer++;
    }

    // now, get scale
    if (isdigit(*buffer)) {
        scale = *buffer - '0';
        buffer++;
    } else {
        scale = default_oct;
    }

    scale += OCTAVE_OFFSET;

    // now, get optional '.' dotted note again.
    // A dot /after/ the octave is allowed too, depending on which
    // RTTTL specification you read.
    // This is also why `default_dur` is handled before `default_oct`.
    if (*buffer == '.') {
        duration += duration / 2;
        buffer++;
    }

    if (*buffer == ',')
        buffer++; // skip comma for next note (or we may be at the end)

    // now play the note
    if (note) {
        // note formula: http://www.phy.mtu.edu/~suits/NoteFreqCalcs.html
        // in RTTL, C in the 4th octave (called note C4) is in the middle of the piano keyboard (close to middle C).
        // That corresponds to a frequency of 261.63 Hz.
        // note will be a value between 1 and 12 for C to B
        // octave will be a value between 4 and 7
        // Note that in mathematical terms, C4 should be 16.35 Hz (4 octaves below middle C)
        // but in RTTTL specification, C4 is note 261.63 Hz which is note C in the 4th octave up from the bottom of a piano.
        unsigned int frequency = (unsigned int)(261.63 * pow(2.0, (note - 1) / 12.0 + (scale - 4)));
        tone(pin, frequency, duration);
        noteDelay = millis() + duration;
    } else {
        // silence
        noTone(pin);
        noteDelay = millis() + duration;
    }
}

void play()
{
    if (!playing)
        return;

    unsigned long m = millis();
    if ((long)(m - noteDelay) >= 0) {
        if (*buffer == '\0') {
            // end of the song
            // play again or stop
            if (loopCount != 255 && loopCount > 0) {
                loopCount--;
            }
            if (loopCount == 0) {
                stop();
            } else {
                // play again
                buffer = firstNote;
                noteDelay = m + loopGap;
            }
        } else {
            nextnote();
        }
    }
}

void stop()
{
    playing = false;
    noTone(pin);
}

bool isPlaying()
{
    return playing;
}

bool done()
{
    return !playing;
}

}; // namespace rtttl
