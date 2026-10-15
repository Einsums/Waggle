//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// A library the sampling test calls as Einsums calls a BLAS: built without line information, so
// samples in it are credited to the line that called it. Its entry calls a loop of its own, as a
// library's entry calls its kernels, so the chain from a sample to its caller crosses two frames.

#include <cstdint>

namespace {

[[gnu::noinline]] auto library_kernel(std::uint64_t n) -> std::uint64_t {
    std::uint64_t x = 5;
    for (std::uint64_t i = 0; i < n; ++i) {
        x = x * 6364136223846793005ULL + i;
    }
    return x;
}

} // namespace

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto waggle_test_library_work(std::uint64_t n) -> std::uint64_t {
    return library_kernel(n) + 1;
}
