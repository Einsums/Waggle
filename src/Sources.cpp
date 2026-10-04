//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Sources.hpp"

#include <fmt/format.h>

#include <atomic>

#include "CounterBackend.hpp"
#include "Duplicates.hpp"
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
    return {counters::status(), ompt::status(), signposts::status()};
}

namespace counters {
namespace {
std::atomic<uint64_t> g_counting{0}; // threads whose counters opened
std::atomic<uint64_t> g_refused{0};  // threads whose counters did not
} // namespace

void note_thread(bool opened) {
    (opened ? g_counting : g_refused).fetch_add(1, std::memory_order_relaxed);
}

auto status() -> SourceStatus {
    SourceStatus status{.name = "counters", .state = "off", .detail = ""};
    if (collector_inactive() || Profiler::gone() || !source_requested(Profiler::instance().settings(), "counters")) {
        return status;
    }
    auto const &backend  = get_counter_backend();
    auto const  counting = g_counting.load(std::memory_order_relaxed);
    auto const  refused  = g_refused.load(std::memory_order_relaxed);
    if (counting > 0) {
        status.state  = "active";
        status.detail = backend.describe();
        if (refused > 0) {
            status.detail += fmt::format("; {} thread(s) could not count: {}", refused, backend.why_not());
        }
    } else if (refused > 0) {
        status.state  = "unavailable";
        status.detail = backend.why_not();
    } else {
        status.state  = "waiting";
        status.detail = "no thread has recorded since it was asked for: a thread counts if the source was asked for when it first "
                        "recorded, so set WAGGLE_SOURCES=counters in the environment, or configure Waggle before threads start";
    }
    return status;
}
} // namespace counters

WAGGLE_NAMESPACE_END
