//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <cstdint>
#include <string>

WAGGLE_NAMESPACE_BEGIN

/// The energy a thread has used since it started, as the platform estimates it, in nanojoules.
struct ThreadEnergy {
    uint64_t total_nj{0};
    uint64_t e_core_nj{0}; ///< of which on efficiency cores
};

/// Thread @p kernel_thread's energy so far, the thread named by its @ref kernel_thread_id. Any thread of the process may read any other's.
///
/// On macOS it is XNU's estimate from the CPU's power model (proc_pidinfo's PROC_PIDTHREADCOUNTS),
/// updated every few milliseconds, not continuously; reading it costs about 160 ns. Elsewhere there is
/// no per-thread energy: Linux's RAPL measures a whole processor package.
WAGGLE_EXPORT auto read_thread_energy(uint64_t kernel_thread, ThreadEnergy &out) -> bool;

/// Whether this machine gives threads their energy; if not, why, in @p why.
WAGGLE_EXPORT auto energy_available(std::string &why) -> bool;

WAGGLE_NAMESPACE_END
