//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// A program that opens a zone with WAGGLE_SAVE set and no server: at exit it must write the
// session file all the same. It once wrote nothing, as only the server wrote session files.

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

int main() {
    WAGGLE_ZONE("save: without a server");
    return 0;
}
