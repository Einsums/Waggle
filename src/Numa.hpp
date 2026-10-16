//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <cstdint>
#include <string>
#include <vector>

WAGGLE_NAMESPACE_BEGIN

/// What the numa source asks the system: which NUMA node memory and threads are on. Linux only;
/// elsewhere every query answers as a machine of one node.
namespace numa {

/// The machine's NUMA nodes and the CPUs of each, read once.
struct Topology {
    int                      nodes{1};
    std::vector<int>         node_of_cpu; ///< by CPU number; -1 for a CPU in no node
    std::vector<std::string> cpus;        ///< each node's CPU list, as sysfs gives it ("0-31")
};
WAGGLE_EXPORT auto topology() -> Topology const &;

/// Where a block's pages are: @ref sampled pages spread evenly over it, each found on a node, not
/// yet touched (mapped, with no page behind it yet), or not mapped at all, as when the block was
/// freed without anyone recording it.
struct Placement {
    std::vector<uint32_t> pages; ///< by node
    uint32_t              untouched{0};
    uint32_t              unmapped{0};
    uint32_t              sampled{0};
};

/// Ask the kernel, without moving anything (move_pages with no target nodes), where @p samples
/// pages of the block of @p bytes at @p address are. Tens of microseconds for 256 pages.
WAGGLE_EXPORT auto placement(uint64_t address, int64_t bytes, int samples = 256) -> Placement;

/// One of the process's threads: the CPU it last ran on and the CPU time it has used.
struct Task {
    uint64_t tid;
    int      cpu;
    uint64_t busy_ticks; ///< user and system time, in clock ticks
};
/// Every thread of the process, from /proc/self/task/<tid>/stat.
WAGGLE_EXPORT auto tasks() -> std::vector<Task>;
/// Clock ticks per second, for Task::busy_ticks.
WAGGLE_EXPORT auto ticks_per_second() -> double;

/// A mapping of the process's address space and the bytes of it in transparent huge pages.
struct Mapping {
    uint64_t start;
    uint64_t end;
    uint64_t huge_bytes;
};
/// Every mapping with huge pages in it, from /proc/self/smaps, by start.
WAGGLE_EXPORT auto huge_mappings() -> std::vector<Mapping>;

/// The share of the block of @p bytes at @p address in huge pages, in percent, from @p mappings:
/// the huge bytes of the mappings it overlaps, as a large block is a mapping of its own.
WAGGLE_EXPORT auto huge_percent(std::vector<Mapping> const &mappings, uint64_t address, int64_t bytes) -> double;

} // namespace numa

WAGGLE_NAMESPACE_END
