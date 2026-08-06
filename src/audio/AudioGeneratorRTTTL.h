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

  ---------------------------------------------------------------------------

  Meshtastic modifications to the ESP8266Audio original:
    - Lives in namespace meshtastic so it can coexist with the library class of
      the same name, which is still linked in as part of ESP8266Audio.
    - Note table extended down to octave 3 (was 4-7), so buzzer melodies using
      octave-3 notes play at their real pitch instead of an octave high.
    - loop() drains across note boundaries instead of returning after one note,
      so a single call fills the whole I2S DMA chain.
    - Hardened against malformed RTTTL, which reaches us from user config: a
      zero tempo or duration no longer divides by zero, and a trailing '#' can
      no longer push the note index past the end of the table.
    - Square wave phase is accumulated modulo the wave period rather than
      derived from `samplesSent << 10`, which overflows a signed int about 95 s
      into a single note at 22050 Hz.
*/

#pragma once

#include <AudioGenerator.h>
#include <stdint.h>

namespace meshtastic
{

class AudioGeneratorRTTTL : public ::AudioGenerator
{
  public:
    AudioGeneratorRTTTL();
    virtual ~AudioGeneratorRTTTL() override;
    virtual bool begin(AudioFileSource *source, AudioOutput *output) override;
    virtual bool loop() override;
    virtual bool stop() override;
    virtual bool isRunning() override;
    void SetRate(uint16_t hz) { rate = hz; }

  private:
    bool SkipWhitespace();
    bool ReadInt(int *dest);
    bool ParseHeader();
    bool GetNextNote();
    /// Push as much of the current note as the output will accept.
    /// Returns false if the output filled up before the note ended.
    bool FillCurrentNote();

  protected:
    uint16_t rate;

    char *buff;
    int len;
    int ptr;

    int defaultDuration;
    int defaultOctave;
    int wholeNoteMS;

    int ttlSamplesPerWaveFP10;
    int phaseFP10;
    int ttlSamples;
    int samplesSent;
};

} // namespace meshtastic
