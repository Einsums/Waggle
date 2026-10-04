//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// The signposts source: zones emitted as os_signpost intervals, so Instruments (its os_signpost
// and Logging instruments, or `xctrace record --template Logging`) shows them beside its own CPU,
// GPU and memory tracks. Subsystem "waggle", one category per library (the zone's domain).
//
// An interval's name must be a string literal, and zone names are data, so every interval is named
// "zone" and carries the zone's name as its message. Zones nested on one thread are open at once
// under that one name, so each gets its own interval id: the address of the thread's slot for its
// nesting level, which no other open interval shares.

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Duplicates.hpp"
#include "Profiler.hpp"
#include "Sources.hpp"

#ifdef __APPLE__
#    include <os/log.h>
#    include <os/signpost.h>
#endif

WAGGLE_NAMESPACE_BEGIN

#ifdef __APPLE__

namespace {

constexpr uint32_t kMaxDepth = 64;

/// The log each open zone of this thread began on, by nesting level, for its end to use. Plain
/// pointers, trivially destructible: zones may close while the thread is torn down.
thread_local os_log_t t_logs[kMaxDepth + 1]{}; // NOLINT(modernize-avoid-c-arrays)

/// The log for a library: subsystem "waggle", the library's name as its category. Never freed.
auto log_for(std::string const &domain) -> os_log_t {
    static auto *const     mutex = new std::mutex;
    static auto *const     logs  = new std::unordered_map<std::string, os_log_t>;
    std::scoped_lock const lock(*mutex);
    auto [it, added] = logs->try_emplace(domain, nullptr);
    if (added) {
        it->second = os_log_create("waggle", domain.empty() ? "zones" : domain.c_str());
    }
    return it->second;
}

} // namespace

void Profiler::signpost_begin(uint32_t site_id, uint32_t name_id, uint32_t depth) {
    if (depth > kMaxDepth) {
        return;
    }
    Site const     site = _sites.get(site_id);
    os_log_t const log  = log_for(_domains.name(site.domain));
    t_logs[depth]       = log;
    if (os_signpost_enabled(log)) {
        // Strings are never removed from the table, so the name stays valid.
        std::string const &name = _strings.get(name_id != 0 ? name_id : site.name_id);
        os_signpost_interval_begin(log, os_signpost_id_make_with_pointer(log, &t_logs[depth]), "zone", "%{public}s", name.c_str());
    }
}

void Profiler::signpost_end(uint32_t depth) {
    if (depth > kMaxDepth || t_logs[depth] == nullptr) {
        return;
    }
    os_log_t const log = t_logs[depth];
    t_logs[depth]      = nullptr;
    if (os_signpost_enabled(log)) {
        os_signpost_interval_end(log, os_signpost_id_make_with_pointer(log, &t_logs[depth]), "zone");
    }
}

#else

void Profiler::signpost_begin(uint32_t /*site_id*/, uint32_t /*name_id*/, uint32_t /*depth*/) {
}

void Profiler::signpost_end(uint32_t /*depth*/) {
}

#endif

namespace signposts {
namespace {
std::atomic<uint64_t> g_emitting{0};
} // namespace

auto available() -> bool {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

void note_thread(bool emitting) {
    if (emitting) {
        g_emitting.fetch_add(1, std::memory_order_relaxed);
    }
}

auto status() -> SourceStatus {
    SourceStatus status{.name = "signposts", .state = "off", .detail = ""};
    if (collector_inactive() || Profiler::gone() || !source_requested(Profiler::instance().settings(), "signposts")) {
        return status;
    }
    if (!available()) {
        status.state  = "unavailable";
        status.detail = "signposts are macOS's (os_signpost, for Instruments)";
    } else if (g_emitting.load(std::memory_order_relaxed) > 0) {
        status.state  = "active";
        status.detail = "os_signpost, subsystem \"waggle\", a category per library: record with Instruments' Logging template";
    } else {
        status.state  = "waiting";
        status.detail = "no thread has recorded since it was asked for: a thread emits if the source was asked for when it first "
                        "recorded, so set WAGGLE_SOURCES=signposts in the environment, or configure Waggle before threads start";
    }
    return status;
}
} // namespace signposts

WAGGLE_NAMESPACE_END
