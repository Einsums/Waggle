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

namespace ompt {
/// The OpenMP source: zones for parallel regions, each thread's share of them, and barrier waits,
/// from the runtime's OMPT callbacks.
WAGGLE_EXPORT auto status() -> SourceStatus;
} // namespace ompt

/// A demangled function name without its argument list or return type, for naming a zone after
/// the function it is in: "void ns::f<int>(int) const" is "ns::f<int>".
WAGGLE_EXPORT auto short_function_name(std::string_view name) -> std::string;

WAGGLE_NAMESPACE_END
