//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// Device work through the C interface, with times made up: what a Metal, CUDA or HIP completion
// callback records, without a device.

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Profiler.hpp"

namespace {

auto intern(char const *s) -> uint32_t {
    return waggle_intern(s, std::strlen(s));
}

auto site(char const *name) -> uint32_t {
    return waggle_register_site(name, std::strlen(name), __FILE__, __LINE__, "", waggle_register_domain("devtest", 7));
}

auto work_named(std::string const &track, std::string const &name) -> std::optional<waggle::DeviceWork> {
    auto &prof = waggle::Profiler::instance();
    prof.flush();
    auto lock = prof.consumer()->lock_shared();
    for (auto const &w : prof.consumer()->device_work()) {
        if (w.track == track && w.name == name) {
            return w;
        }
    }
    return std::nullopt;
}

/// One step at a time between two threads.
struct Turns {
    std::mutex              m;
    std::condition_variable cv;
    int                     turn = 0;
    void                    wait_for(int t) {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return turn >= t; });
    }
    void pass_to(int t) {
        {
            std::scoped_lock const lock(m);
            turn = t;
        }
        cv.notify_all();
    }
};

} // namespace

// The submission and its span come on different threads' rings. The consumer reads rings in the
// order their threads registered, so a completion thread that registered first has its span read
// before the submission it names.
TEST_CASE("Device work is named after the zone that submitted it, whichever ring is read first", "[device]") {
    waggle::set_enabled(true);
    uint32_t const track = intern("test device: queue A");
    uint32_t const what  = site("devtest: kernel");
    Turns          turns;
    uint64_t       token = 0;

    std::thread completion([&] {
        {
            waggle::ScopedZone const registers("devtest: completion thread");
        } // registers this ring first
        turns.pass_to(1);
        turns.wait_for(2);
        int64_t const now = waggle_steady_ns();
        waggle_device_span(track, what, 0, now - 2'000'000, now - 1'000'000, token); // 1 ms of work
    });
    std::thread submitter([&] {
        turns.wait_for(1);
        waggle::ScopedZone const zone("devtest: submitting zone");
        token = waggle_device_submit();
        turns.pass_to(2);
    });
    submitter.join();
    completion.join();

    auto const work = work_named("test device: queue A", "devtest: kernel");
    REQUIRE(work);
    CHECK(work->count == 1);
    CHECK(work->total_ms == Catch::Approx(1.0).margin(1e-6));
    CHECK(work->submitter == "devtest: submitting zone");
}

TEST_CASE("Device work is summed by track and name and placed on the timeline", "[device]") {
    waggle::set_enabled(true);
    uint32_t const track = intern("test device: queue B");
    uint32_t const what  = site("devtest: copy");
    int64_t const  t0    = waggle_steady_ns();
    for (int i = 1; i <= 3; ++i) {
        waggle_device_span(track, what, 0, t0 + i * 10'000'000, t0 + i * 10'000'000 + i * 1'000'000, 0); // i ms each
    }

    auto const work = work_named("test device: queue B", "devtest: copy");
    REQUIRE(work);
    CHECK(work->count == 3);
    CHECK(work->total_ms == Catch::Approx(6.0).margin(1e-6));
    CHECK(work->min_ms == Catch::Approx(1.0).margin(1e-6));
    CHECK(work->max_ms == Catch::Approx(3.0).margin(1e-6));
    CHECK(work->submitter.empty()); // no token

    auto &prof = waggle::Profiler::instance();
    auto  lock = prof.consumer()->lock_shared();
    auto  rows = prof.consumer()->timeline_events();
    auto  n    = std::ranges::count_if(rows, [](auto const &e) { return e.track == "test device: queue B" && e.name == "devtest: copy"; });
    CHECK(n == 3);
}

TEST_CASE("A thread that only records device work has no tree of its own", "[device]") {
    waggle::set_enabled(true);
    uint32_t const track     = intern("test device: queue C");
    uint32_t const what      = site("devtest: alone");
    uint32_t       thread_id = 0;
    std::thread([&] {
        thread_id         = waggle_current_thread_id();
        int64_t const now = waggle_steady_ns();
        waggle_device_span(track, what, 0, now - 1000, now, 0);
    }).join();
    REQUIRE(work_named("test device: queue C", "devtest: alone"));
    for (auto const &thread : waggle::Snapshot::take().threads()) {
        CHECK(thread.id != thread_id);
    }
}

TEST_CASE("A reset sums device work afresh", "[device]") {
    waggle::set_enabled(true);
    uint32_t const track = intern("test device: queue D");
    uint32_t const what  = site("devtest: before reset");
    int64_t const  now   = waggle_steady_ns();
    waggle_device_span(track, what, 0, now - 1000, now, 0);
    REQUIRE(work_named("test device: queue D", "devtest: before reset"));
    waggle_reset();
    CHECK_FALSE(work_named("test device: queue D", "devtest: before reset"));
}
