//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// Metal device zones on a real device: work the GPU ran, its time on the host's timeline.

#include <Waggle/Config.hpp>

#include <Waggle/Metal.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <thread>

#include "Profiler.hpp"

namespace {

auto timeline_entry(std::string const &name, bool device) -> std::optional<waggle::TimelineEvent> {
    auto &prof = waggle::Profiler::instance();
    auto  lock = prof.consumer()->lock_shared();
    for (auto const &e : prof.consumer()->timeline_events()) {
        if (e.name == name && e.track.empty() != device) {
            return e;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("A command buffer's GPU time is device work inside the zone that submitted it", "[metal]") {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        SKIP("no Metal device");
    }
    waggle::set_enabled(true);
    id<MTLCommandQueue> queue  = [device newCommandQueue];
    id<MTLBuffer>       buffer = [device newBufferWithLength:64 << 20 options:MTLResourceStorageModePrivate];

    {
        WAGGLE_ZONE("metal test: submit");
        id<MTLCommandBuffer>      commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit     = [commands blitCommandEncoder];
        [blit fillBuffer:buffer range:NSMakeRange(0, buffer.length) value:7];
        [blit endEncoding];
        WAGGLE_METAL_ZONE(commands, "metal test: fill");
        [commands commit];
        [commands waitUntilCompleted];
        REQUIRE(commands.status == MTLCommandBufferStatusCompleted);
    }

    // Completion handlers may run after waitUntilCompleted returns.
    std::optional<waggle::DeviceWork> work;
    for (int i = 0; i < 200 && !work; ++i) {
        auto &prof = waggle::Profiler::instance();
        prof.flush();
        auto lock = prof.consumer()->lock_shared();
        for (auto const &w : prof.consumer()->device_work()) {
            if (w.name == "metal test: fill") {
                work = w;
            }
        }
        if (!work) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(work);
    CHECK(work->count == 1);
    CHECK(work->track == std::string("Metal: ") + [device.name UTF8String]);
    CHECK(work->submitter == "metal test: submit");
    CHECK(work->total_ms > 0.0);

    // The GPU ran between the commit and the wait's end, so inside the zone around both: the
    // GPU's clock converted to the host's.
    auto const host = timeline_entry("metal test: submit", false);
    auto const gpu  = timeline_entry("metal test: fill", true);
    REQUIRE(host);
    REQUIRE(gpu);
    INFO("host " << host->start_ms << " .. " << host->end_ms << " ms, gpu " << gpu->start_ms << " .. " << gpu->end_ms << " ms");
    constexpr double slack_ms = 0.05; // the two clocks are sampled a few instructions apart
    CHECK(gpu->start_ms >= host->start_ms - slack_ms);
    CHECK(gpu->end_ms <= host->end_ms + slack_ms);
    CHECK(gpu->end_ms > gpu->start_ms);
}
