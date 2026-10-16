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
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
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
        // WAGGLE_REQUIRE_PMU (the WAGGLE_REQUIRE_PMU_TEST option): a machine meant to count.
        if (std::getenv("WAGGLE_REQUIRE_PMU") != nullptr) { // NOLINT(concurrency-mt-unsafe)
            FAIL(why);
        }
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
        // Until the thread itself has run 20 ms, which a loaded machine may take far longer to give.
        auto const cpu_ns = [] {
            timespec now{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
            return now.tv_sec * 1'000'000'000LL + now.tv_nsec;
        };
        auto const start    = cpu_ns();
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (cpu_ns() - start < 20'000'000 && std::chrono::steady_clock::now() < deadline) {
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
    CHECK(once > 1e7);
    CHECK(twice / once > 1.9);
    CHECK(twice / once < 2.1);
    CHECK(backend.slot_name(1) == "twice");
}

TEST_CASE("A thread reads its counters in user space, as a system call would", "[backend]") {
    waggle::PerfCounterBackend backend(
        {{"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS}, {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES}});
    std::array<uint64_t, waggle::kNumCounterSlots> user_before{}, user_after{};
    uint64_t                                       syscall_between = 0;
    bool                                           opened = false, user = false;
    auto const                                     reads_before = waggle::user_reads();
    std::string                                    described;
    std::thread([&] {
        waggle::ThreadCounters tc;
        opened = backend.open(tc);
        user   = opened && tc.user_reads;
        if (!user) {
            backend.close(tc);
            return;
        }
        // Each reading's instructions, the user ones on either side of the kernel's.
        backend.read(tc, user_before);
        struct {
            uint64_t nr, time_enabled, time_running, values[2];
        } reading{};
        CHECK(::read(tc.fds[0], &reading, sizeof(reading)) == sizeof(reading));
        syscall_between = reading.values[0];
        backend.read(tc, user_after);
        described = backend.describe(); // while this thread reads so
        backend.close(tc);
    }).join();
    if (!user) {
        std::string const why = opened ? backend.describe() : backend.why_not();
        if (std::getenv("WAGGLE_REQUIRE_PMU") != nullptr && !opened) { // NOLINT(concurrency-mt-unsafe)
            FAIL(why);
        }
        SKIP(why);
    }
    INFO("user " << user_before[0] << ", system call " << syscall_between << ", user " << user_after[0]);
    // Rescheduled between two reads, a user one may have fallen back to the system call: either way
    // the three come in order, a system call's few thousand instructions apart.
    CHECK(user_before[0] <= syscall_between);
    CHECK(syscall_between <= user_after[0]);
    CHECK(user_after[0] - user_before[0] < 1'000'000);
    CHECK(waggle::user_reads() > reads_before);
    CHECK(described.find("rdpmc on 1 thread(s)") != std::string::npos);
}
#else
TEST_CASE("Events that do not fit one group are read as two, and each slot is its weighted sum", "[backend]") {
    SKIP("the roofline source's counter groups are Linux perf_event's");
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

#ifdef __linux__
TEST_CASE("Each refusal of perf_event_open says what to change", "[backend]") {
    // The kernel's level refuses a process watching itself: name it, and the sysctl that fixes it.
    auto const high = waggle::perf_refusal(EACCES, "4");
    CHECK(high.find("kernel.perf_event_paranoid is 4") != std::string::npos);
    CHECK(high.find("sysctl kernel.perf_event_paranoid=2") != std::string::npos);
    // Refused although the level allows it: a container's seccomp profile.
    auto const sandboxed = waggle::perf_refusal(EPERM, "2");
    CHECK(sandboxed.find("seccomp") != std::string::npos);
    CHECK(sandboxed.find("CAP_PERFMON") != std::string::npos);
    // An unreadable level is taken for the kernel's refusal, the likelier cause.
    CHECK(waggle::perf_refusal(EACCES, "").find("is unknown") != std::string::npos);
    // No PMU, as on many virtual machines, and no perf_event_open at all.
    CHECK(waggle::perf_refusal(ENOENT, "2").find("no hardware counters") != std::string::npos);
    CHECK(waggle::perf_refusal(ENOSYS, "2").find("no perf_event_open") != std::string::npos);
}

TEST_CASE("A roofline precision it does not know is refused, saying which it takes", "[backend]") {
    std::string why;
    CHECK_FALSE(waggle::use_roofline_counters("fp128", why));
    CHECK(why == "roofline_precision is \"fp128\"; it takes fp64 or fp32");
}
#endif

// Programs whose work is known, run by tests/roofline_session.py with WAGGLE_SOURCES=roofline,
// which reads their efficiencies back with `waggle cpu`. Each zone gives its work hints per call,
// as Einsums gives each contraction's.
namespace {

/// c = a * b + 1: one fused multiply-add, 2 FLOPs, per element.
[[gnu::noinline]] void multiply_add(double const *a, double const *b, double *c, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        c[i] = a[i] * b[i] + 1.0;
    }
}

/// C += A B, n by n, the plain way: 2 n^3 FLOPs.
[[gnu::noinline]] void plain_dgemm(double const *a, double const *b, double *c, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < n; ++k) {
            double const aik = a[i * n + k];
            for (std::size_t j = 0; j < n; ++j) {
                c[i * n + j] += aik * b[k * n + j];
            }
        }
    }
}

} // namespace

TEST_CASE("Roofline - a program that computes each result twice", "[.recompute]") {
    constexpr std::size_t n = 4 << 20;
    std::vector<double>   a(n, 1.5), b(n, 2.0), c(n, 0.0);
    for (int call = 0; call < 10; ++call) {
        waggle::ScopedZone const zone("roofline: recompute");
        waggle::annotate("work.flops", 2.0 * static_cast<double>(n));
        multiply_add(a.data(), b.data(), c.data(), n);
        multiply_add(a.data(), b.data(), c.data(), n); // the same results again
    }
    g_sink += static_cast<uint64_t>(c[n / 2]);
}

TEST_CASE("Roofline - a program that multiplies matrices", "[.dgemm]") {
    constexpr std::size_t n = 256;
    std::vector<double>   a(n * n, 1.0), b(n * n, 0.5), c(n * n, 0.0);
    for (int call = 0; call < 10; ++call) {
        waggle::ScopedZone const zone("roofline: dgemm");
        waggle::annotate("work.flops", 2.0 * static_cast<double>(n * n * n));
        waggle::annotate("work.bytes", 3.0 * static_cast<double>(n * n) * sizeof(double));
        plain_dgemm(a.data(), b.data(), c.data(), n);
    }
    g_sink += static_cast<uint64_t>(c[n]);
}

TEST_CASE("Roofline - a program that reads every eighth double", "[.strided]") {
    constexpr std::size_t n = std::size_t{32} << 20; // 256 MB, far past any last-level cache
    std::vector<double>   data(n, 1.0);
    for (int call = 0; call < 4; ++call) {
        waggle::ScopedZone const zone("roofline: strided");
        // The work needs the eighth it reads; each read brings in a whole 64-byte line.
        waggle::annotate("work.bytes", static_cast<double>(n / 8) * sizeof(double));
        double sum = 0.0;
        for (std::size_t i = 0; i < n; i += 8) {
            sum += data[i];
        }
        g_sink += static_cast<uint64_t>(sum);
    }
}

#ifdef WAGGLE_TEST_HAVE_BLAS
// The Fortran interface every BLAS has, declared here so no header's location matters.
extern "C" void dgemm_(char const *transa, char const *transb, int const *m, int const *n, int const *k, double const *alpha,
                       double const *a, int const *lda, double const *b, int const *ldb, double const *beta, double *c, int const *ldc);

// Run by tests/roofline_session.py with one BLAS thread, as the counters are the zone's thread's: a
// matrix multiply as a tuned library does it, compute bound and near the machine's peak.
TEST_CASE("Roofline - a program that multiplies large matrices with a BLAS", "[.dgemm_large]") {
    constexpr int       n = 2048;
    std::vector<double> a(std::size_t{n} * n, 1.0), b(std::size_t{n} * n, 0.5), c(std::size_t{n} * n, 0.0);
    double const        one = 1.0;
    char const          no  = 'N';
    for (int call = 0; call < 3; ++call) {
        waggle::ScopedZone const zone("roofline: dgemm_large");
        waggle::annotate("work.flops", 2.0 * n * n * static_cast<double>(n));
        waggle::annotate("work.bytes", 3.0 * n * static_cast<double>(n) * sizeof(double));
        dgemm_(&no, &no, &n, &n, &n, &one, a.data(), &n, b.data(), &n, &one, c.data(), &n);
    }
    g_sink += static_cast<uint64_t>(c[n]);
}
#endif
