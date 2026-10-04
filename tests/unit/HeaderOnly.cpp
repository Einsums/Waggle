//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// The public headers and nothing else. tests/header_emits_nothing.cmake checks that this object
// defines no symbol and runs no initializer: a file compiled once per instruction set (a SIMD
// kernel) includes these headers, and anything they made the compiler emit would be a weak symbol
// the linker keeps one copy of for every caller, however that copy was compiled.

#include <Waggle/Waggle.h>
#include <Waggle/Waggle.hpp>
