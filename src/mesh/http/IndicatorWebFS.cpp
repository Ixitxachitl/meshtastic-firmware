#include "IndicatorWebFS.h"

#if defined(SENSECAP_INDICATOR) && defined(ARCH_ESP32)

#include "mesh/IndicatorRemoteFS.h"

#include <FSImpl.h>
#include <algorithm>
#include <cerrno>
#include <memory>
#include <string.h>
#include <string>

using fs::FileImplPtr;

namespace
{

constexpr size_t kChunk = sizeof(meshtastic_FileTransfer_filedata_t::bytes);
constexpr size_t kPageEntries =
    sizeof(meshtastic_DirectoryListing::filenames) / sizeof(meshtastic_DirectoryListing::filenames[0]);

// The web server's own link instance: its buffers are several KB, and the UI task keeps its own.
IndicatorRemoteFS &remote()
{
    static IndicatorRemoteFS *fs = new IndicatorRemoteFS();
    return *fs;
}

enum class Kind { Missing, File, Dir, Unknown };

// What path is, from a one-byte read: a file answers with its size, a directory refuses as not a file.
Kind stat(const char *path, uint64_t &size)
{
    uint8_t probe;
    uint32_t got = 0, fileSize = 0;
    size = 0;
    if (remote().readChunk(path, 0, &probe, 1, &got, &fileSize)) {
        size = fileSize;
        return Kind::File;
    }
    switch (remote().lastFileStatus()) {
    case meshtastic_FileStatus_FILE_NOT_A_FILE:
        return Kind::Dir;
    case meshtastic_FileStatus_FILE_NOT_FOUND:
    case meshtastic_FileStatus_FILE_NO_CARD:
        return Kind::Missing;
    default:
        return Kind::Unknown;
    }
}

class RemoteFile : public fs::FileImpl
{
  public:
    // A directory; or a file read from the start, or written. A written file appends from `size`, or is made anew by its
    // first chunk when `create` - an empty one by close() if nothing was written.
    RemoteFile(std::string path, uint64_t size, bool isDir, bool writable, bool create)
        : path_(std::move(path)), size_(size), committed_(size), isDir_(isDir), writable_(writable), create_(create)
    {
        const size_t slash = path_.find_last_of('/');
        name_ = slash == std::string::npos ? path_ : path_.substr(slash + 1);
        // Only what this one needs: listing a folder opens an entry object for every file in it.
        if (writable_)
            buf_.reset(new uint8_t[kChunk]);
        if (isDir_)
            page_.reset(new Page());
    }
    ~RemoteFile() override { close(); }

    size_t write(const uint8_t *buf, size_t n) override
    {
        if (!writable_ || failed_ || closed_)
            return 0;
        size_t done = 0;
        // Writes only append. Bytes below what is already on the card are a slice sent again, taken as written.
        if (pos_ < committed_) {
            done = (size_t)std::min<uint64_t>(n, committed_ - pos_);
            pos_ += done;
        }
        if (done < n && pos_ != committed_ + bufLen_)
            return done; // past the end: the card cannot hold a gap
        while (done < n) {
            const size_t take = std::min(n - done, kChunk - bufLen_);
            memcpy(buf_.get() + bufLen_, buf + done, take);
            bufLen_ += take;
            done += take;
            pos_ += take;
            if (bufLen_ == kChunk && !push())
                return done - kChunk; // that chunk never reached the card
        }
        return n;
    }

    size_t read(uint8_t *buf, size_t n) override
    {
        if (isDir_ || closed_ || (writable_ && !push()))
            return 0;
        size_t done = 0;
        while (done < n && pos_ < committed_) {
            const uint32_t want = (uint32_t)std::min<uint64_t>({(uint64_t)(n - done), (uint64_t)kChunk, committed_ - pos_});
            uint32_t got = 0, fileSize = 0;
            if (!remote().readChunk(path_.c_str(), (uint32_t)pos_, buf + done, want, &got, &fileSize) || got == 0)
                break;
            done += got;
            pos_ += got;
        }
        return done;
    }

    void flush() override { push(); }

    bool seek(uint32_t pos, fs::SeekMode mode) override
    {
        if (isDir_ || !push())
            return false;
        uint64_t to = pos;
        if (mode == fs::SeekCur)
            to = pos_ + pos;
        else if (mode == fs::SeekEnd)
            to = committed_ + pos;
        if (to > committed_)
            return false;
        pos_ = to;
        return true;
    }

    size_t position() const override { return (size_t)pos_; }
    size_t size() const override { return (size_t)(writable_ ? committed_ + bufLen_ : size_); }
    bool setBufferSize(size_t) override { return true; }

    void close() override
    {
        if (closed_)
            return;
        push();
        if (writable_ && create_ && !created_ && !failed_) // nothing was written: still make the file
            remote().writeChunk(path_.c_str(), 0, buf_.get(), 0, true);
        closed_ = true;
    }

    time_t getLastWrite() override { return 0; }
    const char *path() const override { return path_.c_str(); }
    const char *name() const override { return name_.c_str(); }
    boolean isDirectory() override { return isDir_; }

    FileImplPtr openNextFile(const char *) override
    {
        std::string child;
        uint64_t size = 0;
        bool dir = false;
        if (!nextEntry(child, size, dir))
            return FileImplPtr();
        return std::make_shared<RemoteFile>(child, size, dir, false, false);
    }

    boolean seekDir(long position) override
    {
        if (!page_)
            return false;
        rewindDirectory();
        page_->offset = position < 0 ? 0 : (uint32_t)position;
        return true;
    }

    String getNextFileName() override
    {
        bool dir;
        return getNextFileName(&dir);
    }

    String getNextFileName(bool *isDir) override
    {
        std::string child;
        uint64_t size = 0;
        bool dir = false;
        if (!nextEntry(child, size, dir))
            return String();
        if (isDir)
            *isDir = dir;
        return String(child.c_str());
    }

    void rewindDirectory() override
    {
        if (!page_)
            return;
        page_->offset = 0;
        page_->index = page_->count = 0;
        page_->listed = false;
    }

    operator bool() override { return !closed_; }

  private:
    // Sends what is buffered as the next chunk. The first chunk of a new file makes it, parent folders included.
    bool push()
    {
        if (!writable_ || failed_ || bufLen_ == 0)
            return !failed_;
        const bool first = create_ && !created_;
        if (!remote().writeChunk(path_.c_str(), (uint32_t)committed_, buf_.get(), (uint32_t)bufLen_, first)) {
            failed_ = true;
            return false;
        }
        committed_ += bufLen_;
        bufLen_ = 0;
        created_ = true;
        return true;
    }

    // The next entry of this directory, a page of the listing at a time.
    bool nextEntry(std::string &child, uint64_t &size, bool &dir)
    {
        if (!page_ || closed_)
            return false;
        Page &p = *page_;
        if (p.index >= p.count) {
            if (p.listed && p.offset >= p.total)
                return false;
            const meshtastic_DirectoryListing *page = remote().listPage(path_.c_str(), p.offset);
            if (!page || page->filenames_count == 0)
                return false;
            p.count = std::min<size_t>(page->filenames_count, kPageEntries);
            for (size_t i = 0; i < p.count; i++) {
                p.names[i] = page->filenames[i];
                p.sizes[i] = i < page->sizes_count ? page->sizes[i] : 0; // older co-processor firmware sends none
            }
            p.total = page->total_count;
            p.offset += p.count;
            p.index = 0;
            p.listed = true;
        }
        std::string name = p.names[p.index];
        size = p.sizes[p.index];
        p.index++;
        dir = !name.empty() && name.back() == '/';
        if (dir)
            name.pop_back();
        child = path_;
        if (child.empty() || child.back() != '/')
            child += '/';
        child += name;
        return true;
    }

    std::string path_, name_;
    uint64_t size_;      // as found when opened; what a reader serves
    uint64_t committed_; // what is on the card
    uint64_t pos_ = 0;
    bool isDir_, writable_, create_;
    bool created_ = false, failed_ = false, closed_ = false;
    std::unique_ptr<uint8_t[]> buf_; // writes collect here up to a chunk
    size_t bufLen_ = 0;

    // Directory iteration: one page of the listing at a time.
    struct Page {
        std::string names[kPageEntries];
        uint64_t sizes[kPageEntries] = {};
        size_t index = 0, count = 0;
        uint32_t offset = 0, total = 0;
        bool listed = false;
    };
    std::unique_ptr<Page> page_;
};

class RemoteFS : public fs::FSImpl
{
  public:
    FileImplPtr open(const char *path, const char *mode, const bool) override
    {
        errno = 0;
        if (mode[0] == 'w') // made by the first chunk written, or by close()
            return std::make_shared<RemoteFile>(path, 0, false, true, true);
        uint64_t size = 0;
        const Kind kind = stat(path, size);
        if (kind == Kind::Missing && mode[0] != 'a') {
            errno = ENOENT;
            return FileImplPtr();
        }
        if (kind == Kind::Unknown || (kind == Kind::Dir && mode[0] != 'r')) {
            errno = EIO;
            return FileImplPtr();
        }
        if (kind == Kind::Dir)
            return std::make_shared<RemoteFile>(path, 0, true, false, false);
        if (mode[0] == 'a')
            return std::make_shared<RemoteFile>(path, size, false, true, kind == Kind::Missing);
        return std::make_shared<RemoteFile>(path, size, false, strchr(mode, '+') != nullptr, false);
    }

    bool exists(const char *path) override
    {
        uint64_t size;
        const Kind kind = stat(path, size);
        return kind == Kind::File || kind == Kind::Dir;
    }

    bool rename(const char *from, const char *to) override { return remote().rename(from, to); }
    // A file already gone reports OK, as the co-processor's delete is idempotent.
    bool remove(const char *path) override { return remote().remove(path); }
    bool mkdir(const char *path) override { return remote().mkdir(path); }
    bool rmdir(const char *path) override { return remote().remove(path); } // only an empty one
};

} // namespace

IndicatorWebSD::IndicatorWebSD() : fs::FS(std::make_shared<RemoteFS>()) {}

uint64_t IndicatorWebSD::totalBytes()
{
    meshtastic_SdCardInfo info = meshtastic_SdCardInfo_init_zero;
    if (!sensecapIndicator || !sensecapIndicator->sd_info(&info) || !info.present)
        return 0;
    return info.stats_valid ? info.used_bytes + info.free_bytes : info.card_size;
}

uint64_t IndicatorWebSD::usedBytes()
{
    meshtastic_SdCardInfo info = meshtastic_SdCardInfo_init_zero;
    if (!sensecapIndicator || !sensecapIndicator->sd_info(&info) || !info.present || !info.stats_valid)
        return 0;
    return info.used_bytes;
}

IndicatorWebSD indicatorWebSD;

#endif
