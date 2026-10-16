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
#    include <fcntl.h>
#    include <link.h>
#    include <linux/perf_event.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <sys/syscall.h>
#    include <unistd.h>

#    include "CounterBackend.hpp"
#    include "Detail/JsonEscape.hpp"
#    include "Process.hpp"
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

auto CodeIndex::caller_of(Sample const &sample) -> Caller {
    if (sample.chain_size < 2 || has_lines(sample.chain[0])) {
        return {};
    }
    for (uint32_t i = 1; i < sample.chain_size; ++i) {
        if (has_lines(sample.chain[i])) {
            // Return addresses point past their call, often at the next line's code: one less is
            // inside the call. The leaf is the sample's own address, already inside its instruction.
            return {.caller = sample.chain[i] - 1, .callee = i == 1 ? sample.chain[0] : sample.chain[i - 1] - 1};
        }
    }
    return {};
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
        // With PERF_SAMPLE_CALLCHAIN, the frame count and the frames follow. The kernel marks where
        // each context's frames begin (PERF_CONTEXT_USER, ...) with values in the top 4095.
        if (size >= sizeof(SampleRecord) + sizeof(uint64_t)) {
            auto const *at = static_cast<unsigned char const *>(bytes) + sizeof(SampleRecord);
            uint64_t    nr = 0;
            std::memcpy(&nr, at, sizeof(nr));
            nr                               = std::min<uint64_t>(nr, (size - sizeof(SampleRecord) - sizeof(nr)) / sizeof(uint64_t));
            constexpr uint64_t kFirstContext = ~uint64_t{4095} + 1; // PERF_CONTEXT_MAX, -4095
            for (uint64_t i = 0; i < nr && sample.chain_size < kMaxChain; ++i) {
                uint64_t frame = 0;
                std::memcpy(&frame, at + sizeof(nr) + i * sizeof(frame), sizeof(frame));
                if (frame < kFirstContext) {
                    sample.chain[sample.chain_size++] = frame;
                }
            }
        }
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

struct Module {
    std::string                                  path;
    std::string                                  build_id;
    uintptr_t                                    base{0};
    int64_t                                      size{0};     ///< of the file, for one without a build id
    int64_t                                      mtime_ns{0}; ///< likewise
    std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
};

/// The GNU build id in the note segment at @p notes, @p size bytes long and aligned to @p align.
auto build_id_in(unsigned char const *notes, std::size_t size, std::size_t align) -> std::string {
    auto const  padded = [align](std::size_t n) { return (n + align - 1) / align * align; };
    std::size_t at     = 0;
    while (at + sizeof(ElfW(Nhdr)) <= size) {
        ElfW(Nhdr) note{};
        std::memcpy(&note, notes + at, sizeof(note));
        std::size_t const name = at + sizeof(note);
        std::size_t const desc = name + padded(note.n_namesz);
        if (desc + note.n_descsz > size) {
            break;
        }
        if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && std::memcmp(notes + name, "GNU", 4) == 0) {
            std::string hex;
            for (std::size_t i = 0; i < note.n_descsz; ++i) {
                hex += fmt::format("{:02x}", notes[desc + i]);
            }
            return hex;
        }
        at = desc + padded(note.n_descsz);
    }
    return {};
}

auto add_module(dl_phdr_info *info, std::size_t /*size*/, void *data) -> int {
    auto  &modules = *static_cast<std::vector<Module> *>(data);
    Module module{.path = info->dlpi_name != nullptr ? info->dlpi_name : "", .build_id = {}, .base = info->dlpi_addr};
    // The loader names every object but the executable, which comes first.
    if (module.path.empty() && modules.empty()) {
        module.path = executable_path();
    }
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        ElfW(Phdr) const &phdr  = info->dlpi_phdr[i];
        auto const        start = info->dlpi_addr + phdr.p_vaddr;
        if (phdr.p_type == PT_LOAD && (phdr.p_flags & PF_X) != 0) {
            module.ranges.emplace_back(start, start + phdr.p_memsz);
        } else if (phdr.p_type == PT_NOTE && module.build_id.empty()) {
            module.build_id = build_id_in(reinterpret_cast<unsigned char const *>(start), phdr.p_memsz, phdr.p_align == 8 ? 8 : 4);
        }
    }
    // Many toolchains link no build id (conda-forge's among them): the file's size and time then
    // tell a rebuilt file from the one that ran, as long as it was not rebuilt while running.
    if (struct stat st{}; module.build_id.empty() && stat(module.path.c_str(), &st) == 0) {
        module.size     = st.st_size;
        module.mtime_ns = int64_t{st.st_mtim.tv_sec} * 1'000'000'000 + st.st_mtim.tv_nsec;
    }
    if (!module.ranges.empty()) {
        modules.push_back(std::move(module));
    }
    return 0;
}

/// Whether the ELF file at @p path has a .debug_line section: whether it was built with line
/// information. Read with pread, the section headers and their names only.
auto file_has_lines(std::string const &path) -> bool {
    int const fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool found = false;
    ElfW(Ehdr) ehdr{};
    if (pread(fd, &ehdr, sizeof(ehdr), 0) == sizeof(ehdr) && std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) == 0 &&
        ehdr.e_shentsize == sizeof(ElfW(Shdr)) && ehdr.e_shstrndx < ehdr.e_shnum) {
        std::vector<ElfW(Shdr)> sections(ehdr.e_shnum);
        auto const              bytes = static_cast<ssize_t>(sections.size() * sizeof(ElfW(Shdr)));
        if (pread(fd, sections.data(), static_cast<size_t>(bytes), static_cast<off_t>(ehdr.e_shoff)) == bytes) {
            ElfW(Shdr) const &names = sections[ehdr.e_shstrndx];
            std::string       text(names.sh_size, '\0');
            if (pread(fd, text.data(), text.size(), static_cast<off_t>(names.sh_offset)) == static_cast<ssize_t>(text.size())) {
                for (auto const &section : sections) {
                    if (section.sh_name < text.size() && std::strcmp(text.c_str() + section.sh_name, ".debug_line") == 0) {
                        found = section.sh_size > 0;
                        break;
                    }
                }
            }
        }
    }
    close(fd);
    return found;
}

} // namespace

void CodeIndex::list_modules() {
    std::vector<Module> modules;
    dl_iterate_phdr(&add_module, &modules);
    _ranges.clear();
    for (auto const &module : modules) {
        auto [entry, added] = _lines_of_file.try_emplace(module.path, false);
        if (added) {
            entry->second = module.path.starts_with('/') && file_has_lines(module.path);
        }
        for (auto const &[start, end] : module.ranges) {
            _ranges.push_back({.start = start, .end = end, .lines = entry->second});
        }
    }
    std::ranges::sort(_ranges, {}, &Range::start);
    _listed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

auto CodeIndex::has_lines(uint64_t address) -> bool {
    auto const find = [&] {
        auto it = std::ranges::upper_bound(_ranges, address, {}, &Range::start);
        return it == _ranges.begin() || address >= std::prev(it)->end ? nullptr : &*std::prev(it);
    };
    Range const *range = find();
    if (range == nullptr) {
        // A library loaded since the modules were listed: list them again, at most once a second.
        auto const now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (_listed_ns == 0 || now - _listed_ns >= 1'000'000'000) {
            list_modules();
            range = find();
        }
    }
    return range != nullptr && range->lines;
}

auto machine_json() -> std::string {
    std::string   model = "unknown";
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) {
            model = line.substr(std::min(line.size(), line.find(':') + 2));
            break;
        }
    }
    std::unordered_set<std::string> sockets;
    long const                      online = sysconf(_SC_NPROCESSORS_CONF);
    for (long cpu = 0; cpu < online; ++cpu) {
        std::string   id;
        std::ifstream in(fmt::format("/sys/devices/system/cpu/cpu{}/topology/physical_package_id", cpu));
        if (in >> id) {
            sockets.insert(id);
        }
    }
    char host[256] = {}; // NOLINT(modernize-avoid-c-arrays)
    gethostname(host, sizeof(host) - 1);
    return fmt::format(R"({{"host": "{}", "cpu": "{}", "sockets": {}, "cpus": {}}})", detail::json_escape(host), detail::json_escape(model),
                       std::max<size_t>(1, sockets.size()), online);
}

auto module_map_json() -> std::string {
    std::vector<Module> modules;
    dl_iterate_phdr(&add_module, &modules);
    std::string out = "[";
    for (auto const &module : modules) {
        out += fmt::format(R"({}{{"path": "{}", "build_id": "{}", "size": {}, "mtime_ns": {}, "base": "0x{:x}", "ranges": [)",
                           out.size() > 1 ? ", " : "", detail::json_escape(module.path), module.build_id, module.size, module.mtime_ns,
                           module.base);
        for (std::size_t i = 0; i < module.ranges.size(); ++i) {
            out += fmt::format(R"({}["0x{:x}", "0x{:x}"])", i > 0 ? ", " : "", module.ranges[i].first, module.ranges[i].second);
        }
        out += "]}";
    }
    return out + "]";
}

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

auto Sampler::start(uint64_t rate_hz, bool callchain) -> bool {
    _rings.clear();
    _refused.clear();
    _refused_count = 0;
    _lost          = 0;
    _why_not.clear();
    _hardware  = true;
    _rate      = rate_hz;
    _callchain = callchain;
    _running   = false;
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
    attr.sample_type              = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | (_callchain ? PERF_SAMPLE_CALLCHAIN : 0);
    attr.sample_max_stack         = _callchain ? kMaxChain : 0; // user frames, from frame pointers
    attr.exclude_kernel           = 1;                          // user space only, which kernel.perf_event_paranoid 2 allows
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

auto CodeIndex::has_lines(uint64_t /*address*/) -> bool {
    return false;
}

void CodeIndex::list_modules() {
}

auto module_map_json() -> std::string {
    return "[]";
}

auto machine_json() -> std::string {
    return "{}";
}

struct Sampler::Ring {};

Sampler::Sampler() = default;

Sampler::~Sampler() = default;

auto Sampler::start(uint64_t rate_hz, bool callchain) -> bool {
    _rate      = rate_hz;
    _callchain = callchain;
    _why_not   = "sampling uses Linux perf_event, which this platform does not have";
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
