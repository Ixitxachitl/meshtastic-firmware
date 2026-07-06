/*
  AudioGeneratorRTTTL3
  Local copy of AudioGeneratorRTTTL extended to support octaves 3-7.

  Changes vs upstream:
  - Class renamed AudioGeneratorRTTTL3
  - notes[] table extended to include octave 3 (prepended)
  - Index formula changed from (scale-4)*12 to (scale-3)*12
  - Clamp changed from scale>=4 to scale>=3

  Based on AudioGeneratorRTTTL by Earle F. Philhower, III (GPL v3+).
*/

#include "AudioGeneratorRTTTL3.h"
#include <stdlib.h>

AudioGeneratorRTTTL3::AudioGeneratorRTTTL3()
{
    running = false;
    file = NULL;
    output = NULL;
    rate = 22050;
    buff = nullptr;
    ptr = 0;
}

AudioGeneratorRTTTL3::~AudioGeneratorRTTTL3()
{
    free(buff);
}

bool AudioGeneratorRTTTL3::stop()
{
    if (!file || !output)
        return false;
    running = false;
    output->stop();
    return file->close();
}

bool AudioGeneratorRTTTL3::isRunning()
{
    return running;
}

bool AudioGeneratorRTTTL3::loop()
{
    if (!running)
        goto done;

    if (samplesSent == ttlSamples) {
        if (!GetNextNote()) {
            running = false;
            goto done;
        }
        samplesSent = 0;
    }

    if (ttlSamplesPerWaveFP10 == 0) {
        int16_t mute[2] = {0, 0};
        while ((samplesSent < ttlSamples) && output->ConsumeSample(mute))
            samplesSent++;
    } else {
        while (samplesSent < ttlSamples) {
            int samplesSentFP10 = samplesSent << 10;
            int rem = samplesSentFP10 % ttlSamplesPerWaveFP10;
            int16_t val = (rem > ttlSamplesPerWaveFP10 / 2) ? 8192 : -8192;
            int16_t s[2] = {val, val};
            if (!output->ConsumeSample(s))
                goto done;
            samplesSent++;
        }
    }

done:
    file->loop();
    output->loop();
    return running;
}

bool AudioGeneratorRTTTL3::SkipWhitespace()
{
    while ((ptr < len) && (buff[ptr] == ' '))
        ptr++;
    return ptr < len;
}

bool AudioGeneratorRTTTL3::ReadInt(int *dest)
{
    if (ptr >= len)
        return false;
    SkipWhitespace();
    if (ptr >= len)
        return false;
    if ((buff[ptr] < '0') || (buff[ptr] > '9'))
        return false;
    int t = 0;
    while ((buff[ptr] >= '0') && (buff[ptr] <= '9'))
        t = (t * 10) + (buff[ptr++] - '0');
    *dest = t;
    return true;
}

bool AudioGeneratorRTTTL3::ParseHeader()
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
static int notes3[61] = {
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

bool AudioGeneratorRTTTL3::GetNextNote()
{
    int dur, note, scale;
    if (ptr >= len)
        return false;

    if (!ReadInt(&dur))
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
    if (scale < 3)
        scale = 3;
    if (scale > 7)
        scale = 7;

    if (note) {
        int freq = notes3[(scale - 3) * 12 + note];
        ttlSamplesPerWaveFP10 = (rate << 10) / freq;
    } else {
        ttlSamplesPerWaveFP10 = 0;
    }
    ttlSamples = (rate * dur) / 1000;
    return true;
}

bool AudioGeneratorRTTTL3::begin(AudioFileSource *source, AudioOutput *output)
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
    buff = (char *)malloc(len);
    if (!buff)
        return false;
    if (file->read(buff, len) != (uint32_t)len)
        return false;

    ptr = 0;
    samplesSent = 0;
    ttlSamples = 0;

    if (!ParseHeader())
        return false;

    if (!output->SetRate(rate))
        return false;
    if (!output->SetBitsPerSample(16))
        return false;
    if (!output->SetChannels(2))
        return false;
    if (!output->begin())
        return false;

    running = true;
    return true;
}
