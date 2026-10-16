//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Sources.hpp"

#include <fmt/format.h>

#include <atomic>
#include <mutex>

#include "CounterBackend.hpp"
#include "Duplicates.hpp"
#include "Energy.hpp"
#include "Profiler.hpp"

WAGGLE_NAMESPACE_BEGIN

auto source_requested(Settings const &settings, std::string_view name) -> bool {
    std::string_view rest = settings.sources;
    while (!rest.empty()) {
        auto const       comma = rest.find(',');
        std::string_view item  = rest.substr(0, comma);
        rest                   = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (item == name) {
            return true;
        }
    }
    return false;
}

auto source_statuses() -> std::vector<SourceStatus> {
    return {counters::status(), energy::status(), ompt::status(), roofline::status(), sampling::status(), signposts::status()};
}

namespace {

/// Threads that could and could not read, for one source.
struct Openings {
    std::atomic<uint64_t> opened{0};
    std::atomic<uint64_t> refused{0};
    void                  note(bool ok) { (ok ? opened : refused).fetch_add(1, std::memory_order_relaxed); }
};

/// A source read for each thread as it registers: off, active, unavailable (and why), or waiting
/// for a thread to register.
auto thread_source_status(char const *name, Openings const &openings, std::string const &active_detail, std::string const &why_not)
    -> SourceStatus {
    SourceStatus status{.name = name, .state = "off", .detail = ""};
    if (collector_inactive() || Profiler::gone() || !source_requested(Profiler::instance().settings(), name)) {
        return status;
    }
    auto const opened  = openings.opened.load(std::memory_order_relaxed);
    auto const refused = openings.refused.load(std::memory_order_relaxed);
    if (opened > 0) {
        status.state  = "active";
        status.detail = active_detail;
        if (refused > 0) {
            status.detail += fmt::format("; {} thread(s) could not read: {}", refused, why_not);
        }
    } else if (refused > 0) {
        status.state  = "unavailable";
        status.detail = why_not;
    } else {
        status.state  = "waiting";
        status.detail = fmt::format("no thread has recorded since it was asked for: a thread reads if the source was asked for "
                                    "when it first recorded, so set WAGGLE_SOURCES={} in the environment, or configure Waggle "
                                    "before threads start",
                                    name);
    }
    return status;
}

Openings g_counter_threads;

} // namespace

namespace counters {
void note_thread(bool opened) {
    g_counter_threads.note(opened);
}

auto status() -> SourceStatus {
    auto const &backend = get_counter_backend();
    auto        status  = thread_source_status("counters", g_counter_threads, backend.describe(), backend.why_not());
    if (status.state == "active" && roofline::status().state == "active") {
        status.detail += "; cycles and instructions only, as the roofline source takes the cache- and branch-miss counters";
    }
    return status;
}
} // namespace counters

namespace {
std::mutex  g_roofline_choice_mutex;
bool        g_roofline_chosen = false;
std::string g_roofline_why_not;
} // namespace

namespace roofline {
void note_choice(bool chosen, std::string why_not) {
    std::scoped_lock const lock(g_roofline_choice_mutex);
    g_roofline_chosen  = chosen;
    g_roofline_why_not = std::move(why_not);
}

auto status() -> SourceStatus {
    SourceStatus status{.name = "roofline", .state = "off", .detail = ""};
    if (collector_inactive() || Profiler::gone() || !source_requested(Profiler::instance().settings(), "roofline")) {
        return status;
    }
    {
        std::scoped_lock const lock(g_roofline_choice_mutex);
        if (!g_roofline_chosen) {
            status.state  = "unavailable";
            status.detail = g_roofline_why_not;
            return status;
        }
    }
    auto const &backend = get_counter_backend();
    std::string detail  = backend.describe();
#ifdef __linux__
    if (auto const n = multiplexed_threads(); n > 0) {
        detail += fmt::format("; {} thread(s) have more events than the PMU holds at once, so their counts are multiplexed and "
                              "scaled, and short zones carry more error",
                              n);
    }
#endif
    return thread_source_status("roofline", g_counter_threads, detail, backend.why_not());
}
} // namespace roofline

namespace energy {
auto status() -> SourceStatus {
    SourceStatus status{.name = "energy", .state = "off", .detail = ""};
    if (collector_inactive() || Profiler::gone() || !source_requested(Profiler::instance().settings(), "energy")) {
        return status;
    }
    if (std::string why; !energy_available(why)) {
        status.state  = "unavailable";
        status.detail = why;
    } else if (Profiler::instance().consumer()->energy_reads() > 0) {
        status.state  = "active";
        status.detail = "XNU's per-thread estimate from the CPU's power model, updated every few milliseconds and sampled as "
                        "the consumer drains; each change is credited to the zone open when it was read";
    } else {
        status.state  = "waiting";
        status.detail = "no thread has been read yet";
    }
    return status;
}
} // namespace energy

namespace sampling {
auto status() -> SourceStatus {
    SourceStatus status{.name = "sampling", .state = "off", .detail = ""};
    if (collector_inactive() || Profiler::gone() || !source_requested(Profiler::instance().settings(), "sampling")) {
        return status;
    }
    auto const state = Profiler::instance().consumer()->sampling_state();
    if (state.running) {
        status.state  = "active";
        status.detail = fmt::format("Linux perf_event: {} sampled at {} Hz{} on {} thread(s); {} samples, {} lost to full rings",
                                    state.event == "cycles" ? "hardware cycles" : "the software CPU clock (no hardware counters)",
                                    state.rate_hz, state.callchain ? " with call chains" : "", state.threads, state.samples, state.lost);
        if (state.refused > 0) {
            status.detail += fmt::format("; {} thread(s) could not be sampled: {}", state.refused, state.why_not);
        }
    } else if (!state.why_not.empty()) {
        status.state  = "unavailable";
        status.detail = state.why_not;
    } else {
        status.state  = "waiting";
        status.detail = "the consumer opens the sampling events on its next pass";
    }
    return status;
}
} // namespace sampling

WAGGLE_NAMESPACE_END
