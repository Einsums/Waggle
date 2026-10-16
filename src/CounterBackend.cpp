//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "CounterBackend.hpp"

#include <Waggle/Config.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
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

} // namespace

auto perf_refusal(int error) -> std::string {
    std::string paranoid;
    std::ifstream("/proc/sys/kernel/perf_event_paranoid") >> paranoid;
    return perf_refusal(error, paranoid);
}

auto perf_refusal(int error, std::string const &paranoid) -> std::string {
    switch (error) {
    case EACCES:
    case EPERM: {
        // At 2 or less the kernel lets a process watch its own threads, so a refusal then comes from
        // something else: a container runtime's seccomp profile, which blocks the call outright.
        int level = 3;
        try {
            level = std::stoi(paranoid);
        } catch (...) { // NOLINT(bugprone-empty-catch): unreadable, so the kernel's level is the likely cause
        }
        if (level <= 2) {
            return fmt::format("perf_event_open was refused although kernel.perf_event_paranoid is {}: a container's seccomp "
                               "profile blocks it unless the container is given CAP_PERFMON",
                               paranoid);
        }
        return fmt::format("perf_event_open was refused (kernel.perf_event_paranoid is {}; 2 or less lets a process count its own "
                           "threads: sysctl kernel.perf_event_paranoid=2)",
                           paranoid.empty() ? "unknown" : paranoid);
    }
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

namespace {

auto one_per_slot(std::vector<PerfCounterBackend::Counter> const &counters) -> std::vector<PerfCounterBackend::Slot> {
    std::vector<PerfCounterBackend::Slot> slots;
    for (size_t i = 0; i < counters.size(); ++i) {
        slots.push_back({.name = counters[i].name, .terms = {{static_cast<int>(i), 1}}});
    }
    return slots;
}

auto codes_of(std::vector<PerfCounterBackend::Counter> const &counters) -> std::vector<PerfCounterBackend::EventCode> {
    std::vector<PerfCounterBackend::EventCode> codes;
    for (auto const &c : counters) {
        codes.push_back({.type = c.type, .config = c.config});
    }
    return codes;
}

/// The counters source's set: one event per slot.
auto default_counters() -> std::vector<PerfCounterBackend::Counter> {
    return {{"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
            {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
            {"cache-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
            {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES}};
}

/// Threads whose groups take turns on the PMU, for the roofline source's status.
std::atomic<uint64_t> g_multiplexed{0};

/// A group's reading: PERF_FORMAT_GROUP with both times, then each event's count.
struct GroupReading {
    uint64_t nr;
    uint64_t time_enabled;
    uint64_t time_running;
    uint64_t values[kMaxCounterEvents]; // NOLINT(modernize-avoid-c-arrays)
};

} // namespace

PerfCounterBackend::PerfCounterBackend() : PerfCounterBackend(default_counters()) {
}

PerfCounterBackend::PerfCounterBackend(std::vector<Counter> counters) {
    counters.resize(std::min<size_t>(counters.size(), kNumCounterSlots));
    _events = codes_of(counters);
    _slots  = one_per_slot(counters);
}

PerfCounterBackend::PerfCounterBackend(std::vector<EventCode> events, std::vector<Slot> slots, int split_after)
    : _events(std::move(events)), _slots(std::move(slots)), _split_after(split_after) {
    _events.resize(std::min<size_t>(_events.size(), kMaxCounterEvents));
    _slots.resize(std::min<size_t>(_slots.size(), kNumCounterSlots));
}

auto PerfCounterBackend::reconfigure(std::vector<EventCode> events, std::vector<Slot> slots, std::string description) -> bool {
    std::scoped_lock const lock(_mutex);
    if (_opened) {
        return false;
    }
    _events      = std::move(events);
    _slots       = std::move(slots);
    _description = std::move(description);
    _events.resize(std::min<size_t>(_events.size(), kMaxCounterEvents));
    _slots.resize(std::min<size_t>(_slots.size(), kNumCounterSlots));
    return true;
}

auto PerfCounterBackend::describe() const -> std::string {
    std::scoped_lock const lock(_mutex);
    return _description;
}

auto PerfCounterBackend::open_group(ThreadCounters &tc, int first, int end) -> bool {
    for (int i = first; i < end; ++i) {
        struct perf_event_attr pe{};
        pe.type           = _events[static_cast<size_t>(i)].type;
        pe.size           = sizeof(pe);
        pe.config         = _events[static_cast<size_t>(i)].config;
        pe.disabled       = i == first ? 1 : 0; // the group starts when its leader is enabled
        pe.exclude_kernel = 1;
        pe.exclude_hv     = 1;
        // One read() returns the whole group, with the time it was enabled and the time it ran on
        // the PMU, which differ only when groups take turns.
        pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        int const fd   = static_cast<int>(perf_event_open(&pe, 0, -1, i == first ? -1 : tc.fds[first], 0));
        if (fd < 0) {
            std::string            why = perf_refusal(errno);
            std::scoped_lock const lock(_mutex);
            _why_not = std::move(why);
            return false;
        }
        tc.fds[i] = fd;
        tc.count  = i + 1;
    }
    return true;
}

auto PerfCounterBackend::open(ThreadCounters &tc) -> bool {
    if (tc.open) {
        return true;
    }
    std::vector<EventCode> events;
    int                    split = 0;
    {
        std::scoped_lock const lock(_mutex);
        _opened = true;
        events  = _events;
        split   = _split_after;
    }
    int const n = static_cast<int>(events.size());
    if (n == 0) {
        std::scoped_lock const lock(_mutex);
        _why_not = "no counters to open";
        return false;
    }
    auto const start = [&](int groups, int at) {
        tc.groups         = groups;
        tc.group_start[0] = 0;
        tc.group_start[1] = groups == 2 ? at : n;
        tc.group_start[2] = n;
        for (int g = 0; g < groups; ++g) {
            if (!open_group(tc, tc.group_start[g], tc.group_start[g + 1])) {
                close(tc);
                return false;
            }
        }
        for (int g = 0; g < groups; ++g) {
            int const leader = tc.fds[tc.group_start[g]];
            ioctl(leader, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
            ioctl(leader, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        }
        return true;
    };
    if (split > 0 && split < n) {
        if (!start(2, split)) {
            return false;
        }
    } else {
        if (!start(1, n)) {
            return false;
        }
        // A group the PMU cannot hold at once is never scheduled: enabled, it never runs. Then two
        // halves, which it can hold in turns, the fixed counters (cycles, instructions) in the first.
        GroupReading reading{};
        if (::read(tc.fds[0], &reading, sizeof(reading)) > 0 && reading.time_enabled > 0 && reading.time_running == 0) {
            close(tc);
            if (!start(2, (n + 1) / 2)) {
                return false;
            }
        }
    }
    tc.multiplexed = tc.groups > 1;
    if (tc.multiplexed) {
        g_multiplexed.fetch_add(1, std::memory_order_relaxed);
    }
    tc.last.fill(0);
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
    tc.count       = 0;
    tc.groups      = 0;
    tc.open        = false;
    tc.multiplexed = false;
}

void PerfCounterBackend::read(ThreadCounters const &tc, std::array<uint64_t, kNumCounterSlots> &values) {
    values.fill(0);
    if (!tc.open) {
        return;
    }
    // Each event's count, scaled by the share of its group's time on the PMU when groups take
    // turns: the kernel's estimate of what it would have counted alone.
    double counts[kMaxCounterEvents]{}; // NOLINT(modernize-avoid-c-arrays)
    for (int g = 0; g < tc.groups; ++g) {
        GroupReading reading{};
        if (::read(tc.fds[tc.group_start[g]], &reading, sizeof(reading)) < static_cast<ssize_t>(3 * sizeof(uint64_t))) {
            return;
        }
        double const scale = reading.time_running == 0 || reading.time_running == reading.time_enabled
                                 ? (reading.time_running == 0 ? 0.0 : 1.0)
                                 : static_cast<double>(reading.time_enabled) / static_cast<double>(reading.time_running);
        auto const   n     = std::min<uint64_t>(reading.nr, static_cast<uint64_t>(tc.group_start[g + 1] - tc.group_start[g]));
        for (uint64_t i = 0; i < n; ++i) {
            counts[tc.group_start[g] + static_cast<int>(i)] = static_cast<double>(reading.values[i]) * scale;
        }
    }
    std::vector<Slot> slots;
    {
        std::scoped_lock const lock(_mutex);
        slots = _slots;
    }
    for (size_t s = 0; s < slots.size() && s < static_cast<size_t>(kNumCounterSlots); ++s) {
        double sum = 0.0;
        for (auto const &[event, weight] : slots[s].terms) {
            if (event >= 0 && event < tc.count) {
                sum += counts[event] * static_cast<double>(weight);
            }
        }
        // Never below the last reading, so a zone's difference never wraps.
        values[s]  = std::max(static_cast<uint64_t>(sum), tc.last[s]);
        tc.last[s] = values[s];
    }
}

auto PerfCounterBackend::slot_name(int slot) const -> std::string {
    std::scoped_lock const lock(_mutex);
    return slot >= 0 && static_cast<size_t>(slot) < _slots.size() ? _slots[static_cast<size_t>(slot)].name : std::string{};
}

auto PerfCounterBackend::why_not() const -> std::string {
    std::scoped_lock const lock(_mutex);
    return _why_not;
}

namespace {

std::mutex  g_roofline_mutex;
std::string g_roofline; ///< what the roofline set counts, once chosen

/// The CPU's vendor, family and model, from the first processor in /proc/cpuinfo.
struct CpuId {
    std::string vendor;
    int         family{0};
    int         model{0};
};

auto cpu_id() -> CpuId {
    CpuId         id;
    std::ifstream in("/proc/cpuinfo");
    std::string   line;
    while (std::getline(in, line) && (id.vendor.empty() || id.family == 0 || id.model == 0)) {
        auto const colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto key   = line.substr(0, line.find_last_not_of(" \t", colon - 1) + 1);
        auto value = line.substr(std::min(line.size(), colon + 2));
        if (key == "vendor_id") {
            id.vendor = value;
        } else if (key == "cpu family") {
            id.family = std::atoi(value.c_str());
        } else if (key == "model") {
            id.model = std::atoi(value.c_str());
        }
    }
    return id;
}

/// Intel cores whose FP_ARITH_INST_RETIRED (event 0xC7) counts packed and scalar FP instructions
/// by width, one umask bit each, from Broadwell on: server parts and client parts without
/// efficiency cores, whose PMU is another.
auto intel_fp_arith(int model) -> bool {
    static constexpr int kModels[] = {
        0x3D, 0x47, 0x4F, 0x56,       // Broadwell
        0x4E, 0x5E, 0x55,             // Skylake, Skylake-SP and its Cascade and Cooper Lake steppings
        0x8E, 0x9E, 0xA5, 0xA6, 0x66, // Kaby, Coffee, Comet, Cannon Lake
        0x7D, 0x7E, 0x6A, 0x6C,       // Ice Lake and Ice Lake-SP
        0x8C, 0x8D, 0xA7,             // Tiger, Rocket Lake
        0x8F, 0xCF,                   // Sapphire, Emerald Rapids
        0xAD, 0xAE,                   // Granite Rapids
    };
    return std::ranges::find(kModels, model) != std::end(kModels);
}

} // namespace

auto use_roofline_counters(std::string const &precision, std::string &why_not) -> bool {
    if (precision != "fp64" && precision != "fp32") {
        why_not = fmt::format("roofline_precision is \"{}\"; it takes fp64 or fp32", precision);
        return false;
    }
    {
        // Chosen already, as apply runs again with each change of settings.
        std::scoped_lock const lock(g_roofline_mutex);
        if (!g_roofline.empty() && g_roofline.find(fmt::format("({})", precision)) != std::string::npos) {
            return true;
        }
    }
    auto const cpu = cpu_id();
    if (cpu.vendor != "GenuineIntel" || cpu.family != 6 || !intel_fp_arith(cpu.model)) {
        why_not = fmt::format("Waggle knows the FLOP events of Intel cores from Broadwell on, not this CPU's ({} family {} model {:#x}); "
                              "AMD and Arm wait for their events to be checked on real machines",
                              cpu.vendor.empty() ? "unknown" : cpu.vendor, cpu.family, cpu.model);
        return false;
    }
    // FP_ARITH_INST_RETIRED umasks, scalar then 128-, 256- and 512-bit packed, with the operations
    // each instruction does; an FMA already counts twice.
    bool const     fp64    = precision == "fp64";
    uint64_t const umask[] = {fp64 ? 0x01U : 0x02U, fp64 ? 0x04U : 0x08U, fp64 ? 0x10U : 0x20U, fp64 ? 0x40U : 0x80U};
    uint64_t const lanes[] = {1, fp64 ? 2U : 4U, fp64 ? 4U : 8U, fp64 ? 8U : 16U};
    std::vector<PerfCounterBackend::EventCode> events = {{.type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CPU_CYCLES},
                                                         {.type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_INSTRUCTIONS}};
    PerfCounterBackend::Slot                   flops{.name = precision + "_flops", .terms = {}};
    for (int i = 0; i < 4; ++i) {
        flops.terms.emplace_back(static_cast<int>(events.size()), lanes[i]);
        events.push_back({.type = PERF_TYPE_RAW, .config = (umask[i] << 8) | 0xC7});
    }
    events.push_back({.type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CACHE_MISSES});
    std::vector<PerfCounterBackend::Slot> slots = {
        {"cycles", {{0, 1}}}, {"instructions", {{1, 1}}}, std::move(flops), {"llc_miss_bytes", {{6, 64}}}};
    std::string const description =
        fmt::format("Linux perf_event: Intel FP_ARITH_INST_RETIRED ({}), and last-level cache misses times 64 bytes as an estimate "
                    "of DRAM bytes",
                    precision);
    auto &backend = dynamic_cast<PerfCounterBackend &>(get_counter_backend());
    if (!backend.reconfigure(std::move(events), std::move(slots), description)) {
        why_not = "asked for after threads began counting with the counters source's set; set WAGGLE_SOURCES=roofline in the "
                  "environment, or configure Waggle before threads record";
        return false;
    }
    std::scoped_lock const lock(g_roofline_mutex);
    g_roofline = description;
    return true;
}

auto roofline_counters() -> std::string {
    std::scoped_lock const lock(g_roofline_mutex);
    return g_roofline;
}

auto multiplexed_threads() -> uint64_t {
    return g_multiplexed.load(std::memory_order_relaxed);
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
