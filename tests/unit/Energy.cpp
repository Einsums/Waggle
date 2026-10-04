//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// The energy source, run with WAGGLE_SOURCES=energy: each zone's energy, sampled as the consumer
// drains and credited to the zone open when it was read.

#include "Energy.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>

#include "Sources.hpp"

namespace {

std::atomic<uint64_t> g_sink{0};

/// Keep a core busy for @p ms.
void busy(int ms) {
    auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        for (int i = 0; i < 1000; ++i) {
            g_sink.fetch_add(static_cast<uint64_t>(i), std::memory_order_relaxed);
        }
    }
}

auto energy_of(waggle::Snapshot const &snapshot, std::string_view path) -> uint64_t {
    auto const node = snapshot.find(path);
    REQUIRE(node);
    return node->stats().energy_nj;
}

} // namespace

TEST_CASE("Energy is credited to the zone that used it", "[energy]") {
    std::string why;
    if (!waggle::energy_available(why)) {
        auto const status = waggle::energy::status();
        CHECK(status.state == "unavailable");
        CHECK(status.detail == why);
        SKIP(why);
    }

    // The kernel adds a thread's energy every few milliseconds, so what a zone used last can arrive
    // in the zone after it: each busy section is followed by a quiet one to take that, and the
    // checks are on what each zone used, not on its edges.
    auto const quiet = [] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); };
    std::thread([&] {
        {
            waggle::ScopedZone const idle("energy: sleeping");
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        // Written as one event when it closes, as nothing happens inside it: what was read while it
        // was open must still be its own.
        {
            waggle::ScopedZone const leaf("energy: busy leaf");
            busy(150);
        }
        {
            waggle::ScopedZone const gap("energy: quiet");
            quiet();
        }
        {
            waggle::ScopedZone const parent("energy: parent");
            waggle::ScopedZone const child("energy: busy child");
            busy(150);
        }
        waggle::ScopedZone const last("energy: last"); // takes the child's tail, and is the event after it
        quiet();
    }).join();

    auto const snapshot  = waggle::Snapshot::take();
    auto const busy_leaf = energy_of(snapshot, "energy: busy leaf");
    auto const sleeping  = energy_of(snapshot, "energy: sleeping");
    auto const parent    = energy_of(snapshot, "energy: parent");
    auto const child     = energy_of(snapshot, "energy: parent/energy: busy child");
    INFO("busy leaf " << busy_leaf << " nJ, sleeping " << sleeping << " nJ, parent " << parent << " nJ, child " << child << " nJ");
    CHECK(busy_leaf > 0);
    CHECK(sleeping * 10 < busy_leaf); // a thread that waits uses next to nothing
    CHECK(child > 0);
    CHECK(parent * 10 < child); // exclusive, as time is: the work was the child's

    auto const status = waggle::energy::status();
    CHECK(status.state == "active");
}
