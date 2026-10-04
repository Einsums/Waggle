//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// What tests/signposts.py records with Instruments: zones of two libraries, one inside the other,
// and one named at run time. Run with WAGGLE_SOURCES=signposts.

#include <Waggle/Waggle.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

void nap() {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

} // namespace

int main() {
    static waggle::ZoneSite const outer{"signpost outer", __FILE__, __LINE__, "main", "sptest_a"};
    static waggle::ZoneSite const inner{"signpost inner", __FILE__, __LINE__, "main", "sptest_b"};
    for (int i = 0; i < 2; ++i) {
        waggle::ScopedZone const a(outer);
        nap();
        {
            waggle::ScopedZone const b(inner);
            nap();
        }
        waggle::push("signpost dynamic " + std::to_string(i));
        nap();
        waggle::pop();
    }
    auto const status = waggle::source_status("signposts");
    std::printf("signposts: %s: %s\n", status ? status->state.c_str() : "?", status ? status->detail.c_str() : "");
    return 0;
}
