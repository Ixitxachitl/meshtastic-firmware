#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RadioLibInterface.h"
#include "airtime.h"
#include "main.h"
#include "mesh/http/ContentHelper.h"
#include "mesh/http/WebServer.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#if HAS_WIFI
#include "mesh/wifi/WiFiAPClient.h"
#endif
#include "Power.h"
#include "SPILock.h"
#include "mesh/Throttle.h"
#include <FSCommon.h>
#include <HTTPBodyParser.hpp>
#include <HTTPMultipartBodyParser.hpp>
#include <HTTPURLEncodedBodyParser.hpp>
#include <cmath>
#include <sstream>
#include <vector>

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#endif

// Browsing the SD card over the web server. Needs the plain Arduino SD object that setupSDCard()
// mounts, so the SD_MMC and soft-SPI cards are out until they grow the same fs::FS global.
#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI) && !defined(HAS_SD_MMC) && defined(ARCH_ESP32)
#include <SD.h>
#include <cerrno>
#include <cstring>
// FATFS directly, for the one thing the VFS layer cannot express: clearing a file's attribute bits.
#if __has_include(<ff.h>)
#include <ff.h>
#define HAS_WEB_SDCARD_CHMOD 1
#else
#define HAS_WEB_SDCARD_CHMOD 0
#endif
#define HAS_WEB_SDCARD 1
#else
#define HAS_WEB_SDCARD 0
#endif

/*
  Including the esp32_https_server library will trigger a compile time error. I've
  tracked it down to a reoccurrance of this bug:
    https://gcc.gnu.org/bugzilla/show_bug.cgi?id=57824
  The work around is described here:
    https://forums.xilinx.com/t5/Embedded-Development-Tools/Error-with-Standard-Libaries-in-Zynq/td-p/450032

  Long story short is we need "#undef str" before including the esp32_https_server.
    - Jm Casler (jm@casler.org) Oct 2020
*/
#undef str

// Includes for the https server
//   https://github.com/fhessel/esp32_https_server
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPSServer.hpp>
#include <HTTPServer.hpp>
#include <SSLCert.hpp>

// The HTTPS Server comes in a separate namespace. For easier use, include it here.
using namespace httpsserver;

#include "mesh/http/ContentHandler.h"

#define DEST_FS_USES_LITTLEFS

// We need to specify some content-type mapping, so the resources get delivered with the
// right content type and are displayed correctly in the browser
char const *contentTypes[][2] = {{".txt", "text/plain"},     {".html", "text/html"},
                                 {".js", "text/javascript"}, {".png", "image/png"},
                                 {".jpg", "image/jpg"},      {".gz", "application/gzip"},
                                 {".gif", "image/gif"},      {".json", "application/json"},
                                 {".css", "text/css"},       {".ico", "image/vnd.microsoft.icon"},
                                 {".svg", "image/svg+xml"},  {"", ""}};

// const char *certificate = NULL; // change this as needed, leave as is for no TLS check (yolo security)

// Our API to handle messages to and from the radio.
HttpAPI webAPI;

void registerHandlers(HTTPServer *insecureServer, HTTPSServer *secureServer)
{

    // For every resource available on the server, we need to create a ResourceNode
    // The ResourceNode links URL and HTTP method to a handler function

    ResourceNode *nodeAPIv1ToRadioOptions = new ResourceNode("/api/v1/toradio", "OPTIONS", &handleAPIv1ToRadio);
    ResourceNode *nodeAPIv1ToRadio = new ResourceNode("/api/v1/toradio", "PUT", &handleAPIv1ToRadio);
    ResourceNode *nodeAPIv1FromRadioOptions = new ResourceNode("/api/v1/fromradio", "OPTIONS", &handleAPIv1FromRadio);
    ResourceNode *nodeAPIv1FromRadio = new ResourceNode("/api/v1/fromradio", "GET", &handleAPIv1FromRadio);

    ResourceNode *nodeAdmin = new ResourceNode("/admin", "GET", &handleAdmin);

    ResourceNode *nodeRestart = new ResourceNode("/restart", "POST", &handleRestart);
    ResourceNode *nodeFormUpload = new ResourceNode("/upload", "POST", &handleFormUpload);

    ResourceNode *nodeJsonScanNetworks = new ResourceNode("/json/scanNetworks", "GET", &handleScanNetworks);
    ResourceNode *nodeJsonReport = new ResourceNode("/json/report", "GET", &handleReport);
    ResourceNode *nodeJsonNodes = new ResourceNode("/json/nodes", "GET", &handleNodes);
    ResourceNode *nodeJsonFsBrowseStatic = new ResourceNode("/json/fs/browse/static", "GET", &handleFsBrowseStatic);
    ResourceNode *nodeJsonDelete = new ResourceNode("/json/fs/delete/static", "DELETE", &handleFsDeleteStatic);

#if HAS_WEB_SDCARD
    ResourceNode *nodeJsonFsBrowseSD = new ResourceNode("/json/fs/browse/sd", "GET", &handleFsBrowseSD);
    ResourceNode *nodeJsonDeleteSD = new ResourceNode("/json/fs/delete/sd", "DELETE", &handleFsDeleteSD);
    ResourceNode *nodeFormUploadSD = new ResourceNode("/upload/sd", "POST", &handleFormUploadSD);
    ResourceNode *nodeSDStatic = new ResourceNode("/sd/*", "GET", &handleSDStatic);
#endif

    ResourceNode *nodeRoot = new ResourceNode("/*", "GET", &handleStatic);

    // Secure nodes
    secureServer->registerNode(nodeAPIv1ToRadioOptions);
    secureServer->registerNode(nodeAPIv1ToRadio);
    secureServer->registerNode(nodeAPIv1FromRadioOptions);
    secureServer->registerNode(nodeAPIv1FromRadio);
    secureServer->registerNode(nodeRestart);
    secureServer->registerNode(nodeFormUpload);
    secureServer->registerNode(nodeJsonScanNetworks);
    secureServer->registerNode(nodeJsonFsBrowseStatic);
    secureServer->registerNode(nodeJsonDelete);
    secureServer->registerNode(nodeJsonReport);
    secureServer->registerNode(nodeJsonNodes);
    secureServer->registerNode(nodeAdmin);
#if HAS_WEB_SDCARD
    secureServer->registerNode(nodeJsonFsBrowseSD);
    secureServer->registerNode(nodeJsonDeleteSD);
    secureServer->registerNode(nodeFormUploadSD);
    secureServer->registerNode(nodeSDStatic);
#endif
    secureServer->registerNode(nodeRoot); // This has to be last

    // Insecure nodes
    insecureServer->registerNode(nodeAPIv1ToRadioOptions);
    insecureServer->registerNode(nodeAPIv1ToRadio);
    insecureServer->registerNode(nodeAPIv1FromRadioOptions);
    insecureServer->registerNode(nodeAPIv1FromRadio);
    insecureServer->registerNode(nodeRestart);
    insecureServer->registerNode(nodeFormUpload);
    insecureServer->registerNode(nodeJsonScanNetworks);
    insecureServer->registerNode(nodeJsonFsBrowseStatic);
    insecureServer->registerNode(nodeJsonDelete);
    insecureServer->registerNode(nodeJsonReport);
    insecureServer->registerNode(nodeAdmin);
#if HAS_WEB_SDCARD
    insecureServer->registerNode(nodeJsonFsBrowseSD);
    insecureServer->registerNode(nodeJsonDeleteSD);
    insecureServer->registerNode(nodeFormUploadSD);
    insecureServer->registerNode(nodeSDStatic);
#endif
    insecureServer->registerNode(nodeRoot); // This has to be last
}

void handleAPIv1FromRadio(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    LOG_DEBUG("webAPI handleAPIv1FromRadio");

    /*
        For documentation, see:
            https://meshtastic.org/docs/development/device/http-api
            https://meshtastic.org/docs/development/device/client-api
    */

    // Get access to the parameters
    ResourceParameters *params = req->getParams();

    // std::string paramAll = "all";
    std::string valueAll;

    // Status code is 200 OK by default.
    res->setHeader("Content-Type", "application/x-protobuf");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");
    res->setHeader("X-Protobuf-Schema", "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto");

    if (req->getMethod() == "OPTIONS") {
        res->setStatusCode(204); // Success with no content
        res->print("");
        return;
    }

    uint8_t txBuf[MAX_STREAM_BUF_SIZE];
    uint32_t len = 1;

    if (params->getQueryParameter("all", valueAll)) {

        // If all is true, return all the buffers we have available
        //   to us at this point in time.
        if (valueAll == "true") {
            while (len) {
                len = webAPI.getFromRadio(txBuf);
                res->write(txBuf, len);
            }

            // Otherwise, just return one protobuf
        } else {
            len = webAPI.getFromRadio(txBuf);
            res->write(txBuf, len);
        }

        // the param "all" was not specified. Return just one protobuf
    } else {
        len = webAPI.getFromRadio(txBuf);
        res->write(txBuf, len);
    }

    LOG_DEBUG("webAPI handleAPIv1FromRadio, len %d", len);
}

void handleAPIv1ToRadio(HTTPRequest *req, HTTPResponse *res)
{
    LOG_DEBUG("webAPI handleAPIv1ToRadio");

    /*
        For documentation, see:
            https://meshtastic.org/docs/development/device/http-api
            https://meshtastic.org/docs/development/device/client-api
    */

    res->setHeader("Content-Type", "application/x-protobuf");
    res->setHeader("Access-Control-Allow-Headers", "Content-Type");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "PUT, OPTIONS");
    res->setHeader("X-Protobuf-Schema", "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto");

    if (req->getMethod() == "OPTIONS") {
        res->setStatusCode(204); // Success with no content
        res->print("");
        return;
    }

    byte buffer[MAX_TO_FROM_RADIO_SIZE];
    size_t s = req->readBytes(buffer, MAX_TO_FROM_RADIO_SIZE);

    LOG_DEBUG("Received %d bytes from PUT request", s);
    webAPI.handleToRadio(buffer, s);

    res->write(buffer, s);
    LOG_DEBUG("webAPI handleAPIv1ToRadio");
}

// Escape a string into a JSON double-quoted literal. Matches the previous
// SimpleJSON StringifyString behavior (0x00-0x1F and 0x7F -> \u00xx lowercase,
// escapes " \ / \b \f \n \r \t, UTF-8 passes through unchanged).
static std::string jsonEscape(const std::string &str)
{
    std::string out = "\"";
    for (size_t i = 0; i < str.size(); ++i) {
        char chr = str[i];
        if (chr == '"' || chr == '\\' || chr == '/') {
            out += '\\';
            out += chr;
        } else if (chr == '\b') {
            out += "\\b";
        } else if (chr == '\f') {
            out += "\\f";
        } else if (chr == '\n') {
            out += "\\n";
        } else if (chr == '\r') {
            out += "\\r";
        } else if (chr == '\t') {
            out += "\\t";
        } else if ((unsigned char)chr < 0x20 || chr == 0x7F) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)chr);
            out += buf;
        } else {
            out += chr;
        }
    }
    out += "\"";
    return out;
}

// Format a numeric value the way the previous SimpleJSON serializer did
// (std::stringstream with precision 15, NaN/Inf -> "null").
static std::string jsonNum(double v)
{
    if (std::isinf(v) || std::isnan(v))
        return "null";
    std::ostringstream ss;
    ss.precision(15);
    ss << v;
    return ss.str();
}

// Build a serialized JSON array string listing files in `dirname`.
// Subdirectories recurse as nested arrays (up to `levels` deep).
std::string htmlListDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
    File root = fs.open(dirname, FILE_O_READ);
    std::string out = "[";
    bool first = true;
    if (!root) {
        out += "]";
        return out;
    }
    if (!root.isDirectory()) {
        out += "]";
        return out;
    }

    // iterate over the file list
    File file = root.openNextFile();
    while (file) {
        std::string element;
        bool haveElement = false;
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            if (levels) {
#ifdef ARCH_ESP32
                element = htmlListDir(fs, file.path(), levels - 1);
#else
                element = htmlListDir(fs, file.name(), levels - 1);
#endif
                haveElement = true;
                file.close();
            }
        } else {
#ifdef ARCH_ESP32
            String fileName = String(file.path()).substring(1);
#else
            String fileName = String(file.name()).substring(1);
#endif
            String tempName = String(file.name()).substring(1);
            // Keys in the previous std::map<string,...> were emitted in
            // alphabetical order: name, nameModified, size.
            element = "{";
            element += jsonEscape("name");
            element += ":";
            element += jsonEscape(fileName.c_str());
            if (tempName.endsWith(".gz")) {
#ifdef ARCH_ESP32
                String modifiedFile = String(file.path()).substring(1);
#else
                String modifiedFile = String(file.name()).substring(1);
#endif
                modifiedFile.remove((modifiedFile.length() - 3), 3);
                element += ",";
                element += jsonEscape("nameModified");
                element += ":";
                element += jsonEscape(modifiedFile.c_str());
            }
            element += ",";
            element += jsonEscape("size");
            element += ":";
            element += jsonNum((int)file.size());
            element += "}";
            haveElement = true;
        }
        if (haveElement) {
            if (!first)
                out += ",";
            out += element;
            first = false;
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    out += "]";
    return out;
}

void handleFsBrowseStatic(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    concurrency::LockGuard g(spiLock);
    std::string fileList = htmlListDir(FSCom, "/static", 10);

    uint64_t total = FSCom.totalBytes();
    uint64_t used = FSCom.usedBytes();

    // Key order matches the previous std::map-based emission (alphabetical).
    std::string out;
    out.reserve(fileList.size() + 128);
    out += "{\"data\":{\"files\":";
    out += fileList;
    out += ",\"filesystem\":{\"free\":";
    out += jsonNum((int)(total - used));
    out += ",\"total\":";
    out += jsonNum((int)total);
    out += ",\"used\":";
    out += jsonNum((int)used);
    out += "}},\"status\":\"ok\"}";

    res->print(out.c_str());
}

void handleFsDeleteStatic(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string paramValDelete;

    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "DELETE");

    if (params->getQueryParameter("delete", paramValDelete)) {
        std::string pathDelete = "/" + paramValDelete;
        concurrency::LockGuard g(spiLock);
        const char *status = FSCom.remove(pathDelete.c_str()) ? "ok" : "Error";
        LOG_INFO("%s", pathDelete.c_str());
        std::string out = "{\"status\":";
        out += jsonEscape(status);
        out += "}";
        res->print(out.c_str());
        return;
    }
}

void handleStatic(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    // Get access to the parameters
    ResourceParameters *params = req->getParams();

    std::string parameter1;
    // Print the first parameter value
    if (params->getPathParameter(0, parameter1)) {

        std::string filename = "/static/" + parameter1;
        std::string filenameGzip = "/static/" + parameter1 + ".gz";

        // Try to open the file
        File file;

        bool has_set_content_type = false;

        if (filename == "/static/") {
            filename = "/static/index.html";
            filenameGzip = "/static/index.html.gz";
        }

        concurrency::LockGuard g(spiLock);

        if (FSCom.exists(filename.c_str())) {
            file = FSCom.open(filename.c_str());
            if (!file.available()) {
                LOG_WARN("File not available - %s", filename.c_str());
            }
        } else if (FSCom.exists(filenameGzip.c_str())) {
            file = FSCom.open(filenameGzip.c_str());
            res->setHeader("Content-Encoding", "gzip");
            if (!file.available()) {
                LOG_WARN("File not available - %s", filenameGzip.c_str());
            }
        } else {
            has_set_content_type = true;
            filenameGzip = "/static/index.html.gz";
            file = FSCom.open(filenameGzip.c_str());
            res->setHeader("Content-Type", "text/html");
            if (!file.available()) {

                LOG_WARN("File not available - %s", filenameGzip.c_str());
                res->println("Web server is running.<br><br>The content you are looking for can't be found. Please see: <a "
                             "href=https://meshtastic.org/docs/software/web-client/>FAQ</a>.<br><br><a "
                             "href=/admin>admin</a>");

                return;
            } else {
                res->setHeader("Content-Encoding", "gzip");
            }
        }

        res->setHeader("Content-Length", httpsserver::intToString(file.size()));

        // Content-Type is guessed using the definition of the contentTypes-table defined above
        int cTypeIdx = 0;
        do {
            if (filename.rfind(contentTypes[cTypeIdx][0]) != std::string::npos) {
                res->setHeader("Content-Type", contentTypes[cTypeIdx][1]);
                has_set_content_type = true;
                break;
            }
            cTypeIdx += 1;
        } while (strlen(contentTypes[cTypeIdx][0]) > 0);

        if (!has_set_content_type) {
            // Set a default content type
            res->setHeader("Content-Type", "application/octet-stream");
        }

        // Read the file and write it to the HTTP response body
        size_t length = 0;
        do {
            char buffer[256];
            length = file.read((uint8_t *)buffer, 256);
            std::string bufferString(buffer, length);
            res->write((uint8_t *)bufferString.c_str(), bufferString.size());
        } while (length > 0);

        file.close();

        return;
    } else {
        LOG_ERROR("This should not have happened");
        res->println("ERROR: This should not have happened");
    }
}

// Shared by the LittleFS and SD upload routes. dirPrefix is prepended to the client's filename and
// redirectTo is where the browser is sent afterwards. budgetBytes is how much may be written before
// the transfer is refused - the caller measures it, because capacity lives on the concrete
// filesystem classes rather than on fs::FS. holdSpiLock says whether the bus is held for the whole
// transfer: fine for the internal flash, but the SD shares SPI with the radio, so there the lock is
// taken per write instead.
// Give up on an upload that has delivered nothing for this long. Generous: a client on a weak
// link can go quiet for several seconds without the transfer being dead.
#define UPLOAD_STALL_TIMEOUT_MS (15 * 1000)
// How long to stand aside when a read comes back empty, so the network task can run.
#define UPLOAD_IDLE_YIELD_MS 2

static void handleFormUploadTo(HTTPRequest *req, HTTPResponse *res, fs::FS &fs, const char *dirPrefix, uint64_t budgetBytes,
                               const char *redirectTo, bool holdSpiLock)
{

    LOG_DEBUG("Form Upload - Disable keep-alive");
    res->setHeader("Connection", "close");

    // First, we need to check the encoding of the form that we have received.
    // The browser will set the Content-Type request header, so we can use it for that purpose.
    // Then we select the body parser based on the encoding.
    // Actually we do this only for documentary purposes, we know the form is going
    // to be multipart/form-data.
    LOG_DEBUG("Form Upload - Creating body parser reference");
    std::unique_ptr<HTTPBodyParser> parser;
    std::string contentType = req->getHeader("Content-Type");

    // The content type may have additional properties after a semicolon, for example:
    // Content-Type: text/html;charset=utf-8
    // Content-Type: multipart/form-data;boundary=------s0m3w31rdch4r4c73rs
    // As we're interested only in the actual mime _type_, we strip everything after the
    // first semicolon, if one exists:
    size_t semicolonPos = contentType.find(";");
    if (semicolonPos != std::string::npos) {
        contentType.resize(semicolonPos);
    }

    // Now, we can decide based on the content type:
    if (contentType == "multipart/form-data") {
        LOG_DEBUG("Form Upload - multipart/form-data");
        parser.reset(new HTTPMultipartBodyParser(req));
    } else {
        LOG_DEBUG("Unknown POST Content-Type: %s", contentType.c_str());
        return;
    }

    res->printf("<html><head><meta http-equiv=\"refresh\" content=\"1;url=%s\" /><title>File "
                "Upload</title></head><body><h1>File Upload</h1>",
                redirectTo);

    // We iterate over the fields. Any field with a filename is uploaded.
    // Note that the BodyParser consumes the request body, meaning that you can iterate over the request's
    // fields only a single time. The reason for this is that it allows you to handle large requests
    // which would not fit into memory.
    bool didwrite = false;

    // parser->nextField() will move the parser to the next field in the request body (field meaning a
    // form field, if you take the HTML perspective). After the last field has been processed, nextField()
    // returns false and the while loop ends.
    while (parser->nextField()) {
        // For Multipart data, each field has three properties:
        // The name ("name" value of the <input> tag)
        // The filename (If it was a <input type="file">, this is the filename on the machine of the
        //   user uploading it)
        // The mime type (It is determined by the client. So do not trust this value and blindly start
        //   parsing files only if the type matches)
        std::string name = parser->getFieldName();
        std::string filename = parser->getFieldFilename();
        std::string mimeType = parser->getFieldMimeType();
        // We log all three values, so that you can observe the upload on the serial monitor:
        LOG_DEBUG("handleFormUpload: field name='%s', filename='%s', mimetype='%s'", name.c_str(), filename.c_str(),
                  mimeType.c_str());

        // Double check that it is what we expect
        if (name != "file") {
            LOG_DEBUG("Skip unexpected field");
            res->println("<p>No file found.</p>");
            return;
        }

        // Double check that it is what we expect
        if (filename == "") {
            LOG_DEBUG("Skip unexpected field");
            res->println("<p>No file found.</p>");
            return;
        }

        // You should check file name validity and all that, but we skip that to make the core
        // concepts of the body parser functionality easier to understand.
        std::string pathname = dirPrefix + filename;

        // Held for the whole transfer only where that cannot starve anything else of the bus.
        std::unique_ptr<concurrency::LockGuard> wholeTransferLock;
        if (holdSpiLock)
            wholeTransferLock.reset(new concurrency::LockGuard(spiLock));

        File file;
        // Measured once by the caller. The old code asked the filesystem for its used bytes on
        // every 512-byte chunk, which on FAT means walking the allocation table for each one.
        uint64_t budget = budgetBytes;
        {
            std::unique_ptr<concurrency::LockGuard> g;
            if (!holdSpiLock)
                g.reset(new concurrency::LockGuard(spiLock));
            file = fs.open(pathname.c_str(), FILE_O_WRITE);
        }
        if (!file) {
            res->printf("<p>Could not open %s for writing</p>", pathname.c_str());
            return;
        }
        size_t fileLength = 0;
        didwrite = true;

        // With endOfField you can check whether the end of field has been reached or if there's
        // still data pending. With multipart bodies, you cannot know the field size in advance.
        uint32_t lastProgressMs = millis();
        while (!parser->endOfField()) {
            esp_task_wdt_reset();

            byte buf[512];
            size_t readLength = parser->read(buf, sizeof(buf));

            // read() returns 0 both when the body is finished and when the next packet has simply
            // not arrived yet, and endOfField() cannot separate the two: on an empty buffer it has
            // nothing to compare against the boundary, so it says "not the end". Looping straight
            // back round therefore spins as fast as the CPU allows, which starves the network task
            // that would have delivered the next packet - the deadlock behind the "Multipart
            // incomplete" flood. Only the request knows whether more body is coming.
            if (readLength == 0) {
                if (req->requestComplete())
                    break;
                if (Throttle::hasElapsed(lastProgressMs, UPLOAD_STALL_TIMEOUT_MS)) {
                    LOG_ERROR("Upload %s: stalled after %u bytes", pathname.c_str(), (unsigned)fileLength);
                    std::unique_ptr<concurrency::LockGuard> g;
                    if (!holdSpiLock)
                        g.reset(new concurrency::LockGuard(spiLock));
                    file.flush();
                    file.close();
                    res->printf("<p>Upload stalled after %u bytes - the file on the card is incomplete.</p>",
                                (unsigned)fileLength);
                    return;
                }
                delay(UPLOAD_IDLE_YIELD_MS); // hand the CPU back so the next packet can land
                continue;
            }
            lastProgressMs = millis();

            if (readLength > budget) {
                std::unique_ptr<concurrency::LockGuard> g;
                if (!holdSpiLock)
                    g.reset(new concurrency::LockGuard(spiLock));
                file.flush();
                file.close();
                res->println("<p>Write aborted! Not enough free space.</p>");
                return;
            }
            budget -= readLength;

            size_t written = 0;
            int writeErrno = 0;
            {
                std::unique_ptr<concurrency::LockGuard> g;
                if (!holdSpiLock)
                    g.reset(new concurrency::LockGuard(spiLock));
                errno = 0;
                written = file.write(buf, readLength);
                writeErrno = errno;
            }
            // A short write was previously discarded, so a card that stopped accepting data
            // half way through still reported success and left a truncated file behind.
            if (written != readLength) {
                // errno is what separates a full card (ENOSPC) from a card that stopped talking
                // (EIO) from a handle another filesystem driver invalidated underneath us (EBADF).
                LOG_ERROR("Upload %s: short write at %u bytes (%u of %u): %s", pathname.c_str(), (unsigned)fileLength,
                          (unsigned)written, (unsigned)readLength, writeErrno ? strerror(writeErrno) : "no errno");
                fileLength += written;
                std::unique_ptr<concurrency::LockGuard> g;
                if (!holdSpiLock)
                    g.reset(new concurrency::LockGuard(spiLock));
                file.flush();
                file.close();
                res->printf("<p>Write failed after %u bytes (%s) - the file on the card is incomplete.</p>", (unsigned)fileLength,
                            writeErrno ? strerror(writeErrno) : "no errno");
                return;
            }
            fileLength += readLength;
        }

        {
            std::unique_ptr<concurrency::LockGuard> g;
            if (!holdSpiLock)
                g.reset(new concurrency::LockGuard(spiLock));
            file.flush();
            file.close();
        }

        // Size is echoed back so the page can compare it with the file it sent: the multipart
        // parser ends a field on a stalled stream the same way it ends one on a boundary, so a
        // transfer that stops early otherwise looks exactly like a complete one.
        LOG_INFO("Upload %s: %u bytes", pathname.c_str(), (unsigned)fileLength);
        res->printf("<p>Saved %u bytes to %s</p>", (unsigned)fileLength, pathname.c_str());

        // One file per form, and deliberately no second nextField(): that call blocks until the
        // closing boundary arrives, and its wait loop neither yields nor gives up. If the client
        // has gone away it can never be satisfied - _remainingContent stays non-zero, so
        // endOfBody() is never true - and it spins forever logging "Multipart incomplete". Taking
        // the one field we came for keeps that loop out of reach.
        break;
    }
    if (!didwrite) {
        res->println("<p>Did not write any file</p>");
    }
    res->println("</body></html>");
}

void handleFormUpload(HTTPRequest *req, HTTPResponse *res)
{
    // Keep 50k of the internal filesystem free - the config and prefs live there too.
    constexpr uint64_t reserveBytes = 51200;
    uint64_t budget = 0;
    {
        concurrency::LockGuard g(spiLock);
        const uint64_t total = fsTotalBytes(), used = fsUsedBytes();
        budget = (total > used + reserveBytes) ? (total - used - reserveBytes) : 0;
    }
    handleFormUploadTo(req, res, FSCom, "/static/", budget, "/static", true);
}

#if HAS_WEB_SDCARD
// How much of a file ?preview=1 will return. Enough to see what a config or script is doing,
// small enough that previewing a map tile set costs nothing.
#define SD_PREVIEW_MAX_BYTES 8192

// Self-contained listing/upload page. Deliberately plain: it is served from flash on a device with
// no room for a framework, and it only has to move files on and off the card.
static void sendSDBrowsePage(HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html; charset=utf-8");
    res->print(R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>SD card</title><style>
:root{--bg:#faf9f7;--fg:#1c1b19;--muted:#6f6b64;--line:#e3e0da;--card:#fff;--accent:#2f6f4f;--warn:#a8442f;--chip:#efece6}
@media(prefers-color-scheme:dark){:root{--bg:#141310;--fg:#e9e7e2;--muted:#97928a;--line:#2b2925;--card:#1c1a16;--accent:#7cc4a0;--warn:#e08b74;--chip:#26241f}}
*{box-sizing:border-box}
body{margin:0;padding:24px 16px;font:14px/1.55 ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;background:var(--bg);color:var(--fg)}
.wrap{max-width:720px;margin:0 auto}
h1{font-size:20px;font-weight:650;margin:0 0 2px;letter-spacing:-.01em}
.sub{color:var(--muted);font-size:13px;margin-bottom:18px}
.bar{height:6px;border-radius:99px;background:var(--chip);overflow:hidden;margin:10px 0 4px}
.bar i{display:block;height:100%;background:var(--accent);width:0;transition:width .3s}
.drop{display:block;border:1.5px dashed var(--line);border-radius:12px;padding:22px 16px;text-align:center;
 color:var(--muted);background:var(--card);cursor:pointer;transition:border-color .15s,background .15s;margin:0 0 18px}
.drop:hover,.drop.over{border-color:var(--accent);color:var(--fg)}
.drop b{color:var(--fg);font-weight:600}
.up{display:none;margin:0 0 18px}.up.on{display:block}
.up .bar i{background:var(--accent)}
ul{list-style:none;margin:0;padding:0;border:1px solid var(--line);border-radius:12px;overflow:hidden;background:var(--card)}
li{display:flex;align-items:center;gap:12px;padding:10px 14px}
li+li{border-top:1px solid var(--line)}
.ext{flex:none;width:38px;height:38px;border-radius:9px;background:var(--chip);color:var(--muted);
 display:flex;align-items:center;justify-content:center;font:600 10px/1 ui-monospace,monospace;letter-spacing:.04em}
.ext.dir{background:transparent;border:1.5px solid var(--line);color:var(--accent);font-size:15px}
.crumb{display:flex;align-items:center;gap:4px;flex-wrap:wrap;margin:0 0 10px;font-size:13px}
.crumb button{border:0;background:none;color:var(--accent);font:inherit;cursor:pointer;padding:2px 4px;border-radius:5px}
.crumb button:hover{background:var(--chip)}
.crumb span{color:var(--muted)}
.crumb b{color:var(--fg);font-weight:600;padding:2px 4px}
li.folder .nm a{color:var(--accent)}
.msg{margin:0 0 12px;padding:9px 12px;border-radius:9px;background:var(--chip);color:var(--warn);font-size:13px;display:none}
.msg.on{display:block}
.nm{flex:1;min-width:0}
.nm a{color:inherit;text-decoration:none;font-weight:550;display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.nm a:hover{color:var(--accent);text-decoration:underline}
.sz{flex:none;color:var(--muted);font-variant-numeric:tabular-nums;font-size:13px}
.del{flex:none;border:1px solid var(--line);background:transparent;color:var(--muted);border-radius:7px;
 padding:5px 9px;font:inherit;font-size:12px;cursor:pointer}
.del:hover{border-color:var(--warn);color:var(--warn)}
.empty,.err{padding:26px 14px;text-align:center;color:var(--muted)}
.err{color:var(--warn)}
.foot{color:var(--muted);font-size:12px;margin-top:12px;display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap}
.foot button{border:0;background:none;color:var(--accent);font:inherit;cursor:pointer;padding:0}
.view{flex:none;border:1px solid var(--line);background:transparent;color:var(--muted);border-radius:7px;
 padding:5px 9px;font:inherit;font-size:12px;cursor:pointer}
.view:hover{border-color:var(--accent);color:var(--accent)}
dialog{border:1px solid var(--line);border-radius:12px;background:var(--card);color:var(--fg);padding:0;
 width:min(680px,92vw);max-height:80vh;overflow:hidden}
dialog::backdrop{background:rgba(0,0,0,.45)}
dialog header{display:flex;align-items:center;gap:10px;padding:12px 14px;border-bottom:1px solid var(--line)}
dialog h2{font-size:14px;margin:0;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
dialog pre{margin:0;padding:14px;overflow:auto;max-height:62vh;font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;
 white-space:pre-wrap;word-break:break-word}
.note{padding:8px 14px;border-top:1px solid var(--line);color:var(--muted);font-size:12px}
</style>
<div class=wrap>
<h1>SD card</h1>
<div class=sub id=cap>Reading card...</div>
<div class=bar><i id=capbar></i></div>

<div class=crumb id=crumb></div>
<div class=msg id=msg></div>

<label class=drop id=drop>
 <input type=file id=f hidden>
 <b>Choose a file</b> or drag it here
</label>

<div class=up id=up><div id=upname></div><div class=bar><i id=upbar></i></div></div>

<ul id=list><li class=empty>Loading...</li></ul>
<div class=foot><span id=count></span><button onclick=load()>Refresh</button></div>
<div class=foot><span>Map tiles belong in the card root as <b>MAP.BIN</b>.</span></div>
</div>
<dialog id=dlg><header><h2 id=dlgname></h2><button class=view onclick="dlg.close()">Close</button></header>
<pre id=dlgbody></pre><div class=note id=dlgnote></div></dialog>
<script>
var L=document.getElementById('list'),cwd='/';
function sz(n){n=+n||0;return n>=1073741824?(n/1073741824).toFixed(2)+' GB':n>=1048576?(n/1048576).toFixed(1)+' MB':n>=1024?(n/1024).toFixed(1)+' kB':n+' B'}
function ext(n){var i=n.lastIndexOf('.');return i>0&&i<n.length-1?n.slice(i+1).slice(0,4).toUpperCase():'\u2022'}
function join(d,n){return d==='/'?'/'+n:d+'/'+n}
function say(t){var m=document.getElementById('msg');m.textContent=t||'';m.className=t?'msg on':'msg'}
function go(d){cwd=d||'/';load()}
function crumbs(){
 var c=document.getElementById('crumb');c.innerHTML='';
 var parts=cwd.split('/').filter(Boolean),path='/';
 var b=document.createElement('button');b.textContent='SD card';b.onclick=function(){go('/')};c.appendChild(b);
 parts.forEach(function(seg,i){
  var sp=document.createElement('span');sp.textContent='/';c.appendChild(sp);
  path=join(path,seg);
  if(i===parts.length-1){var cur=document.createElement('b');cur.textContent=seg;c.appendChild(cur)}
  else{var t=path,x=document.createElement('button');x.textContent=seg;x.onclick=function(){go(t)};c.appendChild(x)}})}
function del(f){
 var full=join(cwd,f.name);
 var q=f.dir?'Delete the folder '+full+' and everything inside it?':'Delete '+full+' from the card?';
 if(!confirm(q))return;
 say('');
 fetch('/json/fs/delete/sd?delete='+encodeURIComponent(full),{method:'DELETE'})
  .then(function(r){return r.json()}).then(function(j){
   if(j.status!=='ok')say('Could not delete '+f.name+(j.error?': '+j.error:''));load()})
  .catch(function(){say('Could not delete '+f.name+' - the request failed');load()})}
// Read with fetch, never as a navigation: the bytes stay in the page, so nothing reaches the
// downloads folder and the browser never has to decide whether the file type is dangerous.
function view(f){
 var dlg=document.getElementById('dlg'),full=join(cwd,f.name);
 document.getElementById('dlgname').textContent=full;
 document.getElementById('dlgbody').textContent='Reading...';
 document.getElementById('dlgnote').textContent='';
 dlg.showModal();
 var shown=0;
 // Taken from the response rather than assumed: the cap lives in the firmware, so this cannot drift.
 fetch('/sd/?preview=1&p='+encodeURIComponent(full)).then(function(r){
  shown=+r.headers.get('Content-Length')||0;return r.text()}).then(function(t){
  // Control bytes would wreck the layout, so show them as dots. Printable UTF-8 is left alone.
  document.getElementById('dlgbody').textContent=t.replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/g,'\u00b7')||'(empty file)';
  document.getElementById('dlgnote').textContent=shown<f.size?'First '+sz(shown)+' of '+sz(f.size)+' - preview only':sz(f.size)+' - whole file';
 }).catch(function(){document.getElementById('dlgbody').textContent='Could not read the file.'})}
function upRow(){
 var li=document.createElement('li');li.className='folder';
 var e=document.createElement('div');e.className='ext dir';e.textContent='\u2191';
 var d=document.createElement('div');d.className='nm';
 var a=document.createElement('a');a.href='#';a.textContent='..';
 var parent=cwd.slice(0,cwd.lastIndexOf('/'))||'/';
 a.onclick=function(ev){ev.preventDefault();go(parent)};
 d.appendChild(a);li.append(e,d);return li}
function row(f){
 var li=document.createElement('li');
 var e=document.createElement('div');e.className=f.dir?'ext dir':'ext';e.textContent=f.dir?'\u25b8':ext(f.name);
 var d=document.createElement('div');d.className='nm';
 var a=document.createElement('a');a.textContent=f.name;
 var full=join(cwd,f.name);
 if(f.dir){li.className='folder';a.href='#';a.onclick=function(ev){ev.preventDefault();go(full)}}
 else{a.href='/sd/?p='+encodeURIComponent(full);a.download=f.name}
 d.appendChild(a);
 var s=document.createElement('div');s.className='sz';s.textContent=f.dir?'':sz(f.size);
 li.append(e,d,s);
 if(!f.dir){var v=document.createElement('button');v.className='view';v.textContent='View';v.onclick=function(){view(f)};li.append(v)}
 var b=document.createElement('button');b.className='del';b.textContent='Delete';b.onclick=function(){del(f)};
 li.append(b);
 return li}
function load(){
 crumbs();
 fetch('/json/fs/browse/sd?path='+encodeURIComponent(cwd)).then(function(r){return r.json()}).then(function(j){
  var all=(j.data&&j.data.files||[]).filter(function(f){return f&&f.name});
  // Folders first, then files, each alphabetically - the order the card hands them back is arbitrary.
  all.sort(function(a,b){return (b.dir?1:0)-(a.dir?1:0)||a.name.localeCompare(b.name)});
  L.innerHTML='';
  if(cwd!=='/')L.appendChild(upRow());
  if(!all.length&&cwd==='/')L.innerHTML='<li class=empty>The card is empty.</li>';
  else if(!all.length)L.appendChild(Object.assign(document.createElement('li'),{className:'empty',textContent:'This folder is empty.'}));
  all.forEach(function(f){L.appendChild(row(f))});
  var fs=j.data&&j.data.filesystem||{},t=+fs.total||0,u=+fs.used||0;
  document.getElementById('cap').textContent=t?sz(u)+' used of '+sz(t)+' \u2022 '+sz(t-u)+' free':'Capacity unknown';
  document.getElementById('capbar').style.width=t?Math.min(100,u/t*100)+'%':'0';
  var nf=all.filter(function(f){return !f.dir}).length,nd=all.length-nf;
  document.getElementById('count').textContent=nf+(nf===1?' file':' files')+(nd?', '+nd+(nd===1?' folder':' folders'):'');
 }).catch(function(e){L.innerHTML='<li class=err>Could not read the card.</li>';
  document.getElementById('cap').textContent='No card, or it failed to mount.'})}
var drop=document.getElementById('drop'),inp=document.getElementById('f');
;['dragenter','dragover'].forEach(function(t){drop.addEventListener(t,function(e){e.preventDefault();drop.classList.add('over')})});
;['dragleave','drop'].forEach(function(t){drop.addEventListener(t,function(e){e.preventDefault();drop.classList.remove('over')})});
drop.addEventListener('drop',function(e){if(e.dataTransfer.files[0])send(e.dataTransfer.files[0])});
inp.addEventListener('change',function(){if(inp.files[0])send(inp.files[0])});
// One HTTP request per slice. A slice that fails is retried on its own, and the device reports
// how much it actually has, so a retry resumes from there instead of restarting a 474MB transfer.
var CHUNK=1048576,TRIES=4;
function put(file,off){
 return new Promise(function(res,rej){
  var end=Math.min(off+CHUNK,file.size);
  var x=new XMLHttpRequest();
  x.open('POST','/upload/sd?path='+encodeURIComponent(cwd)+'&name='+encodeURIComponent(file.name)+'&offset='+off);
  x.setRequestHeader('Content-Type','application/octet-stream');
  x.onload=function(){
   var j={};try{j=JSON.parse(x.responseText||'{}')}catch(e){}
   if(x.status<400&&j.status==='ok')res(j.size);
   // 409 carries the device's actual length: resume from there rather than failing outright.
   else if(x.status===409&&typeof j.have==='number')res(j.have);
   else rej(new Error(j.error||('HTTP '+x.status)));
  };
  x.onerror=function(){rej(new Error('connection lost'))};
  x.send(file.slice(off,end));
 })}
function send(file){
 var up=document.getElementById('up'),bar=document.getElementById('upbar'),nm=document.getElementById('upname');
 up.className='up on';bar.style.width='0';say('');
 var off=0,started=Date.now();
 function step(){
  if(off>=file.size){
   nm.textContent='Uploaded '+file.name;bar.style.width='100%';
   setTimeout(function(){up.className='up';load()},900);return}
  var attempt=0;
  function tryOnce(){
   var pct=(off/file.size*100);
   var secs=(Date.now()-started)/1000,rate=off/Math.max(secs,1);
   var left=rate>0?Math.round((file.size-off)/rate):0;
   nm.textContent='Uploading '+file.name+' - '+sz(off)+' of '+sz(file.size)+
    (rate>0?'  ('+sz(rate)+'/s, '+Math.floor(left/60)+'m '+(left%60)+'s left)':'')+
    (attempt?'  retry '+attempt+'/'+TRIES:'');
   bar.style.width=pct+'%';
   put(file,off).then(function(have){off=have;step()})
    .catch(function(e){
     if(++attempt>=TRIES){nm.textContent='Upload failed';
      say('Stopped at '+sz(off)+' of '+sz(file.size)+': '+e.message+'. Pick the same file again to resume.');
      setTimeout(function(){up.className='up';load()},1500);return}
     setTimeout(tryOnce,1000*attempt); // back off a little, the card may just be busy
    })}
  tryOnce()}
 step();
 inp.value=''}
load();
</script>)HTML");
}

// A client-supplied path is only ever used below the card root. Rejecting "..' outright is blunter
// than normalising, and blunt is what is wanted here.
static bool sdPathIsSafe(const std::string &path)
{
    return path.find("..") == std::string::npos;
}

// Normalise to a leading slash and no trailing one, so "/" and "maps/" and "/maps" all agree.
static std::string sdNormalizeDir(std::string dir)
{
    if (dir.empty() || dir[0] != '/')
        dir = "/" + dir;
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    return dir;
}

// One directory, not recursive, with directories flagged so the page can navigate into them.
// htmlListDir() is shaped for the /static tree: it inlines subdirectories as nested arrays and
// never names them, which is no use for browsing a card.
static std::string sdListDir(const char *path)
{
    std::string out = "[";
    File root = SD.open(path);
    if (!root || !root.isDirectory())
        return out + "]";

    bool first = true;
    File file = root.openNextFile();
    while (file) {
        if (!first)
            out += ",";
        first = false;
        out += "{\"name\":";
        out += jsonEscape(file.name());
        out += ",\"size\":";
        out += jsonNum((double)file.size());
        out += ",\"dir\":";
        out += file.isDirectory() ? "true" : "false";
        out += "}";
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return out + "]";
}

// ---- Minimal multipart/form-data reader -------------------------------------------------------
//
// HTTPMultipartBodyParser is not usable for a large upload. Its read() cannot tell a stalled
// stream from the end of a field; its wait loops neither yield nor time out, so an interrupted
// transfer spins forever; and every empty fill calls HTTPS_LOGE("Multipart incomplete"), which
// floods the console for the length of the transfer with no way to suppress it from a handler.
// HTTPRequest::readBytes() has none of those behaviours, so the part framing is done here instead.
namespace
{
// "multipart/form-data; boundary=----WebKitFormBoundaryXYZ" -> "------WebKitFormBoundaryXYZ"
// (the two leading dashes are part of how the boundary appears in the body, not in the header)
bool multipartBoundary(HTTPRequest *req, std::string &boundary)
{
    const std::string contentType = req->getHeader("Content-Type");
    size_t at = contentType.find("boundary=");
    if (at == std::string::npos)
        return false;
    std::string value = contentType.substr(at + 9);
    if (!value.empty() && value.front() == '"') {
        value.erase(0, 1);
        const size_t close = value.find('"');
        if (close != std::string::npos)
            value.resize(close);
    } else {
        const size_t end = value.find(';');
        if (end != std::string::npos)
            value.resize(end);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r'))
        value.pop_back();
    if (value.empty())
        return false;
    boundary = "--" + value;
    return true;
}

class UploadBody
{
  public:
    explicit UploadBody(HTTPRequest *req) : req_(req), lastDataMs_(millis()) {}

    bool stalled() const { return stalled_; }
    bool exhausted() const { return exhausted_; }
    std::string &buffer() { return buf_; }

    // Grow the buffer to at least `want` bytes. False once the body is over or has gone quiet for
    // too long - an empty read yields rather than spinning, which is what keeps the network task
    // able to deliver the bytes we are waiting for.
    bool fill(size_t want)
    {
        while (buf_.size() < want) {
            char chunk[512];
            const size_t got = req_->readBytes((byte *)chunk, sizeof(chunk));
            if (got > 0) {
                buf_.append(chunk, got);
                lastDataMs_ = millis();
                continue;
            }
            if (req_->requestComplete()) {
                exhausted_ = true;
                return false;
            }
            if (Throttle::hasElapsed(lastDataMs_, UPLOAD_STALL_TIMEOUT_MS)) {
                stalled_ = true;
                return false;
            }
            // This runs on loopTask, which Arduino only feeds the watchdog for between loop()
            // iterations. A handler that waits here without saying so is indistinguishable from a
            // hung one, and gets the device rebooted mid-upload.
            esp_task_wdt_reset();
            delay(UPLOAD_IDLE_YIELD_MS);
        }
        return true;
    }

    // Consume whatever is left of the body, yielding as we go. The server calls
    // discardRequestBody() on a handler that returns early, and that is a tight
    // `while (!requestComplete()) readBytes()` with no yield and no timeout - which is what put
    // loopTask into the watchdog after an aborted upload. Draining here first means the request is
    // already complete by the time it runs, so its loop exits immediately.
    void drain()
    {
        for (;;) {
            char chunk[512];
            const size_t got = req_->readBytes((byte *)chunk, sizeof(chunk));
            if (got > 0) {
                lastDataMs_ = millis();
                continue;
            }
            if (req_->requestComplete() || Throttle::hasElapsed(lastDataMs_, UPLOAD_STALL_TIMEOUT_MS))
                return;
            esp_task_wdt_reset();
            delay(UPLOAD_IDLE_YIELD_MS);
        }
    }

    // One CRLF-terminated line, CRLF removed.
    bool readLine(std::string &line)
    {
        size_t crlf;
        while ((crlf = buf_.find("\r\n")) == std::string::npos) {
            if (!fill(buf_.size() + 1))
                return false;
        }
        line = buf_.substr(0, crlf);
        buf_.erase(0, crlf + 2);
        return true;
    }

  private:
    HTTPRequest *req_;
    std::string buf_;
    uint32_t lastDataMs_;
    bool exhausted_ = false;
    bool stalled_ = false;
};
} // namespace

// Cards vary enormously in how they behave under a long stream of writes. Two things help and
// neither costs anything on a card that does not need them: keep each write on a sector-aligned
// block so the card is not forced into read-modify-write cycles, and treat a failed write as
// "busy" rather than fatal - a cheap card goes away for a while during internal erase and
// wear-levelling, and reports the write as failed rather than stalling until it is ready.
#define SD_WRITE_BLOCK_BYTES 4096
#define SD_WRITE_ATTEMPTS 5
#define SD_WRITE_RETRY_DELAY_MS 20

// Returns bytes written; short or zero means the card gave up. Takes the bus per attempt so the
// retry wait does not hold it against the radio.
static size_t sdWriteWithRetry(File &file, const uint8_t *data, size_t len, int &lastErrno)
{
    for (int attempt = 0; attempt < SD_WRITE_ATTEMPTS; attempt++) {
        size_t written = 0;
        {
            concurrency::LockGuard g(spiLock);
            errno = 0;
            written = file.write(data, len);
            lastErrno = errno;
        }
        if (written == len)
            return written;
        // A partial write has already consumed part of the buffer; retrying would duplicate it.
        if (written > 0)
            return written;
        LOG_WARN("SD write of %u bytes failed (attempt %d/%d): %s", (unsigned)len, attempt + 1, SD_WRITE_ATTEMPTS,
                 lastErrno ? strerror(lastErrno) : "no errno");
        esp_task_wdt_reset();
        delay(SD_WRITE_RETRY_DELAY_MS);
    }
    return 0;
}

// Clear the attribute bits that stop a file being unlinked. Autorun malware sets +R +H +S on
// itself precisely so a casual delete bounces, and the VFS layer has no way to express this.
static void sdClearAttributes(const char *path)
{
#if HAS_WEB_SDCARD_CHMOD
    if (f_chmod(path, 0, AM_RDO | AM_HID | AM_SYS) == FR_OK)
        LOG_INFO("SD cleared read-only/hidden/system on %s", path);
#else
    (void)path;
#endif
}

// Remove one file, retrying once with its attributes cleared.
static bool sdRemoveFile(const std::string &path, std::string &detail)
{
    errno = 0;
    if (SD.remove(path.c_str()))
        return true;
    const int firstErrno = errno;
    sdClearAttributes(path.c_str());
    errno = 0;
    if (SD.remove(path.c_str()))
        return true;
    detail = strerror(firstErrno ? firstErrno : errno);
    return false;
}

// Depth-first: a directory can only be removed once it is empty. Bounded because a FAT tree deep
// enough to overflow the stack would need thousands of nested directories, but the limit keeps a
// malformed card from taking the web task down with it.
static bool sdRemoveTree(const std::string &path, std::string &detail, int depth = 0)
{
    if (depth > 16) {
        detail = "directory nested too deeply";
        return false;
    }

    File entry = SD.open(path.c_str());
    const bool isDir = entry && entry.isDirectory();
    if (entry)
        entry.close();
    if (!isDir)
        return sdRemoveFile(path, detail);

    File dir = SD.open(path.c_str());
    std::vector<std::string> children;
    if (dir) {
        // Names are collected before anything is removed: deleting during the walk invalidates the
        // directory handle's position on FAT.
        File child = dir.openNextFile();
        while (child) {
            children.push_back(path == "/" ? "/" + std::string(child.name()) : path + "/" + child.name());
            child.close();
            child = dir.openNextFile();
        }
        dir.close();
    }

    for (const auto &child : children) {
        if (!sdRemoveTree(child, detail, depth + 1))
            return false;
    }

    errno = 0;
    if (SD.rmdir(path.c_str()))
        return true;
    const int firstErrno = errno;
    sdClearAttributes(path.c_str());
    errno = 0;
    if (SD.rmdir(path.c_str()))
        return true;
    detail = strerror(firstErrno ? firstErrno : errno);
    return false;
}

// The SD card is browsed from its root, not /static: it carries user data (map tiles and the like)
// rather than the web UI, and is the only storage on these boards big enough to hold it. One
// directory per request, chosen with ?path= - a card can hold thousands of files and the listing is
// built in RAM as a single string.
void handleFsBrowseSD(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    ResourceParameters *params = req->getParams();
    std::string requested;
    if (!params->getQueryParameter("path", requested))
        requested = "/";
    if (!sdPathIsSafe(requested)) {
        res->print("{\"status\":\"Error\"}");
        return;
    }
    const std::string dir = sdNormalizeDir(requested);

    concurrency::LockGuard g(spiLock);
    std::string fileList = sdListDir(dir.c_str());

    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();

    std::string out;
    out.reserve(fileList.size() + 128);
    out += "{\"data\":{\"path\":";
    out += jsonEscape(dir.c_str());
    out += ",\"files\":";
    out += fileList;
    // Straight to double, not through int: a card of a couple of GB overflows 32 bits signed and
    // comes back to the page as a negative capacity.
    out += ",\"filesystem\":{\"free\":";
    out += jsonNum((double)(total - used));
    out += ",\"total\":";
    out += jsonNum((double)total);
    out += ",\"used\":";
    out += jsonNum((double)used);
    out += "}},\"status\":\"ok\"}";

    res->print(out.c_str());
}

void handleFsDeleteSD(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string paramValDelete;

    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "DELETE");

    if (params->getQueryParameter("delete", paramValDelete)) {
        if (!sdPathIsSafe(paramValDelete)) {
            res->print("{\"status\":\"Error\",\"error\":\"bad path\"}");
            return;
        }
        std::string pathDelete = sdNormalizeDir(paramValDelete);
        if (pathDelete == "/") {
            res->print("{\"status\":\"Error\",\"error\":\"refusing to delete the card root\"}");
            return;
        }
        concurrency::LockGuard g(spiLock);

        // Handles both: a plain file, or a directory and everything under it.
        std::string detail;
        const bool ok = sdRemoveTree(pathDelete, detail);

        LOG_INFO("SD delete %s: %s%s", pathDelete.c_str(), ok ? "ok" : "FAILED - ", ok ? "" : detail.c_str());
        std::string out = "{\"status\":";
        out += jsonEscape(ok ? "ok" : "Error");
        if (!ok) {
            out += ",\"error\":";
            out += jsonEscape(detail.c_str());
        }
        out += "}";
        res->print(out.c_str());
    }
}

// Sequential chunk upload: one HTTP request per slice, raw body, no multipart framing at all.
// A single POST for a large map is a bet that nothing goes wrong for tens of minutes - one dropped
// packet or one write the card refuses and the whole transfer is lost. A slice that fails costs
// only itself, and the client simply sends it again.
//
// ?name= is the file, ?offset= is where this slice belongs. Offset 0 truncates; anything else
// appends, and must match the file's current length or the client and card have diverged.
static void handleChunkUploadSD(HTTPRequest *req, HTTPResponse *res, const std::string &dir)
{
    res->setHeader("Content-Type", "application/json");

    std::string name, offsetParam;
    req->getParams()->getQueryParameter("name", name);
    LOG_INFO("Upload slice: name='%s' offset='%s' len=%u", name.c_str(),
             req->getParams()->getQueryParameter("offset", offsetParam) ? offsetParam.c_str() : "(none)",
             (unsigned)req->getContentLength());
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    if (name.empty() || !sdPathIsSafe(name)) {
        res->setStatusCode(400);
        res->print("{\"status\":\"Error\",\"error\":\"bad name\"}");
        return;
    }

    uint64_t offset = 0;
    if (req->getParams()->getQueryParameter("offset", offsetParam))
        offset = strtoull(offsetParam.c_str(), nullptr, 10);

    const std::string pathname = dir + name;
    const size_t expected = req->getContentLength();

    File file;
    uint64_t existing = 0;
    bool positioned = true;
    int openErrno = 0;
    {
        concurrency::LockGuard g(spiLock);
        esp_task_wdt_reset();
        if (offset == 0) {
            // Unlink before creating rather than relying on "w" to truncate: truncation has to
            // rewrite the existing cluster chain, which fails on an entry left damaged by an
            // interrupted write. Removing it first sidesteps the old chain entirely.
            if (SD.exists(pathname.c_str()) && !SD.remove(pathname.c_str()))
                LOG_WARN("Upload: could not remove existing %s: %s", pathname.c_str(), strerror(errno));
            errno = 0;
            file = SD.open(pathname.c_str(), FILE_O_WRITE);
            openErrno = errno;
        } else {
            // "r+" and an explicit seek rather than append mode: where the next byte lands is then
            // stated outright instead of depending on how this core implements "a", and a slice
            // that arrives twice overwrites itself rather than being tacked on the end.
            errno = 0;
            file = SD.open(pathname.c_str(), "r+");
            openErrno = errno;
            if (file) {
                existing = file.size();
                if (existing >= offset)
                    positioned = file.seek((uint32_t)offset);
            }
        }
        esp_task_wdt_reset();
    }
    LOG_INFO("Upload slice: '%s' offset=%u have=%u open=%d seek=%d err=%s", pathname.c_str(), (unsigned)offset,
             (unsigned)existing, (int)(bool)file, (int)positioned, openErrno ? strerror(openErrno) : "-");
    if (!file) {
        res->setStatusCode(500);
        res->print("{\"status\":\"Error\",\"error\":\"could not open the file\"}");
        return;
    }
    // A short file means the client is ahead of the card; a failed seek means we cannot place the
    // bytes at all. Either way, report what is actually there and let the client resume from it.
    if (existing < offset || !positioned) {
        // Out of step: a retried slice that already landed, or one sent out of order. Saying so
        // lets the client resume from where the card actually is instead of corrupting the file.
        char out[128];
        snprintf(out, sizeof(out), "{\"status\":\"Error\",\"error\":\"offset mismatch\",\"have\":%u}", (unsigned)existing);
        {
            concurrency::LockGuard g(spiLock);
            file.close();
        }
        res->setStatusCode(409);
        res->print(out);
        return;
    }

    UploadBody body(req);
    size_t written = 0;
    std::string problem;

    while (written < expected) {
        esp_task_wdt_reset();
        const size_t want = std::min((size_t)SD_WRITE_BLOCK_BYTES, expected - written);
        body.fill(want);
        std::string &buf = body.buffer();
        const size_t take = std::min(want, buf.size());
        if (take == 0) {
            problem = body.stalled() ? "the connection went quiet" : "the slice ended early";
            break;
        }
        int writeErrno = 0;
        const size_t got = sdWriteWithRetry(file, (const uint8_t *)buf.data(), take, writeErrno);
        written += got;
        buf.erase(0, got);
        if (got != take) {
            problem = writeErrno ? strerror(writeErrno) : "the card stopped accepting data";
            break;
        }
    }

    uint64_t finalSize = offset + written;
    {
        concurrency::LockGuard g(spiLock);
        // Only interrogate a handle the card was still talking to. After a run of write errors
        // flush()/size() go back through the same failed layer, and that is where this faulted -
        // close and report what we counted ourselves instead.
        if (problem.empty()) {
            file.flush();
            finalSize = file.size();
        }
        file.close();
    }
    body.drain();

    char out[160];
    if (problem.empty()) {
        snprintf(out, sizeof(out), "{\"status\":\"ok\",\"written\":%u,\"size\":%u}", (unsigned)written, (unsigned)finalSize);
        res->print(out);
    } else {
        LOG_ERROR("Upload %s: slice at %u failed after %u bytes - %s", pathname.c_str(), (unsigned)offset, (unsigned)written,
                  problem.c_str());
        snprintf(out, sizeof(out), "{\"status\":\"Error\",\"error\":\"%s\",\"size\":%u}", problem.c_str(), (unsigned)finalSize);
        res->setStatusCode(500);
        res->print(out);
    }
}

void handleFormUploadSD(HTTPRequest *req, HTTPResponse *res)
{
    std::string requested;
    if (!req->getParams()->getQueryParameter("path", requested))
        requested = "/";
    if (!sdPathIsSafe(requested)) {
        res->setStatusCode(400);
        res->println("Bad path");
        return;
    }
    std::string dir = sdNormalizeDir(requested);
    if (dir.back() != '/')
        dir += "/";

    // ?name= means a raw sequential slice; without it, the old whole-file multipart form.
    std::string probe;
    if (req->getParams()->getQueryParameter("name", probe)) {
        handleChunkUploadSD(req, res, dir);
        return;
    }

    res->setHeader("Connection", "close");
    res->setHeader("Content-Type", "text/html");

    std::string boundary;
    if (!multipartBoundary(req, boundary)) {
        res->println("<p>Not a multipart upload.</p>");
        return;
    }

    UploadBody body(req);

    // Walk the preamble to the first boundary, then the part headers, taking the filename from
    // Content-Disposition. Only the first file part is used; the form never sends another.
    std::string line, filename;
    bool inPartHeaders = false;
    while (body.readLine(line)) {
        if (!inPartHeaders) {
            if (line == boundary)
                inPartHeaders = true;
            else if (line == boundary + "--")
                break;
            continue;
        }
        if (line.empty())
            break; // blank line ends the part headers
        const size_t at = line.find("filename=\"");
        if (at != std::string::npos) {
            const size_t close = line.find('"', at + 10);
            if (close != std::string::npos)
                filename = line.substr(at + 10, close - at - 10);
        }
    }
    // Whatever the client called it, the file lands in the directory being browsed and nowhere else.
    const size_t slash = filename.find_last_of("/\\");
    if (slash != std::string::npos)
        filename = filename.substr(slash + 1);
    if (filename.empty() || !sdPathIsSafe(filename)) {
        body.drain();
        res->println("<p>No file found.</p>");
        return;
    }

    const std::string pathname = dir + filename;
    uint64_t budget = 0;
    File file;
    {
        concurrency::LockGuard g(spiLock);
        // usedBytes() walks the whole allocation table, which on a card this size is seconds of
        // blocking with the bus held - long enough on its own to reach the watchdog.
        esp_task_wdt_reset();
        const uint64_t total = SD.totalBytes(), used = SD.usedBytes();
        esp_task_wdt_reset();
        budget = (total > used) ? (total - used) : 0;
        file = SD.open(pathname.c_str(), FILE_O_WRITE);
    }
    if (!file) {
        body.drain();
        res->printf("<p>Could not open %s for writing</p>", pathname.c_str());
        return;
    }

    // A boundary is preceded by CRLF, and that CRLF belongs to the framing rather than the file.
    const std::string needle = "\r\n" + boundary;
    size_t fileLength = 0;
    bool complete = false;
    std::string problem;

    for (;;) {
        esp_task_wdt_reset();
        // Two blocks' worth, so a whole aligned block is available after holding back the needle.
        body.fill(SD_WRITE_BLOCK_BYTES * 2);
        std::string &buf = body.buffer();
        const size_t at = buf.find(needle);

        // Without the boundary in hand, hold back a needle's worth: it may straddle two reads.
        size_t take = (at != std::string::npos) ? at : (buf.size() > needle.size() ? buf.size() - needle.size() : 0);

        // Mid-stream, round down to a whole block; the ragged tail goes out with the last write,
        // once the boundary has landed or the body has run out.
        const bool finishing = (at != std::string::npos) || body.exhausted() || body.stalled();
        if (!finishing)
            take -= take % SD_WRITE_BLOCK_BYTES;

        if (take > budget) {
            problem = "not enough free space";
            break;
        }

        if (take > 0) {
            int writeErrno = 0;
            const size_t written = sdWriteWithRetry(file, (const uint8_t *)buf.data(), take, writeErrno);
            fileLength += written;
            budget -= written;
            buf.erase(0, written);
            if (written != take) {
                problem = writeErrno ? strerror(writeErrno) : "the card stopped accepting data";
                break;
            }
        }

        if (at != std::string::npos) {
            complete = true;
            break;
        }
        if (body.stalled()) {
            problem = "the connection went quiet";
            break;
        }
        if (body.exhausted() && buf.size() <= needle.size()) {
            problem = "the upload ended without its closing boundary";
            break;
        }
    }

    {
        concurrency::LockGuard g(spiLock);
        file.flush();
        file.close();
    }

    // Whether we finished or gave up, the rest of the body still has to come off the wire.
    body.drain();

    if (complete) {
        LOG_INFO("Upload %s: %u bytes", pathname.c_str(), (unsigned)fileLength);
        res->printf("<p>Saved %u bytes to %s</p>", (unsigned)fileLength, pathname.c_str());
    } else {
        LOG_ERROR("Upload %s: incomplete at %u bytes - %s", pathname.c_str(), (unsigned)fileLength, problem.c_str());
        res->printf("<p>Upload incomplete after %u bytes - %s.</p>", (unsigned)fileLength, problem.c_str());
    }
}

void handleSDStatic(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    ResourceParameters *params = req->getParams();

    // A file in a subdirectory cannot be named in the path: this router's wildcard stops at the
    // next '/', so /sd/maps/tile.bin never matches. ?p= carries the whole relative path instead,
    // and the bare path segment stays supported for a direct link to a file at the root.
    std::string requested;
    bool haveName = params->getQueryParameter("p", requested) && !requested.empty();
    if (!haveName)
        haveName = params->getPathParameter(0, requested) && !requested.empty();

    if (!haveName) {
        // /sd/ with no file named: the browser page itself. Served from here rather than /static
        // because the web client there is a prebuilt bundle that knows nothing about these routes.
        sendSDBrowsePage(res);
        return;
    }
    if (!sdPathIsSafe(requested)) {
        res->setStatusCode(404);
        res->println("Not found");
        return;
    }

    const std::string filename = sdNormalizeDir(requested);

    // ?preview=1 sends the head of the file as text/plain instead of the whole thing as a download.
    // Bounded here rather than by the client hanging up: without a cap this handler would still
    // read every block off the card, holding the shared SPI bus for a file it is not sending.
    std::string previewParam;
    const bool preview = params->getQueryParameter("preview", previewParam);

    File file;
    {
        concurrency::LockGuard g(spiLock);
        if (!SD.exists(filename.c_str())) {
            res->setStatusCode(404);
            res->println("Not found");
            return;
        }
        file = SD.open(filename.c_str());
    }
    if (!file) {
        res->setStatusCode(404);
        res->println("Not found");
        return;
    }

    size_t remaining = file.size();
    if (preview) {
        if (remaining > SD_PREVIEW_MAX_BYTES)
            remaining = SD_PREVIEW_MAX_BYTES;
        // text/plain, and never an attachment: the point is to inspect a file without it landing on
        // the client's disk, which is also why the browser page reads this with fetch().
        res->setHeader("Content-Type", "text/plain; charset=utf-8");
    } else {
        res->setHeader("Content-Type", "application/octet-stream");
    }
    res->setHeader("Content-Length", httpsserver::intToString(remaining));

    // The bus is shared with the radio, so it is taken per read rather than held for the whole file.
    while (remaining > 0) {
        char buffer[512];
        const size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t length = 0;
        {
            concurrency::LockGuard g(spiLock);
            length = file.read((uint8_t *)buffer, want);
        }
        if (!length)
            break;
        res->write((uint8_t *)buffer, length);
        remaining -= length;
    }

    concurrency::LockGuard g(spiLock);
    file.close();
}
#endif

void handleReport(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string content;

    if (!params->getQueryParameter("content", content)) {
        content = "json";
    }

    if (content == "json") {
        res->setHeader("Content-Type", "application/json");
        res->setHeader("Access-Control-Allow-Origin", "*");
        res->setHeader("Access-Control-Allow-Methods", "GET");
    } else {
        res->setHeader("Content-Type", "text/html");
        res->println("<pre>");
    }

    auto arrayFromLog = [](const uint32_t *logArray, int count) -> std::string {
        std::string s = "[";
        for (int i = 0; i < count; i++) {
            if (i)
                s += ",";
            s += jsonNum((int)logArray[i]);
        }
        s += "]";
        return s;
    };

    // One constant sizes the buffer and the count, so they cannot drift. Buffer is per call, so a
    // report that fails emits zeros rather than the previous type's data.
    constexpr size_t periods = AirTime::getPeriodsToLog();
    auto reportFor = [&](reportTypes reportType) {
        uint32_t logArray[periods] = {0};
        (void)airTime->airtimeReport(reportType, logArray, periods);
        return arrayFromLog(logArray, (int)periods);
    };

    std::string txLog = reportFor(TX_LOG);
    std::string rxLog = reportFor(RX_LOG);
    std::string rxAllLog = reportFor(RX_ALL_LOG);

    String wifiIPString = WiFi.localIP().toString();
    std::string wifiIP = wifiIPString.c_str();

    spiLock->lock();
    uint64_t fsTotal = FSCom.totalBytes();
    uint64_t fsUsed = FSCom.usedBytes();
    spiLock->unlock();

    // Emit keys in the same alphabetical order as the previous
    // std::map-based JSON output to keep responses byte-compatible.
    std::string out;
    out.reserve(1024);
    out += "{\"data\":{";

    // airtime
    out += "\"airtime\":{";
    out += "\"channel_utilization\":";
    out += jsonNum(airTime->channelUtilizationPercent());
    out += ",\"periods_to_log\":";
    out += jsonNum(airTime->getPeriodsToLog());
    out += ",\"rx_all_log\":";
    out += rxAllLog;
    out += ",\"rx_log\":";
    out += rxLog;
    out += ",\"seconds_per_period\":";
    out += jsonNum((int)airTime->getSecondsPerPeriod());
    out += ",\"seconds_since_boot\":";
    out += jsonNum((int)airTime->getSecondsSinceBoot());
    out += ",\"tx_log\":";
    out += txLog;
    out += ",\"utilization_tx\":";
    out += jsonNum(airTime->utilizationTXPercent());
    out += "}";

    // device
    out += ",\"device\":{\"reboot_counter\":";
    out += jsonNum((int)myNodeInfo.reboot_count);
    out += "}";

    // memory
    out += ",\"memory\":{";
    out += "\"fs_free\":";
    out += jsonNum((int)(fsTotal - fsUsed));
    out += ",\"fs_total\":";
    out += jsonNum((int)fsTotal);
    out += ",\"fs_used\":";
    out += jsonNum((int)fsUsed);
    out += ",\"heap_free\":";
    out += jsonNum((int)memGet.getFreeHeap());
    out += ",\"heap_total\":";
    out += jsonNum((int)memGet.getHeapSize());
    out += ",\"psram_free\":";
    out += jsonNum((int)memGet.getFreePsram());
    out += ",\"psram_total\":";
    out += jsonNum((int)memGet.getPsramSize());
    out += "}";

    // power (has_* / is_charging were serialized as the strings "true"/"false")
    out += ",\"power\":{";
    out += "\"battery_percent\":";
    out += jsonNum(powerStatus->getBatteryChargePercent());
    out += ",\"battery_voltage_mv\":";
    out += jsonNum(powerStatus->getBatteryVoltageMv());
    out += ",\"has_battery\":";
    out += jsonEscape(BoolToString(powerStatus->getHasBattery()));
    out += ",\"has_usb\":";
    out += jsonEscape(BoolToString(powerStatus->getHasUSB()));
    out += ",\"is_charging\":";
    out += jsonEscape(BoolToString(powerStatus->getIsCharging()));
    out += "}";

    // radio
    out += ",\"radio\":{\"frequency\":";
    out += jsonNum(RadioLibInterface::instance->getFreq());
    out += ",\"lora_channel\":";
    out += jsonNum((int)RadioLibInterface::instance->getChannelNum() + 1);
    out += "}";

    // wifi
    out += ",\"wifi\":{\"ip\":";
    out += jsonEscape(wifiIP);
    out += ",\"rssi\":";
    out += jsonNum(WiFi.RSSI());
    out += "}";

    out += "},\"status\":\"ok\"}";

    res->print(out.c_str());
}

void handleNodes(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string content;

    if (!params->getQueryParameter("content", content)) {
        content = "json";
    }

    if (content == "json") {
        res->setHeader("Content-Type", "application/json");
        res->setHeader("Access-Control-Allow-Origin", "*");
        res->setHeader("Access-Control-Allow-Methods", "GET");
    } else {
        res->setHeader("Content-Type", "text/html");
        res->println("<pre>");
    }

    std::string out;
    out.reserve(2048);
    out += "{\"data\":{\"nodes\":[";

    bool firstNode = true;
    uint32_t readIndex = 0;
    const meshtastic_NodeInfoLite *tempNodeInfo = nodeDB->readNextMeshNode(readIndex);
    while (tempNodeInfo != NULL) {
        if (nodeInfoLiteHasUser(tempNodeInfo)) {
            char id[16];
            snprintf(id, sizeof(id), "!%08x", tempNodeInfo->num);

            std::string position;
            if (nodeDB->hasValidPosition(tempNodeInfo)) {
                meshtastic_PositionLite posLite;
                if (nodeDB->copyNodePosition(tempNodeInfo->num, posLite)) {
                    position = "{\"altitude\":";
                    position += jsonNum((int)posLite.altitude);
                    position += ",\"latitude\":";
                    position += jsonNum((float)posLite.latitude_i * 1e-7);
                    position += ",\"longitude\":";
                    position += jsonNum((float)posLite.longitude_i * 1e-7);
                    position += "}";
                } else {
                    position = "null";
                }
            } else {
                position = "null";
            }

            if (!firstNode)
                out += ",";
            firstNode = false;

            // Alphabetical key order matches previous std::map-based output.
            out += "{\"hw_model\":";
            out += jsonNum(tempNodeInfo->hw_model);
            out += ",\"id\":";
            out += jsonEscape(id);
            out += ",\"last_heard\":";
            out += jsonNum((int)tempNodeInfo->last_heard);
            out += ",\"long_name\":";
            out += jsonEscape(tempNodeInfo->long_name);
            out += ",\"mac_address\":";
            out += jsonEscape("00:00:00:00:00:00");
            out += ",\"position\":";
            out += position;
            out += ",\"short_name\":";
            out += jsonEscape(tempNodeInfo->short_name);
            out += ",\"snr\":";
            out += jsonNum(tempNodeInfo->snr);
            out += ",\"via_mqtt\":";
            out += jsonEscape(BoolToString(nodeInfoLiteViaMqtt(tempNodeInfo)));
            out += "}";
        }
        tempNodeInfo = nodeDB->readNextMeshNode(readIndex);
    }

    out += "]},\"status\":\"ok\"}";
    res->print(out.c_str());
}

void handleAdmin(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    //    res->println("<a href=/admin/settings>Settings</a><br>");
    //    res->println("<a href=/admin/fs>Manage Web Content</a><br>");
    res->println("<a href=/json/report>Device Report</a><br>");
}

void handleRestart(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    res->println("Restarting");

    LOG_DEBUG("Restarted on HTTP(s) Request");
    webServerThread->requestRestart = (millis() / 1000) + 5;
}

void handleScanNetworks(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");
    // res->setHeader("Content-Type", "text/html");

    int n = WiFi.scanNetworks();

    std::string out = "{\"data\":[";
    bool firstNet = true;
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            char ssidArray[50];
            // The previous implementation pre-escaped quotes before handing
            // the value to the JSON serializer; preserve that (byte-compatible
            // even if it double-encodes a quote) so existing clients are not
            // affected by this refactor.
            String ssidString = String(WiFi.SSID(i));
            ssidString.replace("\"", "\\\"");
            ssidString.toCharArray(ssidArray, 50);

            if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
                if (!firstNet)
                    out += ",";
                firstNet = false;
                out += "{\"rssi\":";
                out += jsonNum((int)WiFi.RSSI(i));
                out += ",\"ssid\":";
                out += jsonEscape(ssidArray);
                out += "}";
            }
            // Yield some cpu cycles to IP stack.
            //   This is important in case the list is large and it takes us time to return
            //   to the main loop.
            yield();
        }
    }
    out += "],\"status\":\"ok\"}";
    res->print(out.c_str());
}
#endif
