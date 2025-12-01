#include "configuration.h"
#if HAS_SCREEN
#include "FSCommon.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "gps/RTC.h"
#include "graphics/draw/MessageRenderer.h"
#include <algorithm>
#include <cstdint>
#include <cstring> // memcpy

#ifndef MESSAGE_TEXT_POOL_SIZE
#define MESSAGE_TEXT_POOL_SIZE (MAX_MESSAGES_SAVED * MAX_MESSAGE_SIZE)
#endif

// Global message text pool and state
static char *g_messagePool = nullptr;
static size_t g_poolWritePos = 0;

static inline const char *getTextFromPool(uint16_t offset);

// Debug helper: Log current pool utilization
static inline void logPoolUtilization(const char *context)
{
    if (!g_messagePool)
        return;

    float utilization = (float)g_poolWritePos / MESSAGE_TEXT_POOL_SIZE * 100.0f;
    LOG_DEBUG("MessageStore: Pool utilization at %s: %zu/%d bytes (%.1f%%)", context, g_poolWritePos, MESSAGE_TEXT_POOL_SIZE,
              utilization);

    if (utilization > 90.0f) {
        LOG_WARN("MessageStore: Pool utilization high (%.1f%%) - consider compaction", utilization);
    }
}

static volatile bool g_persistDirty = false;

bool MessageStore::isPersistDirty()
{
    return g_persistDirty;
}
void MessageStore::markPersistDirty()
{
    g_persistDirty = true;
}
void MessageStore::markPersistClean()
{
    g_persistDirty = false;
}

void MessageStore::compactPoolLossless()
{
    logPoolUtilization("before compaction");

    // Allocate a fresh pool; if this fails, leave the old pool intact.
    char *newPool = static_cast<char *>(std::malloc(MESSAGE_TEXT_POOL_SIZE));
    if (!newPool) {
        LOG_ERROR("MessageStore: Failed to allocate memory for pool compaction");
        return;
    }

    size_t wp = 0;
    for (auto &m : liveMessages) {
        const char *txt = getTextFromPool(m.textOffset);
        if (!txt) {
            continue;
        }
        size_t len = std::min<size_t>(m.textLength, /*safety cap*/ 0xFFFF);
        // In case lengths drifted, re-derive from '\0' up to previous cap:
        size_t safeLen = std::min(len, std::strlen(txt));
        if (wp + safeLen + 1 > MESSAGE_TEXT_POOL_SIZE)
            break; // should not happen
        std::memcpy(&newPool[wp], txt, safeLen);
        newPool[wp + safeLen] = '\0';
        m.textOffset = static_cast<uint16_t>(wp);
        m.textLength = static_cast<uint16_t>(safeLen);
        wp += safeLen + 1;
    }

    // Swap pools (free old after successful copy)
    if (g_messagePool)
        std::free(g_messagePool);
    g_messagePool = newPool;
    g_poolWritePos = wp;

    logPoolUtilization("after compaction");
    LOG_INFO("MessageStore: Pool compaction completed, reclaimed %zu bytes", MESSAGE_TEXT_POOL_SIZE - wp);
}

// Reset pool (called on boot or clear)
static inline void resetMessagePool()
{
    if (!g_messagePool) {
        g_messagePool = static_cast<char *>(malloc(MESSAGE_TEXT_POOL_SIZE));
        if (!g_messagePool) {
            LOG_ERROR("MessageStore: Failed to allocate %d bytes for message pool", MESSAGE_TEXT_POOL_SIZE);
            return;
        }
    }
    g_poolWritePos = 0;
    memset(g_messagePool, 0, MESSAGE_TEXT_POOL_SIZE);
}

// Allocate text in pool and return offset
// If not enough space remains, compact pool to reclaim space
static inline uint16_t storeTextInPool(const char *src, size_t len)
{
    if (len >= MAX_MESSAGE_SIZE)
        len = MAX_MESSAGE_SIZE - 1;

    // If not enough space remains, try compacting first
    if (g_poolWritePos + len + 1 >= MESSAGE_TEXT_POOL_SIZE) {
        LOG_DEBUG("MessageStore: Pool nearly full (%zu/%d), attempting compaction", g_poolWritePos, MESSAGE_TEXT_POOL_SIZE);
        messageStore.compactPoolLossless();

        // After compaction, if still no space, we have a real problem
        if (g_poolWritePos + len + 1 >= MESSAGE_TEXT_POOL_SIZE) {
            LOG_WARN("MessageStore: Pool full even after compaction (%zu/%d), forcibly wrapping", g_poolWritePos,
                     MESSAGE_TEXT_POOL_SIZE);
            g_poolWritePos = 0; // Last resort - this may corrupt old messages
        }
    }

    uint16_t offset = g_poolWritePos;
    memcpy(&g_messagePool[g_poolWritePos], src, len);
    g_messagePool[g_poolWritePos + len] = '\0';
    g_poolWritePos += (len + 1);
    MessageStore::markPersistDirty();

    return offset;
}

// Retrieve a const pointer to message text by offset
static inline const char *getTextFromPool(uint16_t offset)
{
    if (!g_messagePool || offset >= MESSAGE_TEXT_POOL_SIZE) {
        LOG_WARN("MessageStore: Invalid text offset %u (pool size %d)", offset, MESSAGE_TEXT_POOL_SIZE);
        return "";
    }
    return &g_messagePool[offset];
}

// Helper: assign a timestamp (RTC if available, else boot-relative)
static inline void assignTimestamp(StoredMessage &sm)
{
    uint32_t nowSecs = getValidTime(RTCQuality::RTCQualityDevice, true);
    if (nowSecs) {
        sm.timestamp = nowSecs;
        sm.isBootRelative = false;
    } else {
        sm.timestamp = millis() / 1000;
        sm.isBootRelative = true;
    }
}

// Generic push with cap (used by live + persisted queues)
template <typename T> static inline void pushWithLimit(std::deque<T> &queue, const T &msg)
{
    if (queue.size() >= MAX_MESSAGES_SAVED) {
        queue.pop_front();
    }
    queue.push_back(msg);
}

template <typename T> static inline void pushWithLimit(std::deque<T> &queue, T &&msg)
{
    if (queue.size() >= MAX_MESSAGES_SAVED) {
        queue.pop_front();
    }
    queue.emplace_back(std::move(msg));
}

MessageStore::MessageStore(const std::string &label)
{
    filename = "/Messages_" + label + ".msgs";
    resetMessagePool(); // initialize text pool on boot
}

// Live message handling (RAM only)
void MessageStore::addLiveMessage(StoredMessage &&msg)
{
    pushWithLimit(liveMessages, std::move(msg));
    MessageStore::markPersistDirty();
}
void MessageStore::addLiveMessage(const StoredMessage &msg)
{
    pushWithLimit(liveMessages, msg);
    MessageStore::markPersistDirty();
}

// Add from incoming/outgoing packet
const StoredMessage &MessageStore::addFromPacket(const meshtastic_MeshPacket &packet)
{
    StoredMessage sm;
    assignTimestamp(sm);
    sm.channelIndex = packet.channel;

    const char *payload = reinterpret_cast<const char *>(packet.decoded.payload.bytes);
    size_t len = strnlen(payload, MAX_MESSAGE_SIZE - 1);
    sm.textOffset = storeTextInPool(payload, len);
    sm.textLength = len;

    // Determine sender
    uint32_t localNode = nodeDB->getNodeNum();
    sm.sender = (packet.from == 0) ? localNode : packet.from;

    sm.dest = packet.to;

    bool isDM = (sm.dest != 0 && sm.dest != NODENUM_BROADCAST);

    if (packet.from == 0) {
        sm.type = isDM ? MessageType::DM_TO_US : MessageType::BROADCAST;
        sm.ackStatus = AckStatus::NONE;
    } else {
        sm.type = isDM ? MessageType::DM_TO_US : MessageType::BROADCAST;
        sm.ackStatus = AckStatus::ACKED;
    }

    addLiveMessage(sm);
    return liveMessages.back();
}

// Outgoing/manual message
void MessageStore::addFromString(uint32_t sender, uint8_t channelIndex, const std::string &text)
{
    StoredMessage sm;

    // Always use our local time (helper handles RTC vs boot time)
    assignTimestamp(sm);

    sm.sender = sender;
    sm.channelIndex = channelIndex;
    sm.textOffset = storeTextInPool(text.c_str(), text.size());
    sm.textLength = text.size();

    // Use the provided destination
    sm.dest = sender;
    sm.type = MessageType::DM_TO_US;

    // Outgoing messages always start with unknown ack status
    sm.ackStatus = AckStatus::NONE;

    addLiveMessage(sm);
}

#if ENABLE_MESSAGE_PERSISTENCE

// Compact, fixed-size on-flash representation using offset + length
struct __attribute__((packed)) StoredMessageRecord {
    uint32_t timestamp;
    uint32_t sender;
    uint8_t channelIndex;
    uint32_t dest;
    uint8_t isBootRelative;
    uint8_t ackStatus;           // static_cast<uint8_t>(AckStatus)
    uint8_t type;                // static_cast<uint8_t>(MessageType)
    uint16_t textLength;         // message length
    char text[MAX_MESSAGE_SIZE]; // store actual text here
};

// Serialize one StoredMessage to flash
static inline void writeMessageRecord(SafeFile &f, const StoredMessage &m)
{
    StoredMessageRecord rec = {};
    rec.timestamp = m.timestamp;
    rec.sender = m.sender;
    rec.channelIndex = m.channelIndex;
    rec.dest = m.dest;
    rec.isBootRelative = m.isBootRelative;
    rec.ackStatus = static_cast<uint8_t>(m.ackStatus);
    rec.type = static_cast<uint8_t>(m.type);
    rec.textLength = m.textLength;

    // Copy the actual text into the record from RAM pool
    const char *txt = getTextFromPool(m.textOffset);
    strncpy(rec.text, txt, MAX_MESSAGE_SIZE - 1);
    rec.text[MAX_MESSAGE_SIZE - 1] = '\0';

    f.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec));
}

// Directly allocate text in pool without going through storeTextInPool to avoid double allocation
static inline uint16_t directAllocateInPool(const char *src, size_t len)
{
    if (len >= MAX_MESSAGE_SIZE)
        len = MAX_MESSAGE_SIZE - 1;

    // Simple wrap if out of space during loading (no compaction to avoid heap churn)
    if (g_poolWritePos + len + 1 >= MESSAGE_TEXT_POOL_SIZE) {
        LOG_WARN("MessageStore: Pool full during loading (%zu/%d), wrapping", g_poolWritePos, MESSAGE_TEXT_POOL_SIZE);
        g_poolWritePos = 0;
    }

    uint16_t offset = g_poolWritePos;
    memcpy(&g_messagePool[g_poolWritePos], src, len);
    g_messagePool[g_poolWritePos + len] = '\0';
    g_poolWritePos += (len + 1);
    // Note: Don't mark persist dirty here since we're loading, not modifying
    return offset;
}

// Deserialize one StoredMessage from flash; returns false on short read
static inline bool readMessageRecord(File &f, StoredMessage &m)
{
    StoredMessageRecord rec = {};
    if (f.readBytes(reinterpret_cast<char *>(&rec), sizeof(rec)) != sizeof(rec))
        return false;

    m.timestamp = rec.timestamp;
    m.sender = rec.sender;
    m.channelIndex = rec.channelIndex;
    m.dest = rec.dest;
    m.isBootRelative = rec.isBootRelative;
    m.ackStatus = static_cast<AckStatus>(rec.ackStatus);
    m.type = static_cast<MessageType>(rec.type);
    m.textLength = rec.textLength;

    // Directly allocate in pool to avoid duplicate allocation overhead
    m.textLength = strnlen(rec.text, MAX_MESSAGE_SIZE - 1);
    m.textOffset = directAllocateInPool(rec.text, m.textLength);

    return true;
}

void MessageStore::saveToFlash()
{
#ifdef FSCom
    // Ensure root exists
    spiLock->lock();
    FSCom.mkdir("/");
    spiLock->unlock();

    SafeFile f(filename.c_str(), false);

    spiLock->lock();
    uint8_t count = static_cast<uint8_t>(liveMessages.size());
    if (count > MAX_MESSAGES_SAVED)
        count = MAX_MESSAGES_SAVED;
    f.write(&count, 1);

    for (uint8_t i = 0; i < count; ++i) {
        writeMessageRecord(f, liveMessages[i]);
    }
    spiLock->unlock();

    f.close();
    MessageStore::markPersistClean();
#endif
}

void MessageStore::loadFromFlash()
{
    std::deque<StoredMessage>().swap(liveMessages);
    resetMessagePool(); // reset pool when loading

#ifdef FSCom
    concurrency::LockGuard guard(spiLock);

    if (!FSCom.exists(filename.c_str()))
        return;

    auto f = FSCom.open(filename.c_str(), FILE_O_READ);
    if (!f)
        return;

    uint8_t count = 0;
    f.readBytes(reinterpret_cast<char *>(&count), 1);
    if (count > MAX_MESSAGES_SAVED)
        count = MAX_MESSAGES_SAVED;

    for (uint8_t i = 0; i < count; ++i) {
        StoredMessage m;
        if (!readMessageRecord(f, m))
            break;
        liveMessages.push_back(m);
    }

    f.close();
#endif
    MessageStore::markPersistClean();
}

#else
// If persistence is disabled, these functions become no-ops
void MessageStore::saveToFlash() {}
void MessageStore::loadFromFlash() {}
#endif

// Clear all messages (RAM + persisted queue)
void MessageStore::clearAllMessages()
{
    std::deque<StoredMessage>().swap(liveMessages);
    resetMessagePool();
    MessageStore::markPersistClean();

#ifdef FSCom
    SafeFile f(filename.c_str(), false);
    uint8_t count = 0;
    f.write(&count, 1); // write "0 messages"
    f.close();
#endif
}

// Internal helper: erase first or last message matching a predicate
template <typename Predicate> static void eraseIf(std::deque<StoredMessage> &deque, Predicate pred, bool fromBack = false)
{
    if (fromBack) {
        // Iterate from the back and erase the first match from the end
        for (auto it = deque.rbegin(); it != deque.rend(); ++it) {
            if (pred(*it)) {
                deque.erase(std::next(it).base());
                break;
            }
        }
    } else {
        // Manual forward search to avoid std::find_if
        auto it = deque.begin();
        for (; it != deque.end(); ++it) {
            if (pred(*it))
                break;
        }
        if (it != deque.end())
            deque.erase(it);
    }
}

// Delete oldest message (RAM + persisted queue)
void MessageStore::deleteOldestMessage()
{
    eraseIf(liveMessages, [](StoredMessage &) { return true; });
    saveToFlash();
}

// Delete oldest message in a specific channel
void MessageStore::deleteOldestMessageInChannel(uint8_t channel)
{
    auto pred = [channel](const StoredMessage &m) { return m.type == MessageType::BROADCAST && m.channelIndex == channel; };
    eraseIf(liveMessages, pred);
    saveToFlash();
}

void MessageStore::deleteAllMessagesInChannel(uint8_t channel)
{
    auto pred = [channel](const StoredMessage &m) { return m.type == MessageType::BROADCAST && m.channelIndex == channel; };
    eraseIf(liveMessages, pred, false /* delete ALL, not just first */);
    saveToFlash();
}

void MessageStore::deleteAllMessagesWithPeer(uint32_t peer)
{
    uint32_t local = nodeDB->getNodeNum();
    auto pred = [&](const StoredMessage &m) {
        if (m.type != MessageType::DM_TO_US)
            return false;
        uint32_t other = (m.sender == local) ? m.dest : m.sender;
        return other == peer;
    };
    eraseIf(liveMessages, pred, false);
    saveToFlash();
}

// Delete oldest message in a direct chat with a node
void MessageStore::deleteOldestMessageWithPeer(uint32_t peer)
{
    auto pred = [peer](const StoredMessage &m) {
        if (m.type != MessageType::DM_TO_US)
            return false;
        uint32_t other = (m.sender == nodeDB->getNodeNum()) ? m.dest : m.sender;
        return other == peer;
    };
    eraseIf(liveMessages, pred);
    saveToFlash();
}

std::deque<StoredMessage> MessageStore::getChannelMessages(uint8_t channel) const
{
    std::deque<StoredMessage> result;
    for (const auto &m : liveMessages) {
        if (m.type == MessageType::BROADCAST && m.channelIndex == channel) {
            result.push_back(m);
        }
    }
    return result;
}

std::deque<StoredMessage> MessageStore::getDirectMessages() const
{
    std::deque<StoredMessage> result;
    for (const auto &m : liveMessages) {
        if (m.type == MessageType::DM_TO_US) {
            result.push_back(m);
        }
    }
    return result;
}

// Upgrade boot-relative timestamps once RTC is valid
// Only same-boot boot-relative messages are healed.
// Persisted boot-relative messages from old boots stay ??? forever.
void MessageStore::upgradeBootRelativeTimestamps()
{
    uint32_t nowSecs = getValidTime(RTCQuality::RTCQualityDevice, true);
    if (nowSecs == 0)
        return; // Still no valid RTC

    uint32_t bootNow = millis() / 1000;

    auto fix = [&](std::deque<StoredMessage> &dq) {
        for (auto &m : dq) {
            if (m.isBootRelative && m.timestamp <= bootNow) {
                uint32_t bootOffset = nowSecs - bootNow;
                m.timestamp += bootOffset;
                m.isBootRelative = false;
            }
        }
    };
    fix(liveMessages);
}

const char *MessageStore::getText(const StoredMessage &msg)
{
    // Wrapper around the internal helper
    return getTextFromPool(msg.textOffset);
}

uint16_t MessageStore::storeText(const char *src, size_t len)
{
    // Wrapper around the internal helper
    return storeTextInPool(src, len);
}

// Global definition
MessageStore messageStore("default");
#endif