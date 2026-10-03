//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// A program that links a library using Waggle but never opens a zone, and never calls finalize:
// at exit it must leave no report file behind, though reports are on by default. Its report path
// is set through the environment and checked to be absent after it ends.

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

int main() {
    // The collector exists and is configured, as any library linking it makes it, but records nothing.
    waggle::configure({.max_distinct_children = 8});
    return waggle::total_push_count() == 0 ? 0 : 1;
}
