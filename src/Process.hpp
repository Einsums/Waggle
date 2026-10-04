//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <cstdint>
#include <optional>
#include <string>

WAGGLE_NAMESPACE_BEGIN

/// This process's id.
WAGGLE_EXPORT auto process_id() -> std::int64_t;

/// The running executable's full path, symlinks resolved where the platform allows; empty if unknown.
WAGGLE_EXPORT auto executable_path() -> std::string const &;

/// The running executable's file name; "unknown" if it cannot be found.
WAGGLE_EXPORT auto executable_name() -> std::string const &;

/// The kernel's id for the calling thread, as other tools show it (a Mach thread id on macOS, the
/// tid on Linux, the thread id on Windows); 0 where there is none.
WAGGLE_EXPORT auto kernel_thread_id() -> std::uint64_t;

/// Whether the calling thread is the one the process started on; empty where the platform does
/// not say (Windows).
WAGGLE_EXPORT auto is_main_thread() -> std::optional<bool>;

WAGGLE_NAMESPACE_END
