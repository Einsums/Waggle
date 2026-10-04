//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Event.hpp"

WAGGLE_NAMESPACE_BEGIN

class StringTable;
class SiteTable;
class DomainTable;

/// A value attached to a zone in the trace: an annotation, or a counter's change across the zone.
struct TraceArg {
    uint32_t          key_id{0}; ///< the name, as a string table id
    AnnotateValueType type{AnnotateValueType::Int64};
    bool              is_unsigned{false}; ///< an Int64 holding a uint64 (a counter)
    int64_t           int_val{0};
    double            float_val{0.0};
    uint32_t          string_id{0};
};

/**
 * @brief Writes every zone the consumer processes to a Perfetto trace file.
 *
 * The file is a protobuf ``Trace`` that ui.perfetto.dev and trace_processor read. Each recording
 * thread is a packet sequence of its own, with its zone names, call sites and argument names
 * interned and its timestamps delta-encoded on a clock local to the sequence, so a zone costs
 * about 32 bytes. Device work, the live-bytes curve and each thread's energy go on a process-wide
 * sequence with absolute timestamps, on CLOCK_MONOTONIC's scale (std::chrono::steady_clock's).
 *
 * A zone becomes a begin and an end on its thread's track. Its annotations and counter changes
 * are written with its end, which trace_processor merges into the zone's arguments. A submission
 * of device work is an instant on the submitting thread with a flow to the work's span.
 *
 * Not thread-safe: the consumer calls it under its tree lock.
 */
class WAGGLE_EXPORT TraceWriter {
  public:
    TraceWriter(StringTable const &strings, SiteTable const &sites);
    ~TraceWriter();

    TraceWriter(TraceWriter const &)            = delete;
    TraceWriter &operator=(TraceWriter const &) = delete;
    TraceWriter(TraceWriter &&)                 = delete;
    TraceWriter &operator=(TraceWriter &&)      = delete;

    /// Start a trace in @p path, closing any trace already open. Returns why it could not, or
    /// empty. @p domains names zones' categories; it must outlive the trace.
    auto open(std::string const &path, DomainTable const &domains) -> std::string;
    /// Write what is buffered and close the file. Zones still open stay open in the trace.
    void               close();
    [[nodiscard]] auto is_open() const -> bool { return _file != nullptr; }
    /// The file being written; empty when none is.
    [[nodiscard]] auto path() const -> std::string const & { return _path; }
    /// Zones written to the current or last trace.
    [[nodiscard]] auto zones() const -> uint64_t { return _zones; }

    /// Describe thread @p thread_id (its kernel id, 0 if unknown, and its name), again when the
    /// name changes.
    void thread(uint32_t thread_id, uint64_t kernel_thread, std::string const &name);

    void begin(uint32_t thread_id, uint64_t ticks, uint32_t site_id, uint32_t name_id);
    /// An annotation on the thread's innermost open zone, written with its end.
    void annotate(uint32_t thread_id, TraceArg const &arg);
    /// The innermost open zone's end, with @p extra (its counters) among its arguments.
    void end(uint32_t thread_id, uint64_t ticks, std::span<TraceArg const> extra = {});
    /// The innermost open zone ended, but its end event was lost; @p ticks is when that was noticed.
    void end_lost(uint32_t thread_id, uint64_t ticks);

    /// Device work submitted from the thread, @p token naming it for its span.
    void submit(uint32_t thread_id, uint64_t ticks, uint64_t token);
    /// Device work done on @p track_id, times in steady_clock nanoseconds; @p token its submission's, or 0.
    void device_span(uint32_t track_id, uint32_t name_id, int64_t start_ns, int64_t end_ns, uint64_t token);
    /// The process's live tracked bytes after an allocation or free.
    void live_bytes(uint64_t ticks, int64_t bytes);
    /// Thread @p thread_id's energy since it started, read at @p ticks.
    void energy(uint32_t thread_id, uint64_t ticks, uint64_t total_nj);

    /// Write the buffer to the file if it has grown large or waited long.
    void maybe_flush();

  private:
    /// A packet sequence: interning and timestamps are per sequence.
    struct Sequence {
        uint32_t                     id{0};
        bool                         started{false};
        uint64_t                     last_ns{0};
        std::unordered_set<uint64_t> names, sites, categories, arg_names;
    };
    struct Thread {
        Sequence                           seq;
        uint64_t                           track{0};
        uint64_t                           kernel_thread{0};
        std::string                        name;
        bool                               described{false};
        bool                               energy_described{false};
        std::vector<std::vector<TraceArg>> open; ///< arguments waiting for each open zone's end
    };

    auto thread_state(uint32_t thread_id) -> Thread &;
    void start_sequence(Thread &t, uint64_t ns);
    void describe_thread(uint32_t thread_id, Thread &t);
    /// Open a TracePacket on the thread's sequence at @p ns; returns the nested-message mark.
    auto thread_packet(Thread &t, uint64_t ns) -> size_t;
    /// Open a TracePacket on the process sequence at absolute @p ns.
    auto process_packet(uint64_t ns, bool interned) -> size_t;
    void write_arg(Sequence &seq, TraceArg const &arg, std::vector<std::pair<uint32_t, std::string const *>> &new_names);
    void flush();

    StringTable const &_strings;
    SiteTable const   &_sites;
    DomainTable const *_domains{nullptr};

    std::FILE  *_file{nullptr};
    std::string _path;
    std::string _buffer;
    uint64_t    _last_flush_ns{0};
    uint64_t    _zones{0};
    uint64_t    _pid{0};

    Sequence                             _process;
    std::unordered_map<uint32_t, Thread> _threads;
    std::unordered_set<uint32_t>         _device_tracks; ///< tracks described, by string id
    bool                                 _memory_described{false};
};

WAGGLE_NAMESPACE_END
