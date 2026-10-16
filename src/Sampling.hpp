//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

WAGGLE_NAMESPACE_BEGIN

namespace sampling {

/// Frames of a call chain kept per sample.
inline constexpr std::size_t kMaxChain = 32;

/// Where one thread was when the kernel interrupted it, and when.
struct Sample {
    uint64_t time_ns; ///< CLOCK_MONOTONIC, the clock steady_clock reads on Linux
    uint64_t ip;      ///< the user-space instruction address
    uint32_t tid;     ///< the kernel's id for the thread
    /// The user-space call chain, from frame pointers, innermost first: the sample's own address,
    /// then each caller's return address. Empty when call chains are off.
    uint32_t                        chain_size{0};
    std::array<uint64_t, kMaxChain> chain{};
};

/// Where a sample in code without line information was called from: the innermost frame of its
/// chain in code with line information, and the frame just inside that call, both as addresses
/// within their instructions (a return address less one). Zeros when the sample's own code has
/// line information, or no frame of its chain does.
struct Caller {
    uint64_t caller{0};
    uint64_t callee{0};
};

/**
 * @brief Which loaded code has line information, by module, to credit a library's samples to the
 * line that called it.
 *
 * A module has line information when its file has a .debug_line section; each file is read once.
 * The process's modules are listed again when an address falls in none of them (a library loaded
 * since), at most once a second. Not thread-safe: the consumer drives it.
 */
class WAGGLE_EXPORT CodeIndex {
  public:
    /// The caller of @p sample, as @ref Caller says.
    auto caller_of(Sample const &sample) -> Caller;
    /// Whether @p address is in a module whose file has line information.
    auto has_lines(uint64_t address) -> bool;

  private:
    struct Range {
        uint64_t start;
        uint64_t end;
        bool     lines;
    };
    std::vector<Range>                    _ranges; ///< by start
    std::unordered_map<std::string, bool> _lines_of_file;
    int64_t                               _listed_ns{0}; ///< when the modules were last listed
    void                                  list_modules();
};

/// What one perf ring record held.
enum class Record : std::uint8_t {
    Sample,    ///< a sample, written to the sample argument
    Lost,      ///< samples the kernel dropped on a full ring, added to the lost argument
    Other,     ///< a record the sampler does not use (throttling, and the like)
    Malformed, ///< too short for its type
};

/// Read one record from a ring the sampler opened (samples of IP, TID and TIME, and CALLCHAIN when
/// call chains are on, which the record's length shows): @p bytes is the whole record, its
/// perf_event_header first, @p size its length. Exported for tests, which feed it fixture bytes.
WAGGLE_EXPORT auto parse_record(void const *bytes, std::size_t size, Sample &sample, uint64_t &lost) -> Record;

/// The two clocks read together: a tick count and the CLOCK_MONOTONIC instant beside it.
struct ClockPair {
    uint64_t ticks;
    uint64_t ns;
    /// Both clocks now.
    WAGGLE_EXPORT static auto now() -> ClockPair;
};

/// The CLOCK_MONOTONIC instant @p ns as TickClock ticks, the clock zones are stamped with, counted
/// from @p anchor. The TSC's rate is measured, not known, so the error grows with the distance
/// from the anchor: one read within milliseconds of @p ns keeps a sample on the right side of a
/// zone's edge, where the process's anchor, seconds or hours away, would not.
WAGGLE_EXPORT auto monotonic_to_ticks(uint64_t ns, ClockPair const &anchor) -> uint64_t;

/// The machine, as a JSON object for the session file: its ``host``, its ``cpu`` model, and its
/// ``sockets`` and ``cpus``, which name its entry in the machine file `waggle calibrate` writes.
WAGGLE_EXPORT auto machine_json() -> std::string;

/// The process's loaded code, as a JSON array for the session file: each module's ``path``, its
/// GNU ``build_id`` (hexadecimal; empty if it has none, and then the file's ``size`` and
/// ``mtime_ns`` instead, 0 otherwise), its load ``base`` and the address
/// ``ranges`` of its executable segments, so a sample's address names a module and an offset that
/// addr2line resolves, and a rebuilt file is told from the one that ran. Empty where the platform
/// has no dl_iterate_phdr.
WAGGLE_EXPORT auto module_map_json() -> std::string;

/**
 * @brief The sampling source's events: one perf_event sampling event per thread of the process.
 *
 * Every thread is sampled, not only those that record zones, as time in a BLAS library runs on
 * pool threads that never open one. Threads are found in /proc/self/task when sampling starts and
 * at each @ref rescan. Each event has its own 64 KB ring, which the kernel writes as it samples and
 * @ref drain reads; the thread being sampled pays nothing. Not thread-safe: the consumer drives it
 * under its tree lock.
 */
class WAGGLE_EXPORT Sampler {
  public:
    /// Receives each sample as the rings are read, oldest first per thread.
    using Sink = std::function<void(Sample const &)>;

    Sampler();
    ~Sampler();
    Sampler(Sampler const &)            = delete;
    Sampler &operator=(Sampler const &) = delete;

    /// Sample every thread of the process at @p rate_hz, hardware cycles where the machine has a
    /// PMU and the software CPU clock where it has none, with each sample's user-space call chain
    /// when @p callchain. False when no thread could be sampled; @ref why_not then says why.
    auto start(uint64_t rate_hz, bool callchain = true) -> bool;

    /// Read what the rings still hold into @p sink, then close every event.
    void stop(Sink const &sink);

    [[nodiscard]] auto running() const -> bool { return _running; }

    /// Never sample thread @p tid (the consumer's own), closing its event if one is open.
    void exclude(uint32_t tid);

    /// Open events for threads started since the last scan; read and close those of threads gone.
    void rescan(Sink const &sink);

    /// Read every ring into @p sink.
    void drain(Sink const &sink);

    /// The event sampled: "cycles", or "cpu-clock" on a machine without hardware counters.
    [[nodiscard]] auto event() const -> std::string;
    [[nodiscard]] auto rate() const -> uint64_t { return _rate; }
    [[nodiscard]] auto callchain() const -> bool { return _callchain; }
    /// Threads sampled now.
    [[nodiscard]] auto threads() const -> std::size_t { return _rings.size(); }
    /// Threads whose event could not be opened, since start.
    [[nodiscard]] auto refused() const -> uint64_t { return _refused_count; }
    /// Samples the kernel dropped because a ring was full, since start.
    [[nodiscard]] auto lost() const -> uint64_t { return _lost; }
    /// Why the last event could not be opened; empty if every one could.
    [[nodiscard]] auto why_not() const -> std::string const & { return _why_not; }

  private:
    struct Ring;

    /// Open thread @p tid's event; false, with @ref why_not set, if it cannot be.
    auto open_thread(uint32_t tid) -> bool;
    void drain_ring(Ring &ring, Sink const &sink);

    bool                                                _running{false};
    bool                                                _hardware{true};
    bool                                                _callchain{true};
    uint64_t                                            _rate{0};
    std::unordered_map<uint32_t, std::unique_ptr<Ring>> _rings;
    std::unordered_set<uint32_t>                        _refused; ///< threads not to try again
    std::unordered_set<uint32_t>                        _excluded;
    uint64_t                                            _refused_count{0};
    uint64_t                                            _lost{0};
    std::string                                         _why_not;
    std::vector<unsigned char>                          _record; ///< one record, copied out of a ring
};

} // namespace sampling

WAGGLE_NAMESPACE_END
