//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

// Metal device zones, for Objective-C++ that submits Metal work:
//
//     id<MTLCommandBuffer> buffer = [queue commandBuffer];
//     [kernel encodeToCommandBuffer:buffer ...];
//     WAGGLE_METAL_ZONE(buffer, "mps gemm");   // before commit
//     [buffer commit];
//
// The GPU time the buffer took is recorded once it completes, on a track for its device, named
// "mps gemm" and with the host zone that submitted it. Device work does not nest in the host's
// tree: it runs when the GPU gets to it. With WAGGLE_DISABLE defined, the macro expands to nothing.

#include <Waggle/Waggle.hpp>

#if !defined(WAGGLE_DISABLE)
#    import <Metal/Metal.h>

#    include <mach/mach_time.h>

#    include <cstdint>
#    include <mutex>
#    include <string>
#    include <unordered_map>

namespace waggle::metal {

/// @p host_seconds, on the clock MTLCommandBuffer's GPU times are given in (mach_absolute_time, in
/// seconds), on the clock device spans are given in: both are sampled now and the difference kept.
inline std::int64_t to_steady_ns(double host_seconds) {
    static mach_timebase_info_data_t const timebase = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    std::int64_t const steady_now = waggle_steady_ns();
    double const       host_now   = static_cast<double>(mach_absolute_time()) * timebase.numer / timebase.denom * 1e-9;
    return steady_now - static_cast<std::int64_t>((host_now - host_seconds) * 1e9);
}

/// The track a device's work goes on: "Metal: Apple M4 Max".
inline std::uint32_t track_of(id<MTLDevice> device) {
    static std::mutex                                mutex;
    static std::unordered_map<void *, std::uint32_t> tracks; // by device; devices live as long as the process
    std::scoped_lock const                           lock(mutex);
    auto [it, added] = tracks.try_emplace((__bridge void *)device, 0);
    if (added) {
        std::string const name = std::string("Metal: ") + [device.name UTF8String];
        it->second             = waggle_intern(name.data(), name.size());
    }
    return it->second;
}

/// Record @p buffer's GPU time as device work at @p site once it completes. Call before commit,
/// which is when Metal accepts completion handlers.
inline void record(id<MTLCommandBuffer> buffer, ZoneSite const &site) {
    if (!detail::on(site.domain_switch)) {
        return;
    }
    std::uint64_t const token = waggle_device_submit(); // names the zone open now
    std::uint32_t const track = track_of(buffer.device);
    std::uint32_t const where = site.site_id; // not `id`, which is Objective-C's object type
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
      // An error leaves no times worth recording; a buffer the GPU never ran reports zeros.
      if (done.status == MTLCommandBufferStatusCompleted && done.GPUEndTime > 0.0) {
          waggle_device_span(track, where, 0, to_steady_ns(done.GPUStartTime), to_steady_ns(done.GPUEndTime), token);
      }
    }];
}

} // namespace waggle::metal

/// Record the GPU time of command buffer @p buffer as device work named @p name (a literal); call
/// before commit.
#    define WAGGLE_METAL_ZONE(buffer, name)                                                                                                \
        do {                                                                                                                               \
            static ::waggle::ZoneSite const _waggle_metal_site{name, __FILE__, __LINE__, __func__, WAGGLE_CURRENT_DOMAIN};               \
            ::waggle::metal::record((buffer), _waggle_metal_site);                                                                         \
        } while (0)
#else
#    define WAGGLE_METAL_ZONE(buffer, name)                                                                                                \
        do {                                                                                                                               \
            (void)(buffer);                                                                                                                \
        } while (0)
#endif
