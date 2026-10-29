//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// What tests/signposts.py records with Instruments: rounds of zones of two libraries, one inside the
// other, and one named at run time for its round. Run with WAGGLE_SOURCES=signposts and the number
// of rounds as the argument.

#include <Waggle/Waggle.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <os/log.h>
#include <os/signpost.h>
#include <string>
#include <thread>

namespace {

void nap() {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

} // namespace

int main(int argc, char **argv) {
    int const rounds = argc > 1 ? std::atoi(argv[1]) : 2;
    // xctrace launches the program a moment before it records, and zones emitted before then are lost.
    // Where signposts are off until something records, this waits for that; on a Mac as it ships they
    // are always on, so it returns at once and says nothing about xctrace.
    os_log_t const probe = os_log_create("waggle", "sptest_a");
    for (int i = 0; i < 500 && !os_signpost_enabled(probe); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Nothing the program can see says when xctrace's logging stream has attached: its "tracing
    // started" notification comes before it on CI's virtual Macs, and a fixed wait was outrun there.
    // So the rounds are spread over more than a second instead, and the script asks only that the
    // ones it got be the last ones emitted, with enough of them whole.
    static waggle::ZoneSite const outer{"signpost outer", __FILE__, __LINE__, "main", "sptest_a"};
    static waggle::ZoneSite const inner{"signpost inner", __FILE__, __LINE__, "main", "sptest_b"};
    for (int i = 0; i < rounds; ++i) {
        {
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
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto const status = waggle::source_status("signposts");
    std::printf("signposts: %s: %s\n", status ? status->state.c_str() : "?", status ? status->detail.c_str() : "");
    return 0;
}
