//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

WAGGLE_NAMESPACE_BEGIN

/// Number of hardware counter slots per event.
static constexpr int kNumCounterSlots = 4;

/// Hardware events a thread may open, across its groups.
static constexpr int kMaxCounterEvents = 8;

/// One thread's open counters. Kept in the thread's channel, not in the backend, so a backend has
/// no per-thread state of its own and a test can count with a second one beside the process's.
struct ThreadCounters {
    // NOLINTBEGIN(modernize-avoid-c-arrays)
    int fds[kMaxCounterEvents] = {-1, -1, -1, -1, -1, -1, -1, -1}; ///< Linux perf's, each group's leader first
    int group_start[3]         = {0, 0, 0};                        ///< where each group's events begin in fds; the last is the end
    // NOLINTEND(modernize-avoid-c-arrays)
    int  groups      = 0; ///< event groups opened: 2 when the events did not fit the PMU at once
    int  count       = 0; ///< events opened
    bool open        = false;
    bool multiplexed = false; ///< whether the groups share the PMU in turns, and readings are scaled
    /// Each slot's last reading: a scaled reading can come out a little under the one before, and
    /// a zone's counters are differences.
    mutable std::array<uint64_t, kNumCounterSlots> last{};
};

/**
 * @brief Where hardware counter values come from: the "counters" source's backend.
 *
 * Counters are read as a zone opens and as it closes, on the zone's thread, so a backend reads
 * only the calling thread's. Reading costs a system call, a few hundred nanoseconds, against a few
 * nanoseconds for a zone without counters: that is why the source is off unless asked for.
 */
class CounterBackend {
  public:
    virtual ~CounterBackend() = default;

    /// Start counting on the calling thread. False when it cannot; @ref why_not says why.
    virtual auto open(ThreadCounters &counters) -> bool = 0;

    /// Stop counting.
    virtual void close(ThreadCounters &counters) = 0;

    /// The calling thread's counts since it opened, one per slot; zeros where it does not count.
    virtual void read(ThreadCounters const &counters, std::array<uint64_t, kNumCounterSlots> &values) = 0;

    /// What slot @p slot counts, as reports name it; empty for a slot this backend leaves unused.
    [[nodiscard]] virtual auto slot_name(int slot) const -> std::string = 0;

    /// Why the last open failed; empty if none has.
    [[nodiscard]] virtual auto why_not() const -> std::string = 0;

    /// The backend, for the source's status: "Linux perf_event", "XNU thread counts".
    [[nodiscard]] virtual auto describe() const -> std::string = 0;
};

/// A platform with no backend: nothing opens.
class NoopCounterBackend : public CounterBackend {
  public:
    auto open(ThreadCounters & /*counters*/) -> bool override { return false; }
    void close(ThreadCounters & /*counters*/) override {}
    void read(ThreadCounters const & /*counters*/, std::array<uint64_t, kNumCounterSlots> &values) override { values.fill(0); }
    [[nodiscard]] auto slot_name(int /*slot*/) const -> std::string override { return {}; }
    [[nodiscard]] auto why_not() const -> std::string override { return "this platform has no hardware counter backend"; }
    [[nodiscard]] auto describe() const -> std::string override { return "none"; }
};

#ifdef __linux__
/// Use the roofline source's counters in every thread that opens them from now on: cycles,
/// instructions, FLOPs and last-level cache misses (as bytes), in place of the counters source's
/// cycles, instructions, cache and branch misses. @p precision is "fp64" or "fp32". False, with
/// @p why_not set, when this CPU's FLOP events are not known, or a thread opened its counters
/// already: the set is the process's, so it is chosen before any thread counts.
WAGGLE_EXPORT auto use_roofline_counters(std::string const &precision, std::string &why_not) -> bool;

/// Whether the process's counters are the roofline set, and what they count, for its status.
WAGGLE_EXPORT auto roofline_counters() -> std::string;

/// Threads whose counter groups did not fit the PMU at once, and are multiplexed.
WAGGLE_EXPORT auto multiplexed_threads() -> uint64_t;

/// Why perf_event_open failed with @p error, in terms a person can act on: the sysctl to change, a
/// container's seccomp profile, or a machine with no counters. The counters and sampling sources'.
WAGGLE_EXPORT auto perf_refusal(int error) -> std::string;
/// The same, with kernel.perf_event_paranoid's value given (empty when unreadable): for tests,
/// which cannot change the machine's.
WAGGLE_EXPORT auto perf_refusal(int error, std::string const &paranoid) -> std::string;

/// Linux perf_event_open: a group of counters, read together in one system call.
class WAGGLE_EXPORT PerfCounterBackend : public CounterBackend {
  public:
    /// A counter to open: what reports call it, and perf_event_attr's type and config.
    struct Counter {
        std::string name;
        uint32_t    type;
        uint64_t    config;
    };

    /// An event to open, without a name of its own: perf_event_attr's type and config.
    struct EventCode {
        uint32_t type;
        uint64_t config;
    };

    /// What a slot reports: a sum of events, each times its weight (FLOPs: scalar + 2 x 128-bit +
    /// 4 x 256-bit + 8 x 512-bit), by index into the events. A zone's slot is then the same sum of
    /// its events' differences.
    struct Slot {
        std::string                           name;
        std::vector<std::pair<int, uint64_t>> terms;
    };

    /// The hardware counters: cycles, instructions, cache misses, branch misses.
    PerfCounterBackend();
    /// @p counters, at most kNumCounterSlots: software ones (task-clock) count where a virtual
    /// machine has no hardware counters, as in CI.
    explicit PerfCounterBackend(std::vector<Counter> counters);
    /// @p events, at most kMaxCounterEvents, reported as @p slots, at most kNumCounterSlots. The
    /// events are opened as one group, read in one system call; when the PMU cannot hold them at
    /// once, as two, which it multiplexes. @p split_after, for tests, splits them after that many
    /// events whether or not they fit.
    PerfCounterBackend(std::vector<EventCode> events, std::vector<Slot> slots, int split_after = 0);

    /// Count @p events as @p slots from the next thread that opens on. False if a thread opened
    /// already, as zones' readings must keep one meaning.
    auto reconfigure(std::vector<EventCode> events, std::vector<Slot> slots, std::string description) -> bool;

    auto               open(ThreadCounters &counters) -> bool override;
    void               close(ThreadCounters &counters) override;
    void               read(ThreadCounters const &counters, std::array<uint64_t, kNumCounterSlots> &values) override;
    [[nodiscard]] auto slot_name(int slot) const -> std::string override;
    [[nodiscard]] auto why_not() const -> std::string override;
    [[nodiscard]] auto describe() const -> std::string override;

  private:
    auto open_group(ThreadCounters &counters, int first, int end) -> bool;

    std::vector<EventCode> _events;
    std::vector<Slot>      _slots;
    int                    _split_after{0};
    std::string            _description{"Linux perf_event"};
    bool                   _opened{false}; ///< whether any thread opened, which fixes the set
    mutable std::mutex     _mutex;         // threads open their counters concurrently
    std::string            _why_not;
};
#endif

#ifdef __APPLE__
/// XNU's per-thread cycles and instructions (the thread_selfcounts system call), which any process
/// may read for its own threads: no root, unlike the configurable counters behind kpc. Slots:
/// cycles and instructions on every core, then those on the efficiency cores alone.
class WAGGLE_EXPORT XnuCounterBackend : public CounterBackend {
  public:
    auto               open(ThreadCounters &counters) -> bool override;
    void               close(ThreadCounters &counters) override;
    void               read(ThreadCounters const &counters, std::array<uint64_t, kNumCounterSlots> &values) override;
    [[nodiscard]] auto slot_name(int slot) const -> std::string override;
    [[nodiscard]] auto why_not() const -> std::string override;
    [[nodiscard]] auto describe() const -> std::string override { return "XNU thread counts"; }

  private:
    mutable std::mutex _mutex; // threads open their counters concurrently
    std::string        _why_not;
};
#endif

#ifdef __APPLE__
/// Whether this Mac is a virtual machine, which gives its guests no performance counters.
WAGGLE_EXPORT auto running_in_vm() -> bool;
#endif

/// The process's backend: Linux perf, XNU's thread counts, or none.
WAGGLE_EXPORT auto get_counter_backend() -> CounterBackend &;

WAGGLE_NAMESPACE_END
