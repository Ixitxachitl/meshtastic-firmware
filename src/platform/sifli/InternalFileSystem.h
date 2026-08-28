// InternalFileSystem.h - Zephyr LittleFS backend for nRF54L15
//
// Implements the Adafruit InternalFileSystem API subset used by Meshtastic,
// backed by Zephyr's fs/littlefs on the storage_partition of the nRF54L15's
// internal RRAM. Partition size is taken from the DTS at compile time via
// FIXED_PARTITION_SIZE(storage_partition) - the DK overlay currently maps
// ~700 KB into slot1 (see zephyr/boards/nrf54l15dk_nrf54l15_cpuapp.overlay).
//
// Mount point: /lfs
// All paths passed to open/exists/mkdir etc. are relative to the FS root
// (e.g. "/prefs/config.proto") and are prepended with "/lfs" internally.
//
// File objects are copyable via std::shared_ptr<SiFliFileState>.
// The underlying Zephyr handle is closed when the last copy is destroyed.

#pragma once

#include "Arduino.h" // String
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>

#ifndef FILE_O_READ
#define FILE_O_READ "r"
#define FILE_O_WRITE "w"
#endif

#define SIFLI_FS_MOUNT "/lfs"

// The TF card, mounted as a second FAT volume. Paths beginning with this are
// passed through untouched instead of being rebased onto the LittleFS mount.
#define SIFLI_SD_MOUNT "/SD"

// Probes the slot and mounts the card, if one is fitted. Safe to call twice.
void sifliSdBegin();

// True once a card has been found, mounted and not since removed.
bool sifliSdMounted();
#define SIFLI_FS_PATHLEN 256

namespace Adafruit_LittleFS_Namespace
{

class InternalFileSystem; // forward

// ── Internal file/dir state ───────────────────────────────────────────────

struct SiFliFileState {
    bool valid = false;
    bool is_dir = false;

    // Absolute Zephyr path, e.g. "/lfs/prefs/config.proto"
    char fullpath[SIFLI_FS_PATHLEN] = {0};
    // Path from FS root, e.g. "/prefs/config.proto"  (returned by name())
    char relpath[SIFLI_FS_PATHLEN] = {0};

    struct fs_file_t file;
    struct fs_dir_t dir;

    SiFliFileState()
    {
        fs_file_t_init(&file);
        fs_dir_t_init(&dir);
    }

    ~SiFliFileState()
    {
        if (valid) {
            if (is_dir)
                fs_closedir(&dir);
            else
                fs_close(&file);
            valid = false;
        }
    }
};

// ── File ─────────────────────────────────────────────────────────────────

class File
{
  public:
    File() = default;
    explicit File(InternalFileSystem &) {} // nRF52 compat constructor

    explicit operator bool() const { return _s && _s->valid; }

    int read(void *buf, uint16_t nbyte)
    {
        if (!_s || !_s->valid || _s->is_dir)
            return -1;
        ssize_t n = fs_read(&_s->file, buf, nbyte);
        return n < 0 ? -1 : (int)n;
    }

    // Arduino's Stream-style name, used by MessageStore.
    size_t readBytes(char *buf, size_t nbyte)
    {
        int n = read(buf, (uint16_t)nbyte);
        return n > 0 ? (size_t)n : 0;
    }

    int read()
    {
        uint8_t b;
        return read(&b, 1) == 1 ? (int)b : -1;
    }

    size_t write(const uint8_t *buf, size_t len)
    {
        if (!_s || !_s->valid || _s->is_dir)
            return 0;
        ssize_t n = fs_write(&_s->file, buf, len);
        return n < 0 ? 0 : (size_t)n;
    }

    size_t write(uint8_t b) { return write(&b, 1); }

    void flush()
    {
        if (_s && _s->valid && !_s->is_dir)
            fs_sync(&_s->file);
    }

    void close() { _s.reset(); }

    size_t size()
    {
        if (!_s || !_s->valid || _s->is_dir)
            return 0;
        struct fs_dirent entry;
        if (fs_stat(_s->fullpath, &entry) == 0)
            return (size_t)entry.size;
        return 0;
    }

    bool isDirectory() { return _s && _s->valid && _s->is_dir; }

    // Returns path from FS root, e.g. "/prefs/config.proto"
    const char *name() { return _s ? _s->relpath : ""; }

    // Returns the next entry in a directory.  Modifies the dir stream in _s.
    File openNextFile();

    void rewindDirectory()
    {
        if (!_s || !_s->valid || !_s->is_dir)
            return;
        // Zephyr has no rewinddir(); close + reopen the same handle. Skipping
        // the close would leak the LittleFS dir state and the next openNextFile
        // could return stale entries on some Zephyr versions.
        fs_closedir(&_s->dir);
        fs_dir_t_init(&_s->dir);
        if (fs_opendir(&_s->dir, _s->fullpath) != 0) {
            _s->valid = false;
        }
    }

    // Arduino's two-argument form; SeekMode values match FS_SEEK_*.
    bool seek(uint32_t pos, int mode)
    {
        if (!_s || !_s->valid)
            return false;
        return fs_seek(&_s->file, (off_t)pos, mode) == 0;
    }

    uint32_t position()
    {
        if (!_s || !_s->valid)
            return 0;
        const off_t p = fs_tell(&_s->file);
        return p < 0 ? 0 : (uint32_t)p;
    }

    bool seek(uint32_t pos)
    {
        if (!_s || !_s->valid || _s->is_dir)
            return false;
        return fs_seek(&_s->file, (off_t)pos, FS_SEEK_SET) == 0;
    }

    int available()
    {
        if (!_s || !_s->valid || _s->is_dir)
            return 0;
        off_t pos = fs_tell(&_s->file);
        if (pos < 0)
            return 0;
        struct fs_dirent entry;
        if (fs_stat(_s->fullpath, &entry) != 0)
            return 0;
        long rem = (long)entry.size - (long)pos;
        return rem > 0 ? (int)rem : 0;
    }

    int peek() { return -1; }

    // Internal: constructed by InternalFileSystem and openNextFile()
    explicit File(std::shared_ptr<SiFliFileState> s) : _s(std::move(s)) {}

  private:
    std::shared_ptr<SiFliFileState> _s;
};

// ── InternalFileSystem ────────────────────────────────────────────────────

class InternalFileSystem
{
  public:
    bool begin();
    File open(const char *path, const char *mode);
    bool exists(const char *path);
    bool remove(const char *path);
    bool rename(const char *from, const char *to);
    bool mkdir(const char *path);
    bool rmdir(const char *path);

    // Arduino's FS takes String as readily as const char*, and libraries
    // written against it pass one or the other interchangeably.
    // Arduino's single-argument open() defaults to reading, which is how
    // callers open a directory to iterate it.
    File open(const char *path) { return open(path, "r"); }
    File open(const String &path, const char *mode) { return open(path.c_str(), mode); }
    File open(const String &path) { return open(path.c_str(), "r"); }
    bool exists(const String &path) { return exists(path.c_str()); }
    bool remove(const String &path) { return remove(path.c_str()); }
    bool rename(const String &from, const String &to) { return rename(from.c_str(), to.c_str()); }
    bool mkdir(const String &path) { return mkdir(path.c_str()); }
    bool rmdir(const String &path) { return rmdir(path.c_str()); }
    bool rmdir_r(const char *path); // recursive delete (used by FSCommon rmDir)
    uint32_t usedBytes()
    {
        struct fs_statvfs st = {};
        if (fs_statvfs(SIFLI_FS_MOUNT, &st) != 0)
            return 0;
        // Zephyr returns block counts; convert to bytes. f_frsize is the
        // fundamental fragment size (LittleFS reports it equal to the block
        // size). used = (total - free) * frag_size.
        if (st.f_blocks <= st.f_bfree)
            return 0;
        return (uint32_t)((st.f_blocks - st.f_bfree) * st.f_frsize);
    }
    uint32_t totalBytes() { return (uint32_t)FIXED_PARTITION_SIZE(storage_partition); }
    bool format();

    // Convert a FS-root-relative path to an absolute Zephyr path.
    static void toabs(const char *rel, char *abs, size_t abssz);

  private:
    bool _mounted = false;
};

} // namespace Adafruit_LittleFS_Namespace

extern Adafruit_LittleFS_Namespace::InternalFileSystem InternalFS;
