//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// What tests/signposts.py records with Instruments: zones of two libraries, one inside the other,
// and one named at run time. Run with WAGGLE_SOURCES=signposts.

#include <Waggle/Waggle.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <notify.h>
#include <string>
#include <thread>

namespace {

void nap() {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

} // namespace

int main() {
    // Zones emitted before xctrace records are lost, and nothing in the process can tell when it
    // does: os_signpost_enabled is true whether or not anyone records. So the test names a Darwin
    // notification in SIGNPOSTS_RECORDING, which xctrace posts once it records (--notify-tracing-started),
    // and the program waits for it. "ready" says it is listening, so the test attaches only then.
    if (char const *recording = std::getenv("SIGNPOSTS_RECORDING")) {
        int token  = 0;
        int posted = 0;
        notify_register_check(recording, &token);
        notify_check(token, &posted); // the first check after registering always says posted
        std::printf("signposts: ready\n");
        std::fflush(stdout);
        posted = 0;
        for (int i = 0; i < 9000 && posted == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            notify_check(token, &posted);
        }
        notify_cancel(token);
        if (posted == 0) {
            std::printf("signposts: xctrace never said it was recording\n");
            return 1;
        }
    }
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
