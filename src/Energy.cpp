//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Energy.hpp"

#include "Process.hpp"

#ifdef __APPLE__
#    include <libproc.h>
#    include <sys/sysctl.h>
#    include <unistd.h>
#endif

WAGGLE_NAMESPACE_BEGIN

#ifdef __APPLE__

namespace {

/// proc_pidinfo's PROC_PIDTHREADCOUNTS: a thread's counts and energy for each kind of core,
/// performance cores first. Declared here as XNU's source declares it; the SDK's headers do not.
constexpr int kPidThreadCounts = 34;
struct ThreadCountsLevel {
    uint64_t instructions;
    uint64_t cycles;
    uint64_t user_time_mach;
    uint64_t system_time_mach;
    uint64_t energy_nj;
};
struct ThreadCounts {
    uint16_t          length; ///< kinds of core filled in
    uint16_t          reserved0;
    uint32_t          reserved1;
    ThreadCountsLevel levels[4]; // NOLINT(modernize-avoid-c-arrays): room for more kinds than any Mac has
};

} // namespace

auto read_thread_energy(uint64_t kernel_thread, ThreadEnergy &out) -> bool {
    static auto const pid = static_cast<pid_t>(process_id());
    ThreadCounts      counts{};
    if (kernel_thread == 0 || proc_pidinfo(pid, kPidThreadCounts, kernel_thread, &counts, sizeof(counts)) <= 0 || counts.length < 1) {
        return false;
    }
    out = {};
    for (uint16_t level = 0; level < counts.length && level < 4; ++level) {
        out.total_nj += counts.levels[level].energy_nj;
    }
    out.e_core_nj = counts.length >= 2 ? counts.levels[1].energy_nj : 0;
    return true;
}

auto energy_available(std::string &why) -> bool {
    int    vm   = 0;
    size_t size = sizeof(vm);
    if (sysctlbyname("kern.hv_vmm_present", &vm, &size, nullptr, 0) == 0 && vm != 0) {
        why = "this Mac is a virtual machine, which estimates no energy";
        return false;
    }
    ThreadEnergy probe;
    if (!read_thread_energy(kernel_thread_id(), probe)) {
        why = "this kernel does not give threads their energy";
        return false;
    }
    return true;
}

#else

auto read_thread_energy(uint64_t /*kernel_thread*/, ThreadEnergy & /*out*/) -> bool {
    return false;
}

auto energy_available(std::string &why) -> bool {
#    ifdef __linux__
    why = "Linux measures energy per processor package (RAPL), not per thread, so a zone's share cannot be told";
#    else
    why = "this platform does not give a thread its energy";
#    endif
    return false;
}

#endif

WAGGLE_NAMESPACE_END
