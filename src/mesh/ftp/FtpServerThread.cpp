#include "mesh/ftp/FtpServerThread.h"

#if HAS_FTP_SERVER

#include "FSCommon.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "mesh/Throttle.h"
#include <MultiFTPServer.h>
#include <SD.h>
#include <WiFi.h>

// Cleartext protocol on the local network - these gate casual access, they are not a security boundary.
#ifndef FTP_SERVER_USERNAME
#define FTP_SERVER_USERNAME "meshtastic"
#endif
#ifndef FTP_SERVER_PASSWORD
#define FTP_SERVER_PASSWORD "meshtastic"
#endif

namespace ftp
{

namespace
{
// With nobody connected the only work is accepting a socket, so polling stays slow; the idle figure is then the
// worst-case delay before a client is accepted.
constexpr int32_t kIdleIntervalMs = 250;
constexpr int32_t kActiveIntervalMs = 10;

// handleFTP() advances each session by a single character (FtpServer::readChar reads one), so a command line needs
// a run of calls. One per poll starves the library's 10s FTP_AUTH_TIME_OUT before USER/PASS ever complete.
constexpr uint32_t kDrainBudgetActiveMs = 10;
constexpr uint32_t kDrainBudgetIdleMs = 2;
constexpr int kDrainMaxCalls = 256;

// Counted, not a flag: a client such as FileZilla opens a second control connection, and one of them closing
// must not drop polling back to idle while the other is still transferring.
int openSessions = 0;

void onConnection(FtpOperation operation, uint32_t freeSpace, uint32_t totalSpace)
{
    (void)freeSpace;
    (void)totalSpace;
    if (operation == FTP_CONNECT)
        openSessions++;
    else if (operation == FTP_DISCONNECT && openSessions > 0)
        openSessions--;
}

// WiFi only: isWifiAvailable() means "configured", not "connected", and an Ethernet board has no
// WiFi.status() to read. A variant wanting FTP over Ethernet needs its own check here.
bool wifiConnected()
{
    return config.network.wifi_enabled && WiFi.status() == WL_CONNECTED;
}
} // namespace

FtpServerThread *ftpServerThread;

FtpServerThread::FtpServerThread() : concurrency::OSThread("FtpServer") {}

bool FtpServerThread::ensureStarted()
{
    if (server)
        return true;

    {
        // setupSDCard() mounts the card once at boot; with none present there is nothing to serve.
        concurrency::LockGuard g(spiLock);
        if (SD.cardType() == CARD_NONE)
            return false;
    }

    server = new FtpServer();
    server->setCallback(onConnection);
    server->begin(FTP_SERVER_USERNAME, FTP_SERVER_PASSWORD);
    LOG_INFO("FTP server on %s:21, user %s", WiFi.localIP().toString().c_str(), FTP_SERVER_USERNAME);
    return true;
}

void FtpServerThread::stop()
{
    if (!server)
        return;
    LOG_INFO("Stop FTP server");
    server->end();
    delete server;
    server = nullptr;
}

int32_t FtpServerThread::runOnce()
{
    if (!wifiConnected()) {
        stop(); // the listening socket does not survive the interface going down
        openSessions = 0;
        return kIdleIntervalMs;
    }

    if (!ensureStarted())
        return kIdleIntervalMs;

    {
        // The card shares its SPI bus with the display and the radio on some boards, so the lock is held for one
        // bounded drain rather than across a whole transfer - the library does its file I/O inside these calls.
        const uint32_t startMs = millis();
        const uint32_t budgetMs = openSessions > 0 ? kDrainBudgetActiveMs : kDrainBudgetIdleMs;
        concurrency::LockGuard g(spiLock);
        for (int i = 0; i < kDrainMaxCalls; i++) {
            server->handleFTP();
            if (Throttle::hasElapsed(startMs, budgetMs))
                break;
        }
    }

    return openSessions > 0 ? kActiveIntervalMs : kIdleIntervalMs;
}

void initFtpServer()
{
    if (!ftpServerThread)
        ftpServerThread = new FtpServerThread();
}

} // namespace ftp

#endif // HAS_FTP_SERVER
