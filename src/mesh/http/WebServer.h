#pragma once

#include "PhoneAPI.h"
#include "concurrency/OSThread.h"
#include <Arduino.h>
#include <functional>

#if !MESHTASTIC_EXCLUDE_WEBSERVER

void initWebServer();
void createSSLCert();

class WebServerThread : private concurrency::OSThread
{
  private:
    uint32_t lastActivityTime = 0;
    uint32_t lastTransferTime = 0;

  public:
    WebServerThread();
    uint32_t requestRestart = 0;
    void markActivity();
    // An SD upload or delete is under way: poll almost continuously so each request is answered as it lands.
    void markTransfer();

  protected:
    virtual int32_t runOnce() override;
    int32_t getAdaptiveInterval();
};

extern WebServerThread *webServerThread;

#else
// Stub implementations when web server is excluded
inline void initWebServer() {}
inline void createSSLCert() {}

class WebServerThread
{
  public:
    WebServerThread() {}
    uint32_t requestRestart = 0;
    void markActivity() {}
    void markTransfer() {}
};

inline WebServerThread *webServerThread = nullptr;

#endif
