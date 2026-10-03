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

/// One thread's open counters. Kept in the thread's channel, not in the backend, so a backend has
/// no per-thread state of its own and a test can count with a second one beside the process's.
struct ThreadCounters {
    int  fds[kNumCounterSlots] = {-1, -1, -1, -1}; // NOLINT(modernize-avoid-c-arrays): Linux perf's, the group leader first
    int  count                 = 0;                ///< counters opened
    bool open                  = false;
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
/// Linux perf_event_open: a group of counters, read together in one system call.
class WAGGLE_EXPORT PerfCounterBackend : public CounterBackend {
  public:
    /// A counter to open: what reports call it, and perf_event_attr's type and config.
    struct Counter {
        std::string name;
        uint32_t    type;
        uint64_t    config;
    };

    /// The hardware counters: cycles, instructions, cache misses, branch misses.
    PerfCounterBackend();
    /// @p counters, at most kNumCounterSlots: software ones (task-clock) count where a virtual
    /// machine has no hardware counters, as in CI.
    explicit PerfCounterBackend(std::vector<Counter> counters);

    auto               open(ThreadCounters &counters) -> bool override;
    void               close(ThreadCounters &counters) override;
    void               read(ThreadCounters const &counters, std::array<uint64_t, kNumCounterSlots> &values) override;
    [[nodiscard]] auto slot_name(int slot) const -> std::string override;
    [[nodiscard]] auto why_not() const -> std::string override;
    [[nodiscard]] auto describe() const -> std::string override { return "Linux perf_event"; }

  private:
    std::vector<Counter> _counters;
    mutable std::mutex   _mutex; // threads open their counters concurrently
    std::string          _why_not;
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

/// The process's backend: Linux perf, XNU's thread counts, or none.
WAGGLE_EXPORT auto get_counter_backend() -> CounterBackend &;

WAGGLE_NAMESPACE_END
