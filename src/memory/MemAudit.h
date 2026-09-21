#pragma once

#include "configuration.h"
#include <stddef.h>
#include <stdint.h>

// MemAudit: tiny per-subsystem heap accounting registry.
//
// Subsystems that own a large long-lived allocation report it here under a short
// tag ("nodedb", "pkthist", ...). logBreakdown() then prints one line, e.g.
//   MemAudit[boot]: tmm=2500 warm=4000 pkthist=5824 nodedb=13440 total=25764
// so heap regressions in field reports self-diagnose from the serial log instead
// of needing a hand-built breakdown for every release.
//
// Tags must be string LITERALS (or otherwise immortal strings): the registry
// stores the pointer, compares by pointer first and falls back to strcmp for
// the same text duplicated across translation units.
//
// Concurrency: counters are 32-bit std::atomic accessed with relaxed ordering -
// on ARM Cortex-M aligned 32-bit loads/stores are single instructions and the
// update sites are low-rate, so add() stays a few instructions with no locks
// (the one hot path is the per-packet pool add). Registration claims a table
// slot with a compare-exchange, so first-use racing is safe too. Counts are
// best-effort diagnostics, not exact bookkeeping.
//
// Compiled out (no-op inline stubs, so call sites need no #ifdefs) when
// MESHTASTIC_MEM_AUDIT is 0 - the default on STM32WL, the tightest flash target.
#ifndef MESHTASTIC_MEM_AUDIT
#ifdef ARCH_STM32WL
#define MESHTASTIC_MEM_AUDIT 0
#else
#define MESHTASTIC_MEM_AUDIT 1
#endif
#endif

namespace memaudit
{

// Fixed registry capacity - updates for tags beyond this are dropped (bump if needed).
constexpr size_t kMaxTags = 16;

// One snapshot row, as returned by snapshot().
struct Tag {
    const char *tag;    // the literal passed to add()/set()
    int32_t bytes;      // current byte count for that subsystem
    int32_t psramBytes; // the part of `bytes` that lives in PSRAM; 0 where the region is unknown
};

#if MESHTASTIC_MEM_AUDIT

// True when p was allocated from external PSRAM. Always false off ESP32.
bool inPsram(const void *p);

// Adjust a subsystem's byte count (registers the tag on first use). Safe from
// concurrent threads; this is the form to use on per-object alloc/free paths.
void add(const char *tag, int32_t delta);

// Set a subsystem's byte count outright - for one-shot pool/table allocations
// where the total is known (use 0 on free or allocation failure). Clears the
// PSRAM split, so a tag that knows its region must use the overloads below.
void set(const char *tag, uint32_t bytes);

// Same, but classify the allocation by its address. On a PSRAM board only the internal figure is
// ever tight, so a breakdown that cannot tell the two apart points at the wrong subsystem - the
// display's frame buffers are 300KB+ of PSRAM and under 10KB of internal DRAM.
void add(const char *tag, int32_t delta, const void *p);
void set(const char *tag, uint32_t bytes, const void *p);

// Copy up to max registered tags into out; returns the number written.
size_t snapshot(Tag *out, size_t max);

// Log the whole table as a single LOG_INFO line, labeled with `when` ("boot", ...).
void logBreakdown(const char *when);

// Print every internal heap region's size, free bytes and largest block. Answers what the
// aggregate figures cannot: whether a small "largest free block" is real fragmentation or just a
// region nothing ever allocates from. Goes out through the IDF's own printf, not the log ring.
void logHeapRegions();

#else

// No-op stubs so call sites compile away without #ifdefs.
inline bool inPsram(const void *)
{
    return false;
}
inline void add(const char *, int32_t) {}
inline void set(const char *, uint32_t) {}
inline void add(const char *, int32_t, const void *) {}
inline void set(const char *, uint32_t, const void *) {}
inline size_t snapshot(Tag *, size_t)
{
    return 0;
}
inline void logBreakdown(const char *) {}
inline void logHeapRegions() {}

#endif // MESHTASTIC_MEM_AUDIT

} // namespace memaudit
