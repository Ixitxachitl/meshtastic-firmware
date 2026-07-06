/*
  AudioGeneratorRTTTL3
  Local copy of AudioGeneratorRTTTL extended to support octaves 3-7.
  The upstream library clamps scale to [4,7]; this copy extends the notes
  table down to octave 3 so buzzer melodies with octave-3 notes play at
  their correct pitch instead of being transposed up one octave.

  Based on AudioGeneratorRTTTL by Earle F. Philhower, III (GPL v3+).
*/

#ifndef _AUDIOGENERATORRTTTL3_H
#define _AUDIOGENERATORRTTTL3_H

#include <AudioGenerator.h>

class AudioGeneratorRTTTL3 : public AudioGenerator
{
  public:
    AudioGeneratorRTTTL3();
    virtual ~AudioGeneratorRTTTL3() override;
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

  protected:
    uint16_t rate;

    char *buff;
    int len;
    int ptr;

    int defaultDuration;
    int defaultOctave;
    int wholeNoteMS;

    int ttlSamplesPerWaveFP10;
    int ttlSamples;
    int samplesSent;
};

#endif
