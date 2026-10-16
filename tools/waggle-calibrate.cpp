//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// waggle-calibrate: measures the machine's peaks for the roofline source's efficiencies, and prints
// them as JSON for `waggle calibrate` to keep in the machine file.
//
// Peaks come from measurement, not from a table of models, as turbo and AVX-512 frequency limits
// make a data sheet's numbers wrong by 20% or more:
// - FLOPs: every thread, pinned to a CPU of its own, runs independent chains of fused
//   multiply-adds at the widest width the CPU has, all at once, for each precision.
// - DRAM bandwidth: a STREAM triad (a = b + s c) per socket, run by that socket's threads on
//   arrays they touched first, so the memory is the socket's own, sized well past its caches.
//
// It should have the machine to itself: run it in an exclusive job, not on a shared login node.
// --threads N uses at most N CPUs, for a quick check; the result then says it is partial.

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#    include <sched.h>
#    include <unistd.h>
#endif

#if defined(__x86_64__)
#    include <immintrin.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    int    threads   = 0;   ///< 0: every CPU the process may run on
    double seconds   = 1.0; ///< per FLOP measurement
    long   stream_mb = 0;   ///< per triad array; 0: from the last-level cache
};

struct Cpu {
    int id;
    int socket;
};

auto read_file(std::string const &path) -> std::string {
    std::ifstream in(path);
    std::string   text;
    std::getline(in, text);
    return text;
}

/// The CPUs this process may run on, and the socket of each.
auto cpus() -> std::vector<Cpu> {
    std::vector<Cpu> out;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int id = 0; id < CPU_SETSIZE; ++id) {
            if (CPU_ISSET(id, &set)) {
                auto const socket = read_file("/sys/devices/system/cpu/cpu" + std::to_string(id) + "/topology/physical_package_id");
                out.push_back({id, socket.empty() ? 0 : std::atoi(socket.c_str())});
            }
        }
    }
#endif
    if (out.empty()) {
        for (unsigned i = 0; i < std::max(1U, std::thread::hardware_concurrency()); ++i) {
            out.push_back({static_cast<int>(i), 0});
        }
    }
    return out;
}

void pin(int cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
#else
    (void)cpu;
#endif
}

auto cpu_model() -> std::string {
    std::ifstream in("/proc/cpuinfo");
    std::string   line;
    while (std::getline(in, line)) {
        if (line.rfind("model name", 0) == 0) {
            return line.substr(line.find(':') + 2);
        }
    }
    return "unknown";
}

auto hostname() -> std::string {
#if defined(__linux__)
    char name[256] = {}; // NOLINT(modernize-avoid-c-arrays)
    gethostname(name, sizeof(name) - 1);
    return name;
#else
    return "unknown";
#endif
}

/// The last-level cache of CPU 0, in bytes; 0 if unknown.
auto llc_bytes() -> long {
    for (int index = 4; index >= 0; --index) {
        auto const size = read_file("/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(index) + "/size");
        if (!size.empty()) {
            long value = std::atol(size.c_str());
            if (size.find('K') != std::string::npos) {
                value *= 1024;
            } else if (size.find('M') != std::string::npos) {
                value *= 1024 * 1024;
            }
            return value;
        }
    }
    return 0;
}

std::atomic<double> g_sink{0.0};

// ---- FMA chains: FLOPs per call, run for `blocks` blocks of 1024 iterations ----------------------

constexpr int kBlock = 1024;

#if defined(__x86_64__)
// One function per width and precision, each compiled for its instructions, the chains in its
// registers: N independent accumulators hide the FMA's latency (4 cycles) on two ports, 16 at
// 512 bits and 12 at 256, where there are 16 registers.
#    define WAGGLE_CHAINS(NAME, TARGET, V, N, SET1, FMA, LANE)                                                                             \
        [[gnu::target(TARGET)]] auto NAME(long blocks) -> double {                                                                         \
            V acc[N]; /* NOLINT(modernize-avoid-c-arrays) */                                                                               \
            for (int i = 0; i < (N); ++i) {                                                                                                \
                acc[i] = SET1(static_cast<LANE>(1.0 + i * 1e-9));                                                                          \
            }                                                                                                                              \
            V const x = SET1(static_cast<LANE>(0.999999));                                                                                 \
            V const y = SET1(static_cast<LANE>(1e-9));                                                                                     \
            for (long b = 0; b < blocks; ++b) {                                                                                            \
                for (int k = 0; k < kBlock; ++k) {                                                                                         \
                    /* unrolled whole, so the accumulators live in registers, not memory */                                                \
                    _Pragma("GCC unroll 16") for (int i = 0; i < (N); ++i) {                                                               \
                        acc[i] = FMA(acc[i], x, y);                                                                                        \
                    }                                                                                                                      \
                }                                                                                                                          \
            }                                                                                                                              \
            double sum = 0.0;                                                                                                              \
            for (int i = 0; i < (N); ++i) {                                                                                                \
                LANE lanes[sizeof(V) / sizeof(LANE)]; /* NOLINT(modernize-avoid-c-arrays) */                                               \
                std::memcpy(lanes, &acc[i], sizeof(V));                                                                                    \
                sum += static_cast<double>(lanes[0]);                                                                                      \
            }                                                                                                                              \
            return sum;                                                                                                                    \
        }

WAGGLE_CHAINS(run512d, "avx512f", __m512d, 16, _mm512_set1_pd, _mm512_fmadd_pd, double)
WAGGLE_CHAINS(run512s, "avx512f", __m512, 16, _mm512_set1_ps, _mm512_fmadd_ps, float)
WAGGLE_CHAINS(run256d, "avx2,fma", __m256d, 12, _mm256_set1_pd, _mm256_fmadd_pd, double)
WAGGLE_CHAINS(run256s, "avx2,fma", __m256, 12, _mm256_set1_ps, _mm256_fmadd_ps, float)
#    undef WAGGLE_CHAINS
#endif

/// Plain C++, for any other CPU: independent multiply-adds the compiler may fuse and vectorize.
template <typename T>
auto run_portable(long blocks) -> double {
    T acc[16]; // NOLINT(modernize-avoid-c-arrays)
    for (int i = 0; i < 16; ++i) {
        acc[i] = static_cast<T>(1.0 + i * 1e-9);
    }
    for (long b = 0; b < blocks; ++b) {
        for (int k = 0; k < kBlock; ++k) {
#pragma GCC unroll 16
            for (int i = 0; i < 16; ++i) {
                acc[i] = acc[i] * static_cast<T>(0.999999) + static_cast<T>(1e-9);
            }
        }
    }
    double sum = 0.0;
    for (int i = 0; i < 16; ++i) {
        sum += static_cast<double>(acc[i]);
    }
    return sum;
}

struct Kernel {
    std::string isa;
    double      flops_per_block; ///< per precision
    auto (*run)(long) -> double;
};

auto kernel(bool fp64) -> Kernel {
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx512f")) {
        return {"avx512", 2.0 * 16 * (fp64 ? 8 : 16) * kBlock, fp64 ? run512d : run512s};
    }
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        return {"avx2", 2.0 * 12 * (fp64 ? 4 : 8) * kBlock, fp64 ? run256d : run256s};
    }
#endif
    return {"portable", 2.0 * 16 * kBlock, fp64 ? run_portable<double> : run_portable<float>};
}

/// Every thread's FLOP rate, summed, running at once on @p use.
auto measure_flops(std::vector<Cpu> const &use, bool fp64, double seconds) -> std::pair<double, std::string> {
    Kernel const             k = kernel(fp64);
    std::vector<double>      rates(use.size(), 0.0);
    std::barrier             start(static_cast<std::ptrdiff_t>(use.size()));
    std::vector<std::thread> threads;
    for (size_t t = 0; t < use.size(); ++t) {
        threads.emplace_back([&, t] {
            pin(use[t].id);
            g_sink = g_sink + k.run(64); // warm the unit up, and the clock with it
            start.arrive_and_wait();
            long       blocks = 0;
            auto const begin  = Clock::now();
            auto       now    = begin;
            while (now - begin < std::chrono::duration<double>(seconds)) {
                g_sink = g_sink + k.run(256);
                blocks += 256;
                now = Clock::now();
            }
            rates[t] = blocks * k.flops_per_block / std::chrono::duration<double>(now - begin).count();
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    double total = 0.0;
    for (double const r : rates) {
        total += r;
    }
    return {total, k.isa};
}

/// The STREAM triad's bandwidth on @p use, every one of them on one socket: bytes the arrays held,
/// three per element, over the best of several passes.
auto measure_triad(std::vector<Cpu> const &use, long elements) -> double {
    size_t const n = use.size();
    // Not a std::vector, which zeroes its elements as it is made, on this thread: that places every
    // page on this thread's NUMA node before the threads below touch them. Left uninitialized, the
    // pages are placed as each thread first writes its own share, on that thread's node.
    auto const               a = std::make_unique_for_overwrite<double[]>(static_cast<size_t>(elements));
    auto const               b = std::make_unique_for_overwrite<double[]>(static_cast<size_t>(elements));
    auto const               c = std::make_unique_for_overwrite<double[]>(static_cast<size_t>(elements));
    std::barrier             sync(static_cast<std::ptrdiff_t>(n));
    constexpr int            kPasses = 6;
    std::vector<double>      pass_seconds(kPasses, 0.0);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < n; ++t) {
        threads.emplace_back([&, t] {
            pin(use[t].id);
            long const first = elements * static_cast<long>(t) / static_cast<long>(n);
            long const last  = elements * static_cast<long>(t + 1) / static_cast<long>(n);
            for (long i = first; i < last; ++i) { // touched first here, so placed on this socket
                a[i] = 0.0;
                b[i] = 1.0;
                c[i] = 2.0;
            }
            for (int pass = 0; pass < kPasses; ++pass) {
                sync.arrive_and_wait();
                auto const begin = Clock::now();
                for (long i = first; i < last; ++i) {
                    a[i] = b[i] + 3.0 * c[i];
                }
                sync.arrive_and_wait();
                if (t == 0) {
                    pass_seconds[static_cast<size_t>(pass)] = std::chrono::duration<double>(Clock::now() - begin).count();
                }
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    g_sink = g_sink + a[static_cast<size_t>(elements) / 2];
    // The first pass pays for page faults and frequency changes; the best of the rest.
    double const best = *std::min_element(pass_seconds.begin() + 1, pass_seconds.end());
    return 3.0 * 8.0 * static_cast<double>(elements) / best;
}

auto parse(int argc, char **argv, Options &options) -> bool {
    for (int i = 1; i < argc; ++i) {
        std::string const arg  = argv[i];
        auto const        next = [&]() -> char const        *{ return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--threads" && i + 1 < argc) {
            options.threads = std::atoi(next());
        } else if (arg == "--seconds" && i + 1 < argc) {
            options.seconds = std::atof(next());
        } else if (arg == "--stream-mb" && i + 1 < argc) {
            options.stream_mb = std::atol(next());
        } else {
            std::fprintf(stderr, "usage: waggle-calibrate [--threads N] [--seconds S] [--stream-mb MB]\n"
                                 "Measures this machine's FLOP and DRAM bandwidth peaks and prints them as JSON.\n"
                                 "Run it with the machine to itself; `waggle calibrate` keeps the result.\n");
            return false;
        }
    }
    return true;
}

} // namespace

auto main(int argc, char **argv) -> int {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    auto const all = cpus();
    // The threads to use: every CPU, or at most --threads of them, taken from each socket in turn.
    std::map<int, std::vector<Cpu>> by_socket;
    for (auto const &cpu : all) {
        by_socket[cpu.socket].push_back(cpu);
    }
    size_t const     wanted = options.threads > 0 ? std::min<size_t>(static_cast<size_t>(options.threads), all.size()) : all.size();
    std::vector<Cpu> use;
    for (size_t round = 0; use.size() < wanted; ++round) {
        for (auto const &[socket, list] : by_socket) {
            if (round < list.size() && use.size() < wanted) {
                use.push_back(list[round]);
            }
        }
    }

    auto const [fp64, isa] = measure_flops(use, true, options.seconds);
    auto const fp32        = measure_flops(use, false, options.seconds).first;

    long const llc = llc_bytes();
    // STREAM's rule: each array at least four times the last-level cache, so no pass finds the last
    // one's data still cached. Arrays of 4/3 of it read 681 GB/s a socket on Granite Rapids where
    // 4 GB arrays read 595. A run limited by --threads is a check, often on a node that limits
    // memory, so its arrays are kept small unless asked for.
    long const          default_mb = options.threads > 0 ? 256 : std::max<long>(64, 4 * llc / (1024 * 1024));
    long const          array_mb   = options.stream_mb > 0 ? options.stream_mb : default_mb;
    long const          elements   = array_mb * 1024 * 1024 / 8;
    std::vector<double> per_socket;
    for (auto const &[socket, list] : by_socket) {
        std::vector<Cpu> mine;
        std::copy_if(use.begin(), use.end(), std::back_inserter(mine), [&](Cpu const &c) { return c.socket == socket; });
        if (!mine.empty()) {
            per_socket.push_back(measure_triad(mine, elements));
        }
    }
    double dram = 0.0;
    for (double const bw : per_socket) {
        dram += bw;
    }

    std::string sockets_json;
    for (size_t i = 0; i < per_socket.size(); ++i) {
        sockets_json += (i ? ", " : "") + std::to_string(per_socket[i] / 1e9);
    }
    std::printf("{\"schema\": \"waggle.calibration/1\", \"host\": \"%s\", \"cpu\": \"%s\", \"sockets\": %zu, \"cpus\": %zu, "
                "\"threads\": %zu, \"complete\": %s, \"isa\": \"%s\", \"stream_array_mb\": %ld,\n"
                " \"peak\": {\"fp64_gflops\": %.3f, \"fp32_gflops\": %.3f, \"fp64_gflops_per_core\": %.3f, "
                "\"fp32_gflops_per_core\": %.3f, \"dram_gbs\": %.3f, \"dram_gbs_per_socket\": [%s]}}\n",
                hostname().c_str(), cpu_model().c_str(), by_socket.size(), all.size(), use.size(),
                use.size() == all.size() ? "true" : "false", isa.c_str(), array_mb, fp64 / 1e9, fp32 / 1e9,
                fp64 / 1e9 / static_cast<double>(use.size()), fp32 / 1e9 / static_cast<double>(use.size()), dram / 1e9,
                sockets_json.c_str());
    return 0;
}
