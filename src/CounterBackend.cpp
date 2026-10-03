//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "CounterBackend.hpp"

#include <Waggle/Config.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#if defined(__linux__)
#    include <linux/perf_event.h>
#    include <sys/ioctl.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#elif defined(__APPLE__)
#    include <sys/syscall.h>
#    include <sys/sysctl.h>
#    include <unistd.h>
#endif

WAGGLE_NAMESPACE_BEGIN

#if defined(__linux__)

namespace {

long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/// Why perf_event_open failed with @p error, in terms a person can act on.
auto refusal(int error) -> std::string {
    std::string paranoid;
    std::ifstream("/proc/sys/kernel/perf_event_paranoid") >> paranoid;
    switch (error) {
    case EACCES:
    case EPERM:
        return fmt::format("perf_event_open was refused (kernel.perf_event_paranoid is {}; 2 or less lets a process count its own "
                           "threads)",
                           paranoid.empty() ? "unknown" : paranoid);
    case ENOENT:
    case EOPNOTSUPP:
    case ENODEV:
        return "this machine has no hardware counters perf can open (a virtual machine often has none)";
    case ENOSYS:
        return "this kernel has no perf_event_open";
    default:
        return fmt::format("perf_event_open failed: {}", std::strerror(error));
    }
}

} // namespace

PerfCounterBackend::PerfCounterBackend()
    : PerfCounterBackend({{"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
                          {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
                          {"cache-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
                          {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES}}) {
}

PerfCounterBackend::PerfCounterBackend(std::vector<Counter> counters) : _counters(std::move(counters)) {
    _counters.resize(std::min<size_t>(_counters.size(), kNumCounterSlots));
}

auto PerfCounterBackend::open(ThreadCounters &tc) -> bool {
    if (tc.open) {
        return true;
    }
    for (size_t i = 0; i < _counters.size(); ++i) {
        struct perf_event_attr pe{};
        pe.type           = _counters[i].type;
        pe.size           = sizeof(pe);
        pe.config         = _counters[i].config;
        pe.disabled       = i == 0 ? 1 : 0; // the group starts when its leader is enabled
        pe.exclude_kernel = 1;
        pe.exclude_hv     = 1;
        pe.read_format    = PERF_FORMAT_GROUP; // one read() returns the whole group
        int const fd      = static_cast<int>(perf_event_open(&pe, 0, -1, i == 0 ? -1 : tc.fds[0], 0));
        if (fd < 0) {
            std::string why = refusal(errno);
            close(tc);
            std::scoped_lock const lock(_mutex);
            _why_not = std::move(why);
            return false;
        }
        tc.fds[i] = fd;
        tc.count  = static_cast<int>(i) + 1;
    }
    if (tc.count == 0) {
        std::scoped_lock const lock(_mutex);
        _why_not = "no counters to open";
        return false;
    }
    ioctl(tc.fds[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(tc.fds[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    tc.open = true;
    return true;
}

void PerfCounterBackend::close(ThreadCounters &tc) {
    for (int &fd : tc.fds) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    tc.count = 0;
    tc.open  = false;
}

void PerfCounterBackend::read(ThreadCounters const &tc, std::array<uint64_t, kNumCounterSlots> &values) {
    values.fill(0);
    if (!tc.open) {
        return;
    }
    // PERF_FORMAT_GROUP: the number of counters, then each value, in the order they were opened.
    uint64_t group[1 + kNumCounterSlots]{}; // NOLINT(modernize-avoid-c-arrays)
    if (::read(tc.fds[0], group, sizeof(group)) < static_cast<ssize_t>(sizeof(uint64_t))) {
        return;
    }
    auto const n = std::min<uint64_t>(group[0], kNumCounterSlots);
    for (uint64_t i = 0; i < n; ++i) {
        values[i] = group[1 + i];
    }
}

auto PerfCounterBackend::slot_name(int slot) const -> std::string {
    return slot >= 0 && static_cast<size_t>(slot) < _counters.size() ? _counters[static_cast<size_t>(slot)].name : std::string{};
}

auto PerfCounterBackend::why_not() const -> std::string {
    std::scoped_lock const lock(_mutex);
    return _why_not;
}

auto get_counter_backend() -> CounterBackend & {
    static PerfCounterBackend backend;
    return backend;
}

#elif defined(__APPLE__)

namespace {

/// XNU's thread_selfcounts: the calling thread's counts. Kind 2 gives instructions and cycles for
/// each kind of core, performance cores first. Not in the SDK's headers, but in its system call
/// table; a kernel without it fails the call, and the source reports itself unavailable.
constexpr int kSelfCountsByLevel = 2;

auto self_counts(uint64_t (&out)[8]) -> bool { // NOLINT(modernize-avoid-c-arrays)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations" // syscall() is deprecated, not removed
    return syscall(SYS_thread_selfcounts, kSelfCountsByLevel, out, sizeof(out)) == 0;
#    pragma clang diagnostic pop
}

} // namespace

auto running_in_vm() -> bool {
    int    present = 0;
    size_t size    = sizeof(present);
    return sysctlbyname("kern.hv_vmm_present", &present, &size, nullptr, 0) == 0 && present != 0;
}

auto XnuCounterBackend::open(ThreadCounters &tc) -> bool {
    uint64_t probe[8]{}; // NOLINT(modernize-avoid-c-arrays)
    if (!self_counts(probe)) {
        int const   error = errno;
        std::string why   = running_in_vm() ? "this Mac is a virtual machine, which has no performance counters"
                                            : fmt::format("this kernel does not give threads their cycle and instruction counts ({})",
                                                          std::strerror(error));
        std::scoped_lock const lock(_mutex);
        _why_not = std::move(why);
        return false;
    }
    tc.count = kNumCounterSlots;
    tc.open  = true;
    return true;
}

void XnuCounterBackend::close(ThreadCounters &tc) {
    tc.count = 0;
    tc.open  = false;
}

void XnuCounterBackend::read(ThreadCounters const &tc, std::array<uint64_t, kNumCounterSlots> &values) {
    values.fill(0);
    uint64_t counts[8]{}; // NOLINT(modernize-avoid-c-arrays): instructions and cycles per kind of core
    if (!tc.open || !self_counts(counts)) {
        return;
    }
    values[0] = counts[1] + counts[3]; // cycles
    values[1] = counts[0] + counts[2]; // instructions
    values[2] = counts[3];             // on efficiency cores
    values[3] = counts[2];
}

auto XnuCounterBackend::slot_name(int slot) const -> std::string {
    switch (slot) {
    case 0:
        return "cycles";
    case 1:
        return "instructions";
    case 2:
        return "e-core cycles";
    case 3:
        return "e-core instructions";
    default:
        return {};
    }
}

auto XnuCounterBackend::why_not() const -> std::string {
    std::scoped_lock const lock(_mutex);
    return _why_not;
}

auto get_counter_backend() -> CounterBackend & {
    static XnuCounterBackend backend;
    return backend;
}

#else

auto get_counter_backend() -> CounterBackend & {
    static NoopCounterBackend backend;
    return backend;
}

#endif

WAGGLE_NAMESPACE_END
