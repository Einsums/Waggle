//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Sampling.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Clock.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

#if defined(__linux__)
#    include <ctime>
#    include <dirent.h>
#    include <linux/perf_event.h>
#    include <sys/mman.h>
#    include <sys/syscall.h>
#    include <unistd.h>

#    include "CounterBackend.hpp"
#endif

WAGGLE_NAMESPACE_BEGIN

namespace sampling {

auto ClockPair::now() -> ClockPair {
    // The tick count between two reads of the other clock, against their midpoint. A thread
    // preempted between the reads would pair instants milliseconds apart, so the closest of a few
    // tries is kept, and a close one ends the search.
    ClockPair best{};
    auto      best_gap = std::chrono::steady_clock::duration::max();
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto const before = std::chrono::steady_clock::now();
        auto const ticks  = TickClock::now();
        auto const after  = std::chrono::steady_clock::now();
        if (after - before < best_gap) {
            best_gap       = after - before;
            auto const mid = before + (after - before) / 2;
            best = {.ticks = ticks,
                    .ns    = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(mid.time_since_epoch()).count())};
        }
        if (best_gap < std::chrono::microseconds(1)) {
            break;
        }
    }
    return best;
}

auto monotonic_to_ticks(uint64_t ns, ClockPair const &anchor) -> uint64_t {
    auto const delta = static_cast<double>(static_cast<int64_t>(ns - anchor.ns)) / TickClock::instance().ns_per_tick;
    return anchor.ticks + static_cast<uint64_t>(std::llround(delta));
}

namespace {

// The records a ring holds, as linux/perf_event.h lays them out, spelled here so the parser builds
// and is tested everywhere. A ring opened with PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME.
struct RecordHeader {
    uint32_t type;
    uint16_t misc;
    uint16_t size;
};

constexpr uint32_t kRecordLost   = 2; // PERF_RECORD_LOST
constexpr uint32_t kRecordSample = 9; // PERF_RECORD_SAMPLE

struct SampleRecord {
    RecordHeader header;
    uint64_t     ip;
    uint32_t     pid;
    uint32_t     tid;
    uint64_t     time;
};

struct LostRecord {
    RecordHeader header;
    uint64_t     id;
    uint64_t     lost;
};

} // namespace

auto parse_record(void const *bytes, std::size_t size, Sample &sample, uint64_t &lost) -> Record {
    RecordHeader header{};
    if (size < sizeof(header)) {
        return Record::Malformed;
    }
    std::memcpy(&header, bytes, sizeof(header));
    switch (header.type) {
    case kRecordSample: {
        if (size < sizeof(SampleRecord)) {
            return Record::Malformed;
        }
        SampleRecord record{};
        std::memcpy(&record, bytes, sizeof(record));
        sample = {.time_ns = record.time, .ip = record.ip, .tid = record.tid};
        return Record::Sample;
    }
    case kRecordLost: {
        if (size < sizeof(LostRecord)) {
            return Record::Malformed;
        }
        LostRecord record{};
        std::memcpy(&record, bytes, sizeof(record));
        lost += record.lost;
        return Record::Lost;
    }
    default:
        return Record::Other;
    }
}

#if defined(__linux__)

static_assert(sizeof(RecordHeader) == sizeof(perf_event_header));

namespace {

/// Bytes of samples each thread's ring holds: about 2 s of samples at the default rate.
constexpr std::size_t kRingBytes = 64 * 1024;

auto perf_event_open(perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags) -> int {
    return static_cast<int>(syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags));
}

/// The process's threads now.
auto task_ids() -> std::vector<uint32_t> {
    std::vector<uint32_t> ids;
    DIR                  *dir = opendir("/proc/self/task");
    if (dir == nullptr) {
        return ids;
    }
    while (dirent const *entry = readdir(dir)) { // NOLINT(concurrency-mt-unsafe): the consumer's own DIR
        char *end = nullptr;
        auto  id  = std::strtoul(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0') {
            ids.push_back(static_cast<uint32_t>(id));
        }
    }
    closedir(dir);
    return ids;
}

/// Why the ring's mmap failed: past the memory perf lets a user lock, or something else.
auto mmap_refusal(int error) -> std::string {
    if (error == EPERM || error == ENOMEM) {
        return fmt::format("the sampling ring could not be mapped ({}): a user may lock only kernel.perf_event_mlock_kb of perf "
                           "rings, shared by every process of theirs",
                           std::strerror(error));
    }
    return fmt::format("the sampling ring could not be mapped: {}", std::strerror(error));
}

/// The largest rate the kernel takes, which it may have lowered after slow sampling interrupts.
auto max_rate() -> uint64_t {
    uint64_t rate = 0;
    std::ifstream("/proc/sys/kernel/perf_event_max_sample_rate") >> rate;
    return rate;
}

} // namespace

struct Sampler::Ring {
    int         fd{-1};
    void       *base{MAP_FAILED};
    std::size_t length{0};

    Ring()                        = default;
    Ring(Ring const &)            = delete;
    Ring &operator=(Ring const &) = delete;
    ~Ring() {
        if (base != MAP_FAILED) {
            munmap(base, length);
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

Sampler::Sampler() = default;

Sampler::~Sampler() = default;

auto Sampler::start(uint64_t rate_hz) -> bool {
    _rings.clear();
    _refused.clear();
    _refused_count = 0;
    _lost          = 0;
    _why_not.clear();
    _hardware = true;
    _rate     = rate_hz;
    _running  = false;
    if (rate_hz == 0) {
        _why_not = "sampling_rate must be at least 1";
        return false;
    }
    if (auto const most = max_rate(); most > 0 && rate_hz > most) {
        _why_not = fmt::format("sampling_rate {} is above kernel.perf_event_max_sample_rate, {}", rate_hz, most);
        return false;
    }
    for (uint32_t const tid : task_ids()) {
        if (!_excluded.contains(tid)) {
            open_thread(tid);
        }
    }
    _running = !_rings.empty();
    return _running;
}

void Sampler::stop(Sink const &sink) {
    drain(sink);
    _rings.clear();
    _running = false;
}

void Sampler::exclude(uint32_t tid) {
    _excluded.insert(tid);
    _rings.erase(tid);
}

auto Sampler::open_thread(uint32_t tid) -> bool {
    perf_event_attr attr{};
    attr.size                     = sizeof(attr);
    attr.type                     = _hardware ? PERF_TYPE_HARDWARE : PERF_TYPE_SOFTWARE;
    attr.config                   = _hardware ? uint64_t{PERF_COUNT_HW_CPU_CYCLES} : uint64_t{PERF_COUNT_SW_CPU_CLOCK};
    attr.freq                     = 1;
    attr.sample_freq              = _rate;
    attr.sample_type              = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
    attr.exclude_kernel           = 1; // user space only, which kernel.perf_event_paranoid 2 allows
    attr.exclude_hv               = 1;
    attr.exclude_callchain_kernel = 1;
    attr.use_clockid              = 1; // the clock zones are on, so every sample finds its zone
    attr.clockid                  = CLOCK_MONOTONIC;

    int fd = perf_event_open(&attr, static_cast<pid_t>(tid), -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0 && _hardware && (errno == ENOENT || errno == EOPNOTSUPP || errno == ENODEV)) {
        // No PMU, as on many virtual machines: the software clock samples by time on the CPU too.
        _hardware   = false;
        attr.type   = PERF_TYPE_SOFTWARE;
        attr.config = PERF_COUNT_SW_CPU_CLOCK;
        fd          = perf_event_open(&attr, static_cast<pid_t>(tid), -1, -1, PERF_FLAG_FD_CLOEXEC);
    }
    if (fd < 0) {
        // A thread that ended since the scan is not a refusal.
        if (errno != ESRCH) {
            _why_not = perf_refusal(errno);
            ++_refused_count;
        }
        _refused.insert(tid);
        return false;
    }

    auto ring = std::make_unique<Ring>();
    ring->fd  = fd;
    // One page of metadata, then a power-of-two number of pages of samples.
    auto const page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    ring->length    = page * (1 + std::max<std::size_t>(1, kRingBytes / page));
    ring->base      = mmap(nullptr, ring->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring->base == MAP_FAILED) {
        _why_not = mmap_refusal(errno);
        ++_refused_count;
        _refused.insert(tid);
        return false;
    }
    _rings[tid] = std::move(ring);
    return true;
}

void Sampler::rescan(Sink const &sink) {
    if (!_running) {
        return;
    }
    auto const                         ids = task_ids();
    std::unordered_set<uint32_t> const live(ids.begin(), ids.end());
    for (auto it = _rings.begin(); it != _rings.end();) {
        if (!live.contains(it->first)) {
            drain_ring(*it->second, sink);
            it = _rings.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(_refused, [&](uint32_t tid) { return !live.contains(tid); });
    for (uint32_t const tid : ids) {
        if (!_rings.contains(tid) && !_refused.contains(tid) && !_excluded.contains(tid)) {
            open_thread(tid);
        }
    }
}

void Sampler::drain(Sink const &sink) {
    for (auto &[tid, ring] : _rings) {
        drain_ring(*ring, sink);
    }
}

void Sampler::drain_ring(Ring &ring, Sink const &sink) {
    auto *meta = static_cast<perf_event_mmap_page *>(ring.base);
    auto *data = static_cast<unsigned char *>(ring.base) + meta->data_offset;
    auto  size = meta->data_size;
    // The kernel writes up to data_head, then the reader frees up to data_tail: the acquire pairs
    // with the kernel's store of data_head, the release keeps the reads before the tail moves.
    uint64_t const head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t       tail = meta->data_tail;
    // A record can wrap past the ring's end, so each is copied out whole before it is read.
    auto const copy_out = [&](uint64_t at, void *into, std::size_t n) {
        auto const offset = static_cast<std::size_t>(at % size);
        auto const first  = std::min<std::size_t>(n, size - offset);
        std::memcpy(into, data + offset, first);
        std::memcpy(static_cast<unsigned char *>(into) + first, data, n - first);
    };
    while (tail < head) {
        perf_event_header header{};
        copy_out(tail, &header, sizeof(header));
        if (header.size < sizeof(header) || header.size > head - tail) {
            tail = head; // the ring is not what it should be: skip what it holds rather than misread it
            break;
        }
        _record.resize(header.size);
        copy_out(tail, _record.data(), header.size);
        Sample sample{};
        if (parse_record(_record.data(), _record.size(), sample, _lost) == Record::Sample) {
            sink(sample);
        }
        tail += header.size;
    }
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

auto Sampler::event() const -> std::string {
    return _hardware ? "cycles" : "cpu-clock";
}

#else

struct Sampler::Ring {};

Sampler::Sampler() = default;

Sampler::~Sampler() = default;

auto Sampler::start(uint64_t rate_hz) -> bool {
    _rate    = rate_hz;
    _why_not = "sampling uses Linux perf_event, which this platform does not have";
    return false;
}

void Sampler::stop(Sink const & /*sink*/) {
    _running = false;
}

void Sampler::exclude(uint32_t tid) {
    _excluded.insert(tid);
}

void Sampler::rescan(Sink const & /*sink*/) {
}

void Sampler::drain(Sink const & /*sink*/) {
}

auto Sampler::event() const -> std::string {
    return "none";
}

#endif

} // namespace sampling

WAGGLE_NAMESPACE_END
