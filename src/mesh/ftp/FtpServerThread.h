#pragma once

#include "configuration.h"

// FTP access to the SD card, for bulk transfers an ordinary client (FileZilla, lftp) can drive. Opt-in per variant
// with FTP_SERVER=1; the env must also carry the MultiFTPServer lib_dep and -D DEFAULT_STORAGE_TYPE_ESP32=STORAGE_SD.
//
// Needs the plain Arduino SD object setupSDCard() mounts, the same one the web server's /sd routes use - so the
// SD_MMC and soft-SPI cards are out, exactly as in ContentHandler.cpp. Never in access-control builds: FTP has no
// way to tell an authorised client, the same reason the web flash browser is excluded there.
#if defined(FTP_SERVER) && FTP_SERVER && defined(ARCH_ESP32) && defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) &&          \
    !defined(HAS_SD_MMC) && !defined(MESHTASTIC_PHONEAPI_ACCESS_CONTROL) && __has_include(<MultiFTPServer.h>)
#define HAS_FTP_SERVER 1
#else
#define HAS_FTP_SERVER 0
#endif

#if HAS_FTP_SERVER

#include "concurrency/OSThread.h"

class FtpServer;

namespace ftp
{

// Polls the FTP server while WiFi is up. Deliberately does not touch WiFi itself - unlike device-ui's UiFtpServer,
// which owns the radio because it runs without the firmware's WiFi, here WiFiAPClient already owns it.
class FtpServerThread : private concurrency::OSThread
{
  public:
    FtpServerThread();

  protected:
    virtual int32_t runOnce() override;

  private:
    bool ensureStarted();
    void stop();

    FtpServer *server = nullptr;
};

extern FtpServerThread *ftpServerThread;

void initFtpServer();

} // namespace ftp

#else

namespace ftp
{
inline void initFtpServer() {}
} // namespace ftp

#endif
