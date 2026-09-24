#pragma once

#include "configuration.h"

#if defined(SENSECAP_INDICATOR)

// Where device-ui is linked (HAS_TFT) this is its IRemoteFS backend, card management included. BaseUI-only
// builds get the same file operations as a standalone class.
#if HAS_TFT
#include "graphics/map/RemoteSDService.h"
#define INDICATOR_FS_OVERRIDE override
#else
#define INDICATOR_FS_OVERRIDE
#endif
#include "mesh/IndicatorSerial.h"

#include <set>
#include <string.h>
#include <string>

// Serves the UI map tiles from the SD card behind the RP2040, chunk-wise
// over the interdevice link.
//
// Retry policy: a lost frame (timeout) and a co-processor busy with card
// maintenance (FILE_BUSY) are transient, so they are retried; a request the
// co-processor refused (nack) or answered definitively (missing file, IO
// error) is not. Correlation ids make retrying safe, a late response to the
// first attempt is dropped as stale.
class IndicatorRemoteFS
#if HAS_TFT
    : public IRemoteFS
#endif
{
    // A lost frame is retried a couple of times. A co-processor busy with
    // card maintenance is a different story: mounting a card takes seconds,
    // and the free space scan of a large card walks its whole FAT, so wait
    // that out rather than reporting a missing tile.
    static const int LINK_ATTEMPTS = 3;
    static const int BUSY_ATTEMPTS = 20;
    // card state is answered from a cache, so it is only ever busy across a
    // mount: a much shorter wait than a file operation has to sit out
    static const int INFO_BUSY_ATTEMPTS = 10;
    static const uint32_t BUSY_BACKOFF_MS = 250;

    // Both budgets are separate and only ever count down: a co-processor that
    // stays busy must not be able to keep a caller here for ever. This runs on
    // the UI task, an unbounded wait is a frozen screen.
    struct Budget {
        int attempts = LINK_ATTEMPTS;
        int busy = BUSY_ATTEMPTS;
    };

    // Returns true when the request should be sent again. `answered` is
    // false when the link itself failed (timeout), true when the
    // co-processor replied.
    static bool retryable(bool answered, meshtastic_FileStatus status, Budget &budget)
    {
        if (!answered) {
            if (--budget.attempts <= 0)
                return false;
            return !sensecapIndicator->last_request_nacked(); // refused, not lost
        }
        if (status == meshtastic_FileStatus_FILE_BUSY) {
            if (--budget.busy <= 0)
                return false;
            delay(BUSY_BACKOFF_MS);
            return true;
        }
        return false;
    }

  public:
    IndicatorRemoteFS() : result(meshtastic_FileTransfer_init_zero), listing(meshtastic_DirectoryListing_init_zero) {}

    bool readChunk(const char *path, uint32_t offset, uint8_t *buf, uint32_t len, uint32_t *bytesRead,
                   uint32_t *fileSize) INDICATOR_FS_OVERRIDE
    {
        if (!sensecapIndicator)
            return false;
        Budget budget;
        do {
            memset(&result, 0, sizeof(result));
            bool answered = sensecapIndicator->file_read(path, offset, len, &result);
            if (answered && result.status == meshtastic_FileStatus_FILE_OK) {
                uint32_t n = result.filedata.size;
                if (n > len)
                    n = len;
                memcpy(buf, result.filedata.bytes, n);
                *bytesRead = n;
                // lv_fs positions are 32 bit, map tiles never come close
                *fileSize = (uint32_t)result.file_size;
                return true;
            }
            if (!retryable(answered, result.status, budget))
                return false;
        } while (true);
    }

    bool writeChunk(const char *path, uint32_t offset, const uint8_t *buf, uint32_t len, bool create) INDICATOR_FS_OVERRIDE
    {
        if (!sensecapIndicator)
            return false;
        Budget budget;
        bool retried = false;
        do {
            memset(&result, 0, sizeof(result));
            bool answered = sensecapIndicator->file_write(path, offset, buf, len, create, &result);
            if (answered) {
                if (result.status == meshtastic_FileStatus_FILE_OK)
                    return true;
                // An append whose first attempt landed but whose response was
                // lost is refused as an offset conflict, and the file already
                // holds this chunk: that is the outcome we wanted
                if (retried && !create && result.status == meshtastic_FileStatus_FILE_OFFSET_CONFLICT &&
                    result.file_size == (uint64_t)offset + len)
                    return true;
            }
            if (!retryable(answered, result.status, budget))
                return false;
            retried = true;
        } while (true);
    }

#if HAS_TFT
    bool sdInfo(RemoteSdInfo &info) override
    {
        if (!sensecapIndicator)
            return false;
        meshtastic_SdCardInfo sdState = meshtastic_SdCardInfo_init_zero;
        // A mount takes up to two seconds. Waiting it out here is worth it (an
        // empty slot reported once sticks in the UI), but this runs on the UI
        // task, so the wait is bounded well below what a user would call a
        // hang, and a card that stays busy longer is simply asked again later.
        Budget budget;
        budget.busy = INFO_BUSY_ATTEMPTS; // a mount, not a whole FAT scan
        while (true) {
            sdState = meshtastic_SdCardInfo_init_zero;
            bool answered = sensecapIndicator->sd_info(&sdState);
            if (answered && !sdState.busy)
                break;
            meshtastic_FileStatus status = answered ? meshtastic_FileStatus_FILE_BUSY : meshtastic_FileStatus_FILE_UNSPECIFIED;
            if (!retryable(answered, status, budget))
                return false;
        }
        info.present = sdState.present;
        info.cardType = (uint8_t)sdState.card_type;
        info.fatType = (uint8_t)sdState.fat_type;
        info.cardSize = sdState.card_size;
        info.usedBytes = sdState.used_bytes;
        info.freeBytes = sdState.free_bytes;
        info.statsValid = sdState.stats_valid;
        info.unformatted = sdState.unformatted;
        return true;
    }

    bool sdEject(void) override { return sdCommand(meshtastic_SdCommand_SD_EJECT); }
    bool sdMount(void) override { return sdCommand(meshtastic_SdCommand_SD_MOUNT); }
    bool sdFormat(void) override
    {
        // wiping the card takes seconds; the co-processor answers right away
        // and mounts the fresh filesystem afterwards
        return sdCommand(meshtastic_SdCommand_SD_FORMAT);
    }
#endif

    bool remove(const char *path) INDICATOR_FS_OVERRIDE
    {
        if (!sensecapIndicator)
            return false;
        Budget budget;
        do {
            memset(&result, 0, sizeof(result));
            bool answered = sensecapIndicator->file_remove(path, &result);
            // delete is idempotent on the co-processor: a file that is
            // already gone reports OK, so a retry after a lost response does
            // not have to be guessed at here
            if (answered && result.status == meshtastic_FileStatus_FILE_OK)
                return true;
            if (!retryable(answered, result.status, budget))
                return false;
        } while (true);
    }

    bool listDir(const char *path, std::set<std::string> &entries) INDICATOR_FS_OVERRIDE
    {
        if (!sensecapIndicator)
            return false;
        uint32_t offset = 0;
        while (true) {
            bool got_page = false;
            Budget budget;
            while (!got_page) {
                memset(&listing, 0, sizeof(listing));
                bool answered = sensecapIndicator->list_directory(path, offset, &listing);
                if (answered && listing.status == meshtastic_FileStatus_FILE_OK)
                    got_page = true;
                else if (!retryable(answered, listing.status, budget))
                    return false;
            }
            if (!got_page)
                return false;
            for (pb_size_t i = 0; i < listing.filenames_count; i++)
                entries.insert(listing.filenames[i]);
            offset += listing.filenames_count;
            if (listing.filenames_count == 0 || offset >= listing.total_count)
                break;
        }
        return true;
    }

    // Outcome of the last file operation, which the bool results fold together: FILE_NOT_FOUND
    // (or FILE_NO_CARD) against FILE_UNSPECIFIED for a link that never answered.
    meshtastic_FileStatus lastFileStatus() const { return result.status; }

  private:
    bool sdCommand(meshtastic_SdCommand command)
    {
        if (!sensecapIndicator)
            return false;
        meshtastic_SdCardInfo state = meshtastic_SdCardInfo_init_zero;
        return sensecapIndicator->sd_command(command, &state);
    }

    // Several KB each, kept off the calling task's stack. Not shared between
    // tasks: each task that touches the card holds its own instance.
    meshtastic_FileTransfer result;
    meshtastic_DirectoryListing listing;
};

#undef INDICATOR_FS_OVERRIDE

#endif
