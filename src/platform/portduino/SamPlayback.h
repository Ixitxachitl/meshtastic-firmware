#pragma once
// Native/portduino speech. Unlike the ESP32 I2S path (AudioThread/SamPcm), there is no DMA
// deadline to feed: a spoken message is short, so this renders the whole utterance into one
// buffer on a background thread and hands it to SdlAudio's queue in a single shot, rather
// than porting the ring-buffer/FreeRTOS-task machinery that exists to bound memory on
// boards without PSRAM.
#if defined(USE_SDL_AUDIO) && defined(MESHTASTIC_ENABLE_TTS)
namespace portduino_audio
{
/// Speak `text` with SAM on a background thread; returns immediately. Ignored (not queued)
/// if a previous utterance is still rendering - SAM keeps its synthesis state in file-scope
/// globals, so only one render may run at a time.
void readAloud(const char *text);
} // namespace portduino_audio
#endif
