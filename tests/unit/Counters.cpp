//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// Hardware counters: the backends directly ("[backend]"), and the counters source end to end
// ("[source]", run with WAGGLE_SOURCES=counters in the environment).

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <string>
#include <thread>

#include "CounterBackend.hpp"
#include "Profiler.hpp"
#include "Sources.hpp"

#ifdef __linux__
#    include <linux/perf_event.h>
#endif

namespace {

std::atomic<uint64_t> g_sink{0};

[[gnu::noinline]] void work(int n) {
    for (int i = 0; i < n; ++i) {
        g_sink.fetch_add(static_cast<uint64_t>(i), std::memory_order_relaxed);
    }
}

/// Counts over @p n iterations of work, on a fresh thread, through @p backend.
auto count_work(waggle::CounterBackend &backend, int n, bool &opened) -> std::array<uint64_t, waggle::kNumCounterSlots> {
    std::array<uint64_t, waggle::kNumCounterSlots> delta{};
    std::thread([&] {
        waggle::ThreadCounters tc;
        opened = backend.open(tc);
        if (!opened) {
            return;
        }
        std::array<uint64_t, waggle::kNumCounterSlots> before{}, after{};
        backend.read(tc, before);
        work(n);
        backend.read(tc, after);
        for (int i = 0; i < waggle::kNumCounterSlots; ++i) {
            delta[i] = after[i] - before[i];
        }
        backend.close(tc);
    }).join();
    return delta;
}

} // namespace

#if defined(__APPLE__)
TEST_CASE("XNU gives a thread its own cycles and instructions, without root", "[backend]") {
    waggle::XnuCounterBackend backend;
    bool                      opened = false;
    auto const                delta  = count_work(backend, 1'000'000, opened);
    INFO(backend.why_not());
    if (!opened && waggle::running_in_vm()) {
        // CI's macOS runners are virtual machines; on real hardware a failure still fails.
        CHECK(backend.why_not().find("virtual machine") != std::string::npos);
        SKIP("a virtual machine has no performance counters");
    }
    REQUIRE(opened);
    CHECK(backend.slot_name(0) == "cycles");
    CHECK(backend.slot_name(1) == "instructions");
    CHECK(delta[1] > 1'000'000); // at least an instruction per iteration
    CHECK(delta[0] > 0);
    CHECK(delta[2] <= delta[0]); // the efficiency cores' share of the cycles
    CHECK(delta[3] <= delta[1]);
}
#elif defined(__linux__)
// Software counters, which a virtual machine without hardware ones (as in CI) still has: the group
// is opened and read in one system call either way.
TEST_CASE("Linux perf reads a thread's counter group in one read", "[backend]") {
    waggle::PerfCounterBackend backend(
        {{"task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK}, {"page-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS}});
    bool       opened = false;
    auto const delta  = count_work(backend, 5'000'000, opened);
    INFO(backend.why_not());
    // A kernel that lets a process count nothing (Ubuntu's perf_event_paranoid is 4) is a machine's
    // policy, not a defect. Waggle's CI lowers it and sets WAGGLE_REQUIRE_COUNTERS, so there a
    // refusal fails.
    if (!opened && std::getenv("WAGGLE_REQUIRE_COUNTERS") == nullptr && // NOLINT(concurrency-mt-unsafe)
        backend.why_not().find("perf_event_paranoid") != std::string::npos) {
        SKIP(backend.why_not());
    }
    REQUIRE(opened);
    CHECK(backend.slot_name(0) == "task-clock");
    CHECK(backend.slot_name(1) == "page-faults");
    CHECK(backend.slot_name(2).empty());
    CHECK(delta[0] > 0); // nanoseconds of the thread's time
    CHECK(delta[2] == 0);
}

TEST_CASE("A counter the kernel refuses says why", "[backend]") {
    waggle::PerfCounterBackend backend({{"nonsense", PERF_TYPE_HARDWARE, 0xffff}});
    bool                       opened = false;
    (void)count_work(backend, 1, opened);
    CHECK_FALSE(opened);
    CHECK_FALSE(backend.why_not().empty());
}
#else
TEST_CASE("A platform without a counter backend says so", "[backend]") {
    auto &backend = waggle::get_counter_backend();
    bool  opened  = false;
    (void)count_work(backend, 1, opened);
    CHECK_FALSE(opened);
    CHECK_FALSE(backend.why_not().empty());
}
#endif

TEST_CASE("Zones carry counters exactly when the source is active", "[source]") {
    std::string zone_name = "counted zone";
    std::thread([&] {
        waggle::ScopedZone const zone("counted zone");
        work(1'000'000);
    }).join();

    auto const status = waggle::counters::status();
    INFO(status.state << ": " << status.detail);
    CHECK(status.state != "off");
    CHECK(status.state != "waiting"); // the thread above registered after the request

    auto &prof = waggle::Profiler::instance();
    prof.flush();
    auto        lock    = prof.consumer()->lock_shared();
    bool        found   = false;
    bool        counted = false;
    std::string names;
    for (auto const &[tid, ts] : prof.consumer()->thread_data()) {
        for (auto const &[id, child] : ts.root.children) {
            if (child->name == zone_name) {
                found   = true;
                counted = !child->counters_total.empty();
                for (auto const &[name, total] : child->counters_total) {
                    names += name + "=" + std::to_string(total) + " ";
                }
            }
        }
    }
    INFO("counters: " << names);
    REQUIRE(found);
    CHECK(counted == (status.state == "active"));
    if (status.state == "unavailable") {
        CHECK_FALSE(status.detail.empty()); // and why
    }
}
