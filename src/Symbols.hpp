//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <string>

WAGGLE_NAMESPACE_BEGIN

/// The function a code address is in, demangled and without its arguments ("einsums::pack<double>"),
/// for naming zones nobody wrote. Where no name is known, the module and the offset into it
/// ("libfoo.so+0x1a2b"), which addr2line resolves later.
///
/// On Linux the module's own symbol table is read, as dladdr knows only the exported symbols and a
/// library built with hidden visibility exports few; it is read once per module and kept. macOS's
/// dladdr knows every symbol. Windows gives the module and offset.
WAGGLE_EXPORT auto function_at(void const *address) -> std::string;

WAGGLE_NAMESPACE_END
