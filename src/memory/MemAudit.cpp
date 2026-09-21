#include "MemAudit.h"

#if MESHTASTIC_MEM_AUDIT

#include "DebugConfiguration.h"
#include <atomic>
#include <stdio.h>
#include <string.h>
#if defined(ARCH_ESP32)
#include <esp_heap_caps.h>    // heap_caps_print_heap_info(), for the per-region dump
#include <esp_memory_utils.h> // esp_ptr_external_ram(), to tell PSRAM from internal DRAM
#endif

namespace memaudit
{

namespace
{

struct Entry {
    std::atomic<const char *> tag; // registered literal; nullptr = free slot
    std::atomic<int32_t> bytes;
    std::atomic<int32_t> psramBytes; // the part of bytes that came from PSRAM
};

// Static storage only - the accounting registry must never itself allocate.
// Zero-initialized (BSS), so it is usable from constructors of static objects.
Entry table[kMaxTags];

// Find the slot for a tag, registering it on first use.
// Returns nullptr for a null tag or when the table is full (update dropped).
Entry *findOrRegister(const char *tag)
{
    if (!tag)
        return nullptr;

    // Fast path: same literal, pointer compare only. This is all the hot
    // per-packet add() ever executes once the tag is registered.
    size_t used = 0;
    for (; used < kMaxTags; used++) {
        const char *cur = table[used].tag.load(std::memory_order_acquire);
        if (!cur)
            break; // slots fill in order - first empty slot ends the table
        if (cur == tag)
            return &table[used];
    }

    // Slow path: same text from a different literal (duplicated across
    // translation units, so not pointer-identical).
    for (size_t i = 0; i < used; i++) {
        if (strcmp(table[i].tag.load(std::memory_order_relaxed), tag) == 0)
            return &table[i];
    }

    // First use: claim a free slot. compare_exchange keeps a registration race
    // from double-claiming; the loser re-checks what the winner wrote.
    for (size_t i = used; i < kMaxTags; i++) {
        const char *expected = nullptr;
        if (table[i].tag.compare_exchange_strong(expected, tag, std::memory_order_acq_rel))
            return &table[i];
        if (expected == tag || strcmp(expected, tag) == 0)
            return &table[i];
    }

    return nullptr; // table full - bump kMaxTags if this ever happens
}

} // namespace

bool inPsram(const void *p)
{
#if defined(ARCH_ESP32)
    return p && esp_ptr_external_ram(p);
#else
    (void)p;
    return false;
#endif
}

void add(const char *tag, int32_t delta)
{
    Entry *e = findOrRegister(tag);
    if (e)
        e->bytes.fetch_add(delta, std::memory_order_relaxed);
}

void set(const char *tag, uint32_t bytes)
{
    Entry *e = findOrRegister(tag);
    if (e) {
        e->bytes.store((int32_t)bytes, std::memory_order_relaxed);
        e->psramBytes.store(0, std::memory_order_relaxed); // region unknown; also clears on free
    }
}

void add(const char *tag, int32_t delta, const void *p)
{
    Entry *e = findOrRegister(tag);
    if (!e)
        return;
    e->bytes.fetch_add(delta, std::memory_order_relaxed);
    if (inPsram(p))
        e->psramBytes.fetch_add(delta, std::memory_order_relaxed);
}

void set(const char *tag, uint32_t bytes, const void *p)
{
    Entry *e = findOrRegister(tag);
    if (!e)
        return;
    e->bytes.store((int32_t)bytes, std::memory_order_relaxed);
    e->psramBytes.store(inPsram(p) ? (int32_t)bytes : 0, std::memory_order_relaxed);
}

size_t snapshot(Tag *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < kMaxTags && n < max; i++) {
        const char *tag = table[i].tag.load(std::memory_order_acquire);
        if (!tag)
            break;
        out[n].tag = tag;
        out[n].bytes = table[i].bytes.load(std::memory_order_relaxed);
        out[n].psramBytes = table[i].psramBytes.load(std::memory_order_relaxed);
        n++;
    }
    return n;
}

void logBreakdown(const char *when)
{
    Tag rows[kMaxTags];
    size_t n = snapshot(rows, kMaxTags);
    if (n == 0)
        return;

    // RedirectablePrint::vprintf formats through a 160-byte buffer and silently swaps the last
    // character for a newline, so an over-long line looks like a complete one. A full table needs
    // roughly 370 characters, so wrap onto as many lines as it takes rather than pick a buffer size
    // a later tag would quietly overflow. 128 leaves room for the "MemAudit[periodic]: " prefix.
    static constexpr size_t kMaxPayload = 128;

    const char *label = when ? when : "?";
    char line[kMaxPayload + 1];
    size_t pos = 0;
    int32_t totalInternal = 0;
    int32_t totalPsram = 0;

    for (size_t i = 0; i < n; i++) {
        const int32_t psram = rows[i].psramBytes;
        const int32_t internal = rows[i].bytes - psram;
        char row[64];
        const int rowLen = psram ? snprintf(row, sizeof(row), "%s=%ld+%ldps", rows[i].tag, (long)internal, (long)psram)
                                 : snprintf(row, sizeof(row), "%s=%ld", rows[i].tag, (long)internal);
        if (rowLen < 0 || (size_t)rowLen >= sizeof(row))
            continue; // unrepresentable row; the totals below still count it

        // Flush first when this row would not fit, so a row never straddles two lines.
        if (pos && pos + 1 + (size_t)rowLen > kMaxPayload) {
            LOG_INFO("MemAudit[%s]: %s", label, line);
            pos = 0;
        }
        if (pos)
            line[pos++] = ' ';
        memcpy(line + pos, row, (size_t)rowLen);
        pos += (size_t)rowLen;
        line[pos] = '\0';

        totalInternal += internal;
        totalPsram += psram;
    }

    if (pos)
        LOG_INFO("MemAudit[%s]: %s", label, line);
    LOG_INFO("MemAudit[%s]: total=%ld internal=%ld psram=%ld", label, (long)(totalInternal + totalPsram), (long)totalInternal,
             (long)totalPsram);
}

void logHeapRegions()
{
#if defined(ARCH_ESP32)
    LOG_INFO("MemAudit: internal heap regions follow, in the IDF's own format");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
#endif
}

} // namespace memaudit

#endif // MESHTASTIC_MEM_AUDIT
