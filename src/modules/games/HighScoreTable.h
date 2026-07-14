#pragma once

#include "configuration.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#include "DebugConfiguration.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "concurrency/LockGuard.h"
#include "gps/RTC.h"
#include "mesh/NodeDB.h"
#include <ErriezCRC32.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

/**
 * Non-templated view of a game's top-N high-score table, so the shared games UI (attract line,
 * high-score screen) and the Game interface can talk to any game's table without knowing its
 * on-disk record layout.
 */
class HighScoreTableBase
{
  public:
    static constexpr uint8_t HS_COUNT = 5;     // entries kept per game
    static constexpr uint8_t INITIALS_LEN = 3; // arcade-style initials captured per high score

    virtual ~HighScoreTableBase() = default;

    virtual uint32_t scoreAt(uint8_t i) const = 0;
    virtual const char *nameAt(uint8_t i) const = 0;
    virtual uint32_t nodeNumAt(uint8_t i) const = 0;
    virtual uint32_t scoreIdAt(uint8_t i) const = 0;

    // True if `score` would place on the sorted-descending table (peek; no mutation).
    virtual bool qualifies(uint32_t score) const = 0;
    // Insert under `initials` with the given `nodeNum` (0 == local, or a foreign node for merged
    // remote scores). `scoreId` is a per-game-session nonce for dedup (0 = generate a new one).
    // Returns the 0-based rank if it placed, else -1; isNewTop set on the #1 slot.
    virtual int insert(uint32_t score, const char *initials, uint32_t nodeNum, bool &isNewTop, uint32_t scoreId = 0) = 0;
    virtual void clear() = 0;

    virtual void load() = 0;
    virtual void save() = 0;
    virtual bool loaded() const = 0;
};

// ---------------------------------------------------------------------------
// HasScoreId trait: detects whether Entry has a `scoreId` field.
// ---------------------------------------------------------------------------
template <typename T, typename = void> struct HasScoreId : std::false_type {
};
template <typename T> struct HasScoreId<T, std::void_t<decltype(std::declval<T &>().scoreId)>> : std::true_type {
};

/**
 * Templated top-N table shared by every game. The algorithm (qualify / insert / load / save / CRC)
 * is identical across games; only the on-disk record layout differs, so `Entry` is a template
 * parameter. Each game supplies its own packed `Entry` (with `score`, `shortName[5]`, `nodeNum`,
 * `epoch` fields, in whatever byte order its existing save file used) plus a file path, magic, and
 * version -- so pre-existing save files keep loading byte-for-byte after the refactor.
 *
 * If `Entry` also has a `scoreId` field, duplicate game sessions from the same node are
 * rejected automatically (same nodeNum + same non-zero scoreId = duplicate).
 */
template <typename Entry> class HighScoreTable : public HighScoreTableBase
{
  public:
    HighScoreTable(const char *path, uint32_t magic, uint8_t version, const char *logTag)
        : path_(path), magic_(magic), version_(version), logTag_(logTag)
    {
    }

    uint32_t scoreAt(uint8_t i) const override { return entries_[i].score; }
    const char *nameAt(uint8_t i) const override { return entries_[i].shortName; }
    const Entry &entryAt(uint8_t i) const { return entries_[i]; }
    uint32_t nodeNumAt(uint8_t i) const override { return entries_[i].nodeNum; }
    uint32_t scoreIdAt(uint8_t i) const override
    {
        if constexpr (HasScoreId<Entry>::value)
            return entries_[i].scoreId;
        return 0;
    }

    bool qualifies(uint32_t score) const override
    {
        if (score == 0)
            return false;
        for (int i = 0; i < HS_COUNT; i++) {
            if (score > entries_[i].score)
                return true;
        }
        return false;
    }

    int insert(uint32_t score, const char *initials, uint32_t nodeNum, bool &isNewTop, uint32_t scoreId = 0) override
    {
        isNewTop = false;
        if (score == 0)
            return -1;

        // Dedup: if Entry has a scoreId field and the caller provided a non-zero scoreId,
        // skip any entry already carrying that (nodeNum, scoreId) pair.
        if constexpr (HasScoreId<Entry>::value) {
            if (scoreId != 0) {
                for (int i = 0; i < HS_COUNT; i++) {
                    if (entries_[i].nodeNum == nodeNum && entries_[i].scoreId == scoreId)
                        return -1;
                }
            }
        }

        int pos = -1;
        for (int i = 0; i < HS_COUNT; i++) {
            if (score > entries_[i].score) {
                pos = i;
                break;
            }
        }
        if (pos < 0)
            return -1;

        for (int i = HS_COUNT - 1; i > pos; i--)
            entries_[i] = entries_[i - 1];

        Entry &e = entries_[pos];
        memset(&e, 0, sizeof(e));
        e.score = score;
        e.nodeNum = nodeNum;
        strncpy(e.shortName, (initials && initials[0]) ? initials : owner.short_name, sizeof(e.shortName) - 1);
        e.shortName[sizeof(e.shortName) - 1] = '\0';
        e.epoch = getValidTime(RTCQualityDevice, false);

        if constexpr (HasScoreId<Entry>::value) {
            if (scoreId != 0) {
                e.scoreId = scoreId;
            } else {
                // Generate a scoreId that doesn't collide with any existing entry for this node.
                // A collision is astronomically unlikely (1 in 2^32 per attempt), but if the PRNG
                // is poorly seeded at boot two consecutive calls can return the same value. A
                // duplicate (nodeNum, scoreId) pair in the local table causes remote nodes to
                // discard the lower-ranked score as a false-positive session duplicate.
                uint32_t sid;
                do {
                    sid = static_cast<uint32_t>(random());
                    if (sid == 0)
                        sid = 1u;
                    bool collision = false;
                    for (int j = 0; j < HS_COUNT; j++) {
                        if (entries_[j].score != 0 && entries_[j].nodeNum == nodeNum && entries_[j].scoreId == sid) {
                            collision = true;
                            break;
                        }
                    }
                    if (!collision)
                        break;
                } while (true);
                e.scoreId = sid;
            }
        }

        isNewTop = (pos == 0);
        return pos;
    }

    void clear() override { memset(entries_, 0, sizeof(entries_)); }
    bool loaded() const override { return loaded_; }

    void load() override
    {
        loaded_ = true;
        memset(entries_, 0, sizeof(entries_));
#ifdef FSCom
        concurrency::LockGuard g(spiLock);
        auto f = FSCom.open(path_, FILE_O_READ);
        if (!f)
            return;
        File file;
        const bool readOk = (f.read(reinterpret_cast<uint8_t *>(&file), sizeof(file)) == sizeof(file));
        f.close();
        if (!readOk || file.magic != magic_ || file.version != version_) {
            LOG_DEBUG("%s: no valid high-score file, starting fresh", logTag_);
            return;
        }
        if (crc32Buffer(&file, offsetof(File, crc)) != file.crc) {
            LOG_WARN("%s: high-score CRC mismatch, resetting table", logTag_);
            return;
        }
        memcpy(entries_, file.entries, sizeof(entries_));
        LOG_INFO("%s: loaded high scores (top=%lu)", logTag_, static_cast<unsigned long>(entries_[0].score));
#endif
    }

    void save() override
    {
#ifdef FSCom
        {
            concurrency::LockGuard g(spiLock);
            FSCom.mkdir("/prefs");
        }
        File file;
        memset(&file, 0, sizeof(file));
        file.magic = magic_;
        file.version = version_;
        memcpy(file.entries, entries_, sizeof(entries_));
        file.crc = crc32Buffer(&file, offsetof(File, crc));

        auto sf = SafeFile(path_, true);
        const size_t written = sf.write(reinterpret_cast<uint8_t *>(&file), sizeof(file));
        if (!sf.close() || written != sizeof(file))
            LOG_WARN("%s: failed to save high scores", logTag_);
#endif
    }

  private:
    struct File {
        uint32_t magic;
        uint8_t version;
        uint8_t reserved[3];
        Entry entries[HighScoreTableBase::HS_COUNT];
        uint32_t crc;
    } __attribute__((packed));

    Entry entries_[HS_COUNT] = {};
    const char *path_;
    uint32_t magic_;
    uint8_t version_;
    const char *logTag_;
    bool loaded_ = false;
};

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
