//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// The roofline source's counters: event groups split in two and summed into slots, anywhere; and,
// with WAGGLE_SOURCES=roofline, each zone's FLOPs against loops whose FLOPs are known exactly, and
// its estimate of DRAM bytes against a loop that streams memory. Where the source cannot count
// (no PMU, a CPU whose FLOP events are not known, macOS), the kernel cases say why and skip.

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "CounterBackend.hpp"
#include "Profiler.hpp"
#include "Sources.hpp"

#ifdef __linux__
#    include <linux/perf_event.h>
#endif

namespace {

std::atomic<uint64_t> g_sink{0};

/// Each zone's counter totals, by zone name, every thread's together.
auto zone_counters() -> std::map<std::string, std::map<std::string, uint64_t>> {
    auto &prof = waggle::Profiler::instance();
    prof.flush();
    auto                                                   lock = prof.consumer()->lock_shared();
    std::map<std::string, std::map<std::string, uint64_t>> out;
    std::vector<waggle::AggNode const *>                   pending;
    for (auto const &[tid, ts] : prof.consumer()->thread_data()) {
        pending.push_back(&ts.root);
    }
    while (!pending.empty()) {
        auto const *node = pending.back();
        pending.pop_back();
        for (auto const &[name, total] : node->counters_total) {
            out[node->name][name] += total;
        }
        for (auto const &child : node->children) {
            pending.push_back(child.second.get());
        }
    }
    return out;
}

void require_roofline() {
    // A thread registering now opens the counters; the status is active once one has.
    std::thread([] { waggle::ScopedZone const zone("roofline: first"); }).join();
    auto const status = waggle::roofline::status();
    if (status.state != "active") {
        std::string const why = "roofline is " + status.state + ": " + status.detail;
        SKIP(why);
    }
}

#if defined(__x86_64__)
// N fused multiply-adds of doubles, each 2 FLOPs, written in assembly so the count is exact.
[[gnu::noinline, gnu::target("fma")]] void scalar_fmas(long n) {
    double a = 1.0;
    double b = 0.999;
    for (long i = 0; i < n; ++i) {
        asm volatile("vfmadd231sd %1, %1, %0" : "+x"(a) : "x"(b));
    }
    g_sink += static_cast<uint64_t>(a);
}

// N 512-bit fused multiply-adds of doubles, each 16 FLOPs.
[[gnu::noinline, gnu::target("avx512f")]] void packed_fmas(long n) {
    for (long i = 0; i < n; ++i) {
        asm volatile("vxorpd %%zmm1, %%zmm1, %%zmm1\n\tvfmadd231pd %%zmm1, %%zmm1, %%zmm0" ::: "xmm0", "xmm1");
    }
}
#endif

} // namespace

#ifdef __linux__
TEST_CASE("Events that do not fit one group are read as two, and each slot is its weighted sum", "[backend]") {
    // Software clocks, which every machine has: the second slot is twice the second group's clock,
    // which ran beside the first's, so it reads about twice the first slot.
    waggle::PerfCounterBackend                     backend({{.type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_TASK_CLOCK},
                                                            {.type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_TASK_CLOCK}},
                                                           {{.name = "once", .terms = {{0, 1}}}, {.name = "twice", .terms = {{1, 2}}}}, 1);
    std::array<uint64_t, waggle::kNumCounterSlots> before{}, after{};
    bool                                           opened = false;
    std::thread([&] {
        waggle::ThreadCounters tc;
        opened = backend.open(tc);
        if (!opened) {
            return;
        }
        CHECK(tc.groups == 2);
        backend.read(tc, before);
        auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (std::chrono::steady_clock::now() < until) {
            g_sink.fetch_add(1, std::memory_order_relaxed);
        }
        backend.read(tc, after);
        backend.close(tc);
    }).join();
    if (!opened) {
        SKIP(backend.why_not());
    }
    auto const once  = static_cast<double>(after[0] - before[0]);
    auto const twice = static_cast<double>(after[1] - before[1]);
    INFO("once " << once << " ns, twice " << twice << " ns");
    // A loaded machine gives the thread less than the 50 ms it spins; the ratio is the check.
    CHECK(once > 1e6);
    CHECK(twice / once > 1.9);
    CHECK(twice / once < 2.1);
    CHECK(backend.slot_name(1) == "twice");
}
#endif

#if defined(__x86_64__)
TEST_CASE("A zone's FLOPs are the FLOPs it ran", "[source]") {
    require_roofline();
    constexpr long kScalar = 20'000'000;
    constexpr long kPacked = 10'000'000;
    std::thread([] {
        {
            waggle::ScopedZone const zone("roofline: scalar");
            scalar_fmas(kScalar);
        }
        if (__builtin_cpu_supports("avx512f")) {
            waggle::ScopedZone const zone("roofline: packed");
            packed_fmas(kPacked);
        }
    }).join();
    auto const counters = zone_counters();
    auto const flops    = [&](std::string const &zone) {
        auto const it = counters.find(zone);
        return it == counters.end() || !it->second.contains("fp64_flops") ? 0.0 : static_cast<double>(it->second.at("fp64_flops"));
    };
    INFO("scalar zone: " << flops("roofline: scalar") << " FLOPs, packed zone: " << flops("roofline: packed"));
    CHECK(flops("roofline: scalar") / (2.0 * kScalar) > 0.99);
    CHECK(flops("roofline: scalar") / (2.0 * kScalar) < 1.01);
    if (__builtin_cpu_supports("avx512f")) {
        CHECK(flops("roofline: packed") / (16.0 * kPacked) > 0.99);
        CHECK(flops("roofline: packed") / (16.0 * kPacked) < 1.01);
    }
}
#endif

TEST_CASE("A zone that streams memory misses the last-level cache, and one in registers does not", "[source]") {
    require_roofline();
    std::thread([] {
        std::vector<double> data(32 * 1024 * 1024, 1.0); // 256 MB, far past any last-level cache
        {
            waggle::ScopedZone const zone("roofline: streams");
            double                   sum = 0.0;
            for (int pass = 0; pass < 2; ++pass) {
                for (double const v : data) {
                    sum += v;
                }
            }
            g_sink += static_cast<uint64_t>(sum);
        }
        {
            waggle::ScopedZone const zone("roofline: registers");
            uint64_t                 x = 1;
            for (long i = 0; i < 100'000'000; ++i) {
                x = x * 6364136223846793005ULL + static_cast<uint64_t>(i);
            }
            g_sink += x;
        }
    }).join();
    auto const counters  = zone_counters();
    auto const streams   = counters.at("roofline: streams").at("llc_miss_bytes");
    auto const registers = counters.at("roofline: registers").at("llc_miss_bytes");
    INFO("streams " << streams << " bytes, registers " << registers << " bytes, of 512 MB read");
    // Prefetched lines are not demand misses, so the estimate undercounts streaming: it is only
    // checked to be of the right kind.
    CHECK(streams > 4 * 1024 * 1024);
    CHECK(streams > 100 * registers);
}
