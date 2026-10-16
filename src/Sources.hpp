//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <Waggle/Types.hpp>

#include <string>
#include <string_view>
#include <vector>

WAGGLE_NAMESPACE_BEGIN

/// Sources are optional instruments built into the collector, each off unless the `sources`
/// setting names it (`WAGGLE_SOURCES=openmp`), each recording into a domain of its own so that
/// domain's switch mutes it while the program runs.
struct SourceStatus {
    std::string name;
    /// "off" (not requested), "active", or why a requested source records nothing: "waiting" (what
    /// it instruments has not started), "missed" (that started before the request) or "unavailable".
    std::string state;
    /// What a person needs to know about the state: the runtime it attached to, or what to change.
    std::string detail;
};

/// Whether @p settings name source @p name.
WAGGLE_EXPORT auto source_requested(Settings const &settings, std::string_view name) -> bool;

/// Every source built into this collector, and its state.
WAGGLE_EXPORT auto source_statuses() -> std::vector<SourceStatus>;

namespace counters {
/// The hardware counter source: each zone's counter readings (cycles, instructions, ...) from the
/// platform's backend. A thread counts if the source was asked for when it first recorded.
WAGGLE_EXPORT auto status() -> SourceStatus;
/// A thread tried to open its counters: @p opened is whether it could.
WAGGLE_EXPORT void note_thread(bool opened);
} // namespace counters

namespace numa {
/// The numa source: which NUMA node each large block's pages are on, set against the nodes the
/// program's threads run on, checked by the consumer at most every 500 ms.
WAGGLE_EXPORT auto status() -> SourceStatus;
} // namespace numa

namespace roofline {
/// The roofline source: each zone's FLOPs and an estimate of its DRAM bytes, from hardware
/// counters read with the zone's, which it shares with the counters source.
WAGGLE_EXPORT auto status() -> SourceStatus;
/// Settings were applied: @p chosen is whether the roofline counters are the process's, and
/// @p why_not why not when they were asked for.
WAGGLE_EXPORT void note_choice(bool chosen, std::string why_not);
} // namespace roofline

namespace energy {
/// The energy source: each zone's energy, sampled from the platform's per-thread estimate (macOS).
WAGGLE_EXPORT auto status() -> SourceStatus;
} // namespace energy

namespace sampling {
/// The sampling source: where each zone's time goes, by instruction address, from the kernel
/// sampling every thread of the process (Linux perf_event).
WAGGLE_EXPORT auto status() -> SourceStatus;
} // namespace sampling

namespace signposts {
/// The signposts source: every zone also emitted as an os_signpost interval, so Instruments shows
/// them beside its own tracks, one category per library. macOS only.
WAGGLE_EXPORT auto status() -> SourceStatus;
/// Whether this platform can emit signposts.
WAGGLE_EXPORT auto available() -> bool;
/// A thread registered while the source was asked for: @p emitting is whether it emits.
WAGGLE_EXPORT void note_thread(bool emitting);
} // namespace signposts

namespace ompt {
/// The OpenMP source: zones for parallel regions, each thread's share of them, and barrier waits,
/// from the runtime's OMPT callbacks.
WAGGLE_EXPORT auto status() -> SourceStatus;
/// The site of the region zone ("omp parallel: f") whose threads' shares are zones of @p work_site
/// ("omp work: f"), or 0 when @p work_site is no share's: for crediting a worker's samples to the
/// zone that started its region too.
WAGGLE_EXPORT auto region_site_of(uint32_t work_site) -> uint32_t;
/// Whether @p site is a region zone's ("omp parallel: f").
WAGGLE_EXPORT auto is_region_site(uint32_t site) -> bool;
} // namespace ompt

/// A demangled function name without its argument list or return type, for naming a zone after
/// the function it is in: "void ns::f<int>(int) const" is "ns::f<int>".
WAGGLE_EXPORT auto short_function_name(std::string_view name) -> std::string;

WAGGLE_NAMESPACE_END
