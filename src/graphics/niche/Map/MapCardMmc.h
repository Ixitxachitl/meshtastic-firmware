#pragma once

// The subset of SdFat's SdFs/FsFile the map code uses, over the Arduino SD_MMC card that setupSDCard() mounts.
// SdFat drives cards over SPI only; an SDIO-wired card (HAS_SD_MMC) has no chip select for it to use.

#include "configuration.h"

#if defined(HAS_SD_MMC) && defined(ARCH_ESP32)

#include <SD_MMC.h>
#include <fcntl.h>
#include <string.h>

namespace NicheGraphics::MapTiles
{

class FsFile
{
  public:
    FsFile() = default;
    explicit FsFile(fs::File f) : file(f) {}

    explicit operator bool() { return (bool)file; }
    void close() { file.close(); }
    int read(void *buf, size_t n) { return (int)file.read(static_cast<uint8_t *>(buf), n); }
    size_t write(const void *buf, size_t n) { return file.write(static_cast<const uint8_t *>(buf), n); }
    bool seekSet(uint64_t pos) { return file.seek((uint32_t)pos, fs::SeekSet); }
    uint64_t fileSize() { return file.size(); }
    bool isDir() { return file && file.isDirectory(); }

    // The next entry of `dir`; false at the end.
    bool openNext(FsFile *dir, int)
    {
        file = dir->file.openNextFile();
        return (bool)file;
    }

    size_t getName(char *name, size_t size)
    {
        if (size == 0)
            return 0;
        strncpy(name, file ? file.name() : "", size - 1);
        name[size - 1] = '\0';
        return strlen(name);
    }

  private:
    fs::File file;
};

class SdFs
{
  public:
    FsFile open(const char *path, int oflag)
    {
        if ((oflag & O_ACCMODE) == O_RDONLY)
            return SD_MMC.exists(path) ? FsFile(SD_MMC.open(path, FILE_READ)) : FsFile();
        return FsFile(SD_MMC.open(path, (oflag & O_TRUNC) ? FILE_WRITE : FILE_APPEND));
    }

    bool exists(const char *path) { return SD_MMC.exists(path); }
    bool remove(const char *path) { return SD_MMC.remove(path); }
    bool rename(const char *from, const char *to) { return SD_MMC.rename(from, to); }

    // With `parents`, each missing folder on the way down is made too.
    bool mkdir(const char *path, bool parents = true)
    {
        if (!parents)
            return SD_MMC.mkdir(path);
        char partial[96];
        const size_t len = strlen(path);
        if (len >= sizeof(partial))
            return false;
        for (size_t i = 1; i <= len; i++) {
            if (path[i] != '/' && path[i] != '\0')
                continue;
            memcpy(partial, path, i);
            partial[i] = '\0';
            if (!SD_MMC.exists(partial) && !SD_MMC.mkdir(partial))
                return false;
        }
        return true;
    }
};

} // namespace NicheGraphics::MapTiles

#endif
