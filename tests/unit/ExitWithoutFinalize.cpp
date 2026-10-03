//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// A program that never calls finalize leaves the profiler's destructor to drain the rings, write
// the outputs the settings ask for, and stop the server, during static destruction. It used to
// abort there ("mutex lock failed"): the server reported its shutdown through a diagnostics mutex
// that was first used after the profiler was built, and so had already been destroyed. And it used
// to write nothing. Passes when the process exits cleanly; the report and session file it is asked
// for through the environment are checked after it ends.

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <cstdint>

#include "Sockets.hpp"

int main() {
    // The profiler is built here, before anything it reports through has been used.
    waggle::set_enabled(true);
    {
        WAGGLE_ZONE("exit: before the server");
    }

    // A running server, so the destructor shuts one down and reports doing so.
    if (auto const port = waggle_test::free_port(); port != 0) {
        waggle::configure({.server = true, .port = port});
    }
    // The first diagnostic of the run, after the profiler was built: a refused setting.
    waggle::configure({.max_distinct_children = 3});
    waggle::configure({.max_distinct_children = 4});

    // Left in the ring for the destructor's final drain.
    {
        WAGGLE_ZONE("exit: left for the drain");
        waggle::annotate("key", "value");
    }
    return 0;
}
