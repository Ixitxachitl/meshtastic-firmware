/*
  AudioGeneratorRTTTL
  Audio output generator that plays RTTTL (Nokia ringtone)

  Based on the Rtttl Arduino library by James BM, https://github.com/spicajames/Rtttl
  Based on the gist from Daniel Hall https://gist.github.com/smarthall/1618800

  Copyright (C) 2018  Earle F. Philhower, III

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  See AudioGeneratorRTTTL.h for the list of Meshtastic modifications.
*/

#include "configuration.h"

// ESP8266Audio, which provides the AudioGenerator base class, is only pulled in
// by boards that define HAS_I2S. Everything else - nRF52, RP2040, the native
// test build - must not try to compile this.
#ifdef HAS_I2S

#include "AudioGeneratorRTTTL.h"
#include <stdlib.h>

namespace meshtastic
{

AudioGeneratorRTTTL::AudioGeneratorRTTTL()
{
    running = false;
    file = NULL;
    output = NULL;
    rate = 22050;
    buff = nullptr;
    len = 0;
    ptr = 0;
    defaultDuration = 4;
    defaultOctave = 6;
    wholeNoteMS = 0;
    ttlSamplesPerWaveFP10 = 0;
    phaseFP10 = 0;
    ttlSamples = 0;
    samplesSent = 0;
}

AudioGeneratorRTTTL::~AudioGeneratorRTTTL()
{
    free(buff);
}

bool AudioGeneratorRTTTL::stop()
{
    if (!file || !output)
        return false;
    running = false;
    output->stop();
    return file->close();
}

bool AudioGeneratorRTTTL::isRunning()
{
    return running;
}

bool AudioGeneratorRTTTL::FillCurrentNote()
{
    if (ttlSamplesPerWaveFP10 == 0) {
        int16_t mute[2] = {0, 0};
        while (samplesSent < ttlSamples) {
            if (!output->ConsumeSample(mute))
                return false;
            samplesSent++;
        }
        return true;
    }

    while (samplesSent < ttlSamples) {
        int16_t val = (phaseFP10 > ttlSamplesPerWaveFP10 / 2) ? 8192 : -8192;
        int16_t s[2] = {val, val};
        if (!output->ConsumeSample(s))
            return false;
        samplesSent++;
        // Advance the phase by one sample, wrapped, so this cannot overflow
        // however long the note runs.
        phaseFP10 += 1 << 10;
        if (phaseFP10 >= ttlSamplesPerWaveFP10)
            phaseFP10 -= ttlSamplesPerWaveFP10;
    }
    return true;
}

bool AudioGeneratorRTTTL::loop()
{
    if (!file || !output)
        return false;

    // Keep handing notes to the output until it refuses a sample, so a single
    // call fills the whole DMA chain instead of stopping at the next note.
    while (running) {
        if (samplesSent == ttlSamples) {
            if (!GetNextNote()) {
                running = false;
                break;
            }
            samplesSent = 0;
        }
        if (!FillCurrentNote())
            break; // output is full; pick up here on the next call
    }

    file->loop();
    output->loop();
    return running;
}

bool AudioGeneratorRTTTL::SkipWhitespace()
{
    while ((ptr < len) && (buff[ptr] == ' '))
        ptr++;
    return ptr < len;
}

bool AudioGeneratorRTTTL::ReadInt(int *dest)
{
    if (ptr >= len)
        return false;
    SkipWhitespace();
    if (ptr >= len)
        return false;
    if ((buff[ptr] < '0') || (buff[ptr] > '9'))
        return false;
    int t = 0;
    while ((ptr < len) && (buff[ptr] >= '0') && (buff[ptr] <= '9'))
        t = (t * 10) + (buff[ptr++] - '0');
    *dest = t;
    return true;
}

bool AudioGeneratorRTTTL::ParseHeader()
{
    while ((ptr < len) && (buff[ptr] != ':'))
        ptr++;
    if (ptr >= len)
        return false;
    if (buff[ptr++] != ':')
        return false;
    if (!SkipWhitespace())
        return false;
    if ((buff[ptr] != 'd') && (buff[ptr] != 'D'))
        return false;
    ptr++;
    if (!SkipWhitespace())
        return false;
    if (buff[ptr++] != '=')
        return false;
    if (!ReadInt(&defaultDuration))
        return false;
    if (defaultDuration <= 0)
        return false; // would divide by zero in GetNextNote()
    if (!SkipWhitespace())
        return false;
    if (buff[ptr++] != ',')
        return false;

    if (!SkipWhitespace())
        return false;
    if ((buff[ptr] != 'o') && (buff[ptr] != 'O'))
        return false;
    ptr++;
    if (!SkipWhitespace())
        return false;
    if (buff[ptr++] != '=')
        return false;
    if (!ReadInt(&defaultOctave))
        return false;
    if (!SkipWhitespace())
        return false;
    if (buff[ptr++] != ',')
        return false;

    int bpm;
    if (!SkipWhitespace())
        return false;
    if ((buff[ptr] != 'b') && (buff[ptr] != 'B'))
        return false;
    ptr++;
    if (!SkipWhitespace())
        return false;
    if (buff[ptr++] != '=')
        return false;
    if (!ReadInt(&bpm))
        return false;
    if (bpm <= 0)
        return false; // would divide by zero below
    if (!SkipWhitespace())
        return false;
    if (buff[ptr++] != ':')
        return false;

    wholeNoteMS = (60 * 1000 * 4) / bpm;
    return true;
}

// Notes table covering octaves 3-7 (5 octaves * 12 notes + 1 rest = 61 entries).
// Index formula: notes[(scale - 3) * 12 + note]  where note=1..12, scale=3..7.
// note index: 1=C 2=C# 3=D 4=D# 5=E 6=F 7=F# 8=G 9=G# 10=A 11=A# 12=B
static const int notes[61] = {
    0, // rest
    // octave 3
    131,
    139,
    147,
    156,
    165,
    175,
    185,
    196,
    208,
    220,
    233,
    247,
    // octave 4
    262,
    277,
    294,
    311,
    330,
    349,
    370,
    392,
    415,
    440,
    466,
    494,
    // octave 5
    523,
    554,
    587,
    622,
    659,
    698,
    740,
    784,
    831,
    880,
    932,
    988,
    // octave 6
    1047,
    1109,
    1175,
    1245,
    1319,
    1397,
    1480,
    1568,
    1661,
    1760,
    1865,
    1976,
    // octave 7
    2093,
    2217,
    2349,
    2489,
    2637,
    2794,
    2960,
    3136,
    3322,
    3520,
    3729,
    3951,
};

static const int kLowestOctave = 3;
static const int kHighestOctave = 7;

bool AudioGeneratorRTTTL::GetNextNote()
{
    int dur, note, scale;
    if (ptr >= len)
        return false;

    if (!ReadInt(&dur) || (dur <= 0))
        dur = defaultDuration;
    dur = wholeNoteMS / dur;

    if (ptr >= len)
        return false;
    note = 0;
    switch (buff[ptr++]) {
    case 'c':
    case 'C':
        note = 1;
        break;
    case 'd':
    case 'D':
        note = 3;
        break;
    case 'e':
    case 'E':
        note = 5;
        break;
    case 'f':
    case 'F':
        note = 6;
        break;
    case 'g':
    case 'G':
        note = 8;
        break;
    case 'a':
    case 'A':
        note = 10;
        break;
    case 'b':
    case 'B':
        note = 12;
        break;
    case 'p':
    case 'P':
        note = 0;
        break;
    default:
        return false;
    }
    if ((ptr < len) && (buff[ptr] == '#')) {
        ptr++;
        // "b#" and "e#" are not legal RTTTL, but a malformed string can still
        // contain them, and note==13 would index one past the table.
        if (note < 12)
            note++;
    }
    if (!ReadInt(&scale))
        scale = defaultOctave;
    if ((ptr < len) && (buff[ptr] == '.')) {
        ptr++;
        dur += dur / 2;
    }
    SkipWhitespace();
    if ((ptr < len) && (buff[ptr] == ','))
        ptr++;

    // Clamp to the supported range (octaves 3-7).
    if (scale < kLowestOctave)
        scale = kLowestOctave;
    if (scale > kHighestOctave)
        scale = kHighestOctave;

    if (note) {
        int freq = notes[(scale - kLowestOctave) * 12 + note];
        ttlSamplesPerWaveFP10 = (rate << 10) / freq;
    } else {
        ttlSamplesPerWaveFP10 = 0;
    }
    phaseFP10 = 0;
    ttlSamples = (rate * dur) / 1000;
    return true;
}

bool AudioGeneratorRTTTL::begin(AudioFileSource *source, AudioOutput *output)
{
    if (!source)
        return false;
    file = source;
    if (!output)
        return false;
    this->output = output;
    if (!file->isOpen())
        return false;

    len = file->getSize();
    if (len <= 0)
        return false;
    // begin() on an instance that already parsed something would otherwise leak
    // the previous copy.
    free(buff);
    buff = (char *)malloc(len);
    if (!buff)
        return false;
    if (file->read(buff, len) != (uint32_t)len)
        return false;

    ptr = 0;
    samplesSent = 0;
    ttlSamples = 0;
    phaseFP10 = 0;

    if (!ParseHeader())
        return false;

    if (!output->SetRate(rate))
        return false;
    // No SetBitsPerSample() here: ESP8266Audio 2.4.x dropped it (16-bit is assumed),
    // and on the 2.0.0 fork the variants pin, AudioOutputI2S already defaults bps to
    // 16 in its constructor - so the call was redundant there and breaks the build on 2.4.x.
    if (!output->SetChannels(2))
        return false;
    if (!output->begin())
        return false;

    running = true;
    return true;
}

} // namespace meshtastic

#endif // HAS_I2S
