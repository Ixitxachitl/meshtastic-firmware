#include "SamPlayback.h"

#if defined(USE_SDL_AUDIO) && defined(MESHTASTIC_ENABLE_TTS)

#include "SdlAudio.h"
#include "audio/sam/ESP8266SAM.h"
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace portduino_audio
{
namespace
{
// ESP8266SAM::OutputByte() already widens SAM's 8-bit unsigned output to signed 16-bit and
// duplicates it to both channels before calling ConsumeSample(), so sample[0] is ready to
// queue as-is - see ESP8266SAM.cpp.
class VectorSink : public SamAudioOut
{
  public:
    std::vector<int16_t> samples;
    bool begin() override { return true; }
    bool SetRate(int) override { return true; }     // fixed at 22050Hz, matching SdlAudio's device
    bool SetChannels(int) override { return true; } // mono
    bool ConsumeSample(int16_t sample[2]) override
    {
        samples.push_back(sample[0]);
        return true;
    }
};

std::atomic<bool> g_speaking{false};

void renderAndQueue(std::string text)
{
    VectorSink sink;
    ESP8266SAM sam;
    sam.Say(&sink, text.c_str());
    if (!sink.samples.empty())
        queuePcm(sink.samples.data(), sink.samples.size());
    g_speaking.store(false, std::memory_order_release);
}
} // namespace

void readAloud(const char *text)
{
    if (!text || !text[0])
        return;

    bool expected = false;
    if (!g_speaking.compare_exchange_strong(expected, true))
        return; // previous utterance still rendering

    // SAM refuses anything longer than one page outright; truncate rather than say nothing.
    char truncated[256];
    strncpy(truncated, text, sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = '\0';
    if (strlen(truncated) > 254)
        truncated[254] = '\0';

    std::thread(renderAndQueue, std::string(truncated)).detach();
}

} // namespace portduino_audio

#endif
