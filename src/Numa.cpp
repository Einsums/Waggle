//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Numa.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#if defined(__linux__)
#    include <dirent.h>
#    include <sys/mman.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#endif

WAGGLE_NAMESPACE_BEGIN

namespace numa {

#if defined(__linux__)

namespace {

/// The CPUs of a sysfs list: "0-31,64-95".
auto parse_cpu_list(std::string const &list) -> std::vector<int> {
    std::vector<int>  cpus;
    std::stringstream in(list);
    std::string       range;
    while (std::getline(in, range, ',')) {
        if (range.empty()) {
            continue;
        }
        auto const dash  = range.find('-');
        int const  first = std::atoi(range.c_str());
        int const  last  = dash == std::string::npos ? first : std::atoi(range.c_str() + dash + 1);
        for (int cpu = first; cpu <= last; ++cpu) {
            cpus.push_back(cpu);
        }
    }
    return cpus;
}

auto read_topology() -> Topology {
    Topology    topo;
    std::string text;
    std::getline(std::ifstream("/sys/devices/system/node/online"), text);
    auto const nodes = parse_cpu_list(text); // the same list syntax
    if (nodes.empty()) {
        return topo;
    }
    topo.nodes = *std::max_element(nodes.begin(), nodes.end()) + 1;
    topo.cpus.assign(static_cast<size_t>(topo.nodes), "");
    for (int const node : nodes) {
        std::string list;
        std::getline(std::ifstream("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist"), list);
        topo.cpus[static_cast<size_t>(node)] = list;
        for (int const cpu : parse_cpu_list(list)) {
            if (cpu >= static_cast<int>(topo.node_of_cpu.size())) {
                topo.node_of_cpu.resize(static_cast<size_t>(cpu) + 1, -1);
            }
            topo.node_of_cpu[static_cast<size_t>(cpu)] = node;
        }
    }
    return topo;
}

} // namespace

auto topology() -> Topology const & {
    static Topology const topo = read_topology();
    return topo;
}

auto placement(uint64_t address, int64_t bytes, int samples) -> Placement {
    Placement  out;
    auto const page  = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    auto const first = (address + page - 1) / page * page;
    auto const end   = address + static_cast<uint64_t>(std::max<int64_t>(bytes, 0));
    if (end <= first) {
        return out;
    }
    uint64_t const      pages = (end - first) / page;
    auto const          n     = static_cast<uint64_t>(std::min<uint64_t>(pages, static_cast<uint64_t>(samples)));
    std::vector<void *> where(n);
    std::vector<int>    status(n, -1);
    for (uint64_t i = 0; i < n; ++i) {
        where[i] = reinterpret_cast<void *>(first + (i * pages / n) * page); // NOLINT(performance-no-int-to-ptr)
    }
    // No target nodes: the kernel only reports where each page is.
    if (syscall(SYS_move_pages, 0, n, where.data(), nullptr, status.data(), 0) != 0) {
        return out;
    }
    out.pages.assign(static_cast<size_t>(topology().nodes), 0);
    out.sampled = static_cast<uint32_t>(n);
    // -EFAULT is both "not mapped" and "mapped, but so untouched it has no page tables yet": the
    // kernel answers so for a fresh anonymous mapping. mincore tells them apart, failing with
    // ENOMEM where nothing is mapped.
    bool mapped = true;
    if (std::ranges::find(status, -EFAULT) != status.end()) {
        unsigned char resident = 0;
        mapped                 = mincore(reinterpret_cast<void *>(first), page, &resident) == 0 || errno != ENOMEM; // NOLINT
    }
    for (int const s : status) {
        if (s >= 0 && s < static_cast<int>(out.pages.size())) {
            ++out.pages[static_cast<size_t>(s)];
        } else if (s == -EFAULT && !mapped) {
            ++out.unmapped; // the block is gone
        } else {
            ++out.untouched; // mapped, with no page there yet
        }
    }
    return out;
}

auto tasks() -> std::vector<Task> {
    std::vector<Task> out;
    DIR              *dir = opendir("/proc/self/task");
    if (dir == nullptr) {
        return out;
    }
    while (dirent const *entry = readdir(dir)) { // NOLINT(concurrency-mt-unsafe): the consumer's own DIR
        char      *end = nullptr;
        auto const tid = std::strtoull(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0') {
            continue;
        }
        std::string line;
        std::getline(std::ifstream(std::string("/proc/self/task/") + entry->d_name + "/stat"), line);
        // The command name, in parentheses, can hold spaces: the fields that matter follow its end.
        auto const close = line.rfind(')');
        if (close == std::string::npos) {
            continue;
        }
        std::istringstream       rest(line.substr(close + 2));
        std::vector<std::string> fields;
        for (std::string field; rest >> field;) {
            fields.push_back(field);
        }
        // From field 3 (state) on: utime is field 14, stime 15, the CPU last run on 39.
        if (fields.size() > 36) {
            out.push_back({.tid        = tid,
                           .cpu        = std::atoi(fields[36].c_str()),
                           .busy_ticks = std::strtoull(fields[11].c_str(), nullptr, 10) + std::strtoull(fields[12].c_str(), nullptr, 10)});
        }
    }
    closedir(dir);
    return out;
}

auto ticks_per_second() -> double {
    static double const ticks = static_cast<double>(sysconf(_SC_CLK_TCK));
    return ticks > 0 ? ticks : 100.0;
}

auto huge_mappings() -> std::vector<Mapping> {
    std::vector<Mapping> out;
    std::ifstream        smaps("/proc/self/smaps");
    Mapping              current{};
    for (std::string line; std::getline(smaps, line);) {
        // A mapping's header begins with its range in hexadecimal, "7f...-7f... rw-p ..."; its
        // fields are "Name:   value kB".
        if (auto const dash = line.find('-');
            dash != std::string::npos && dash > 0 && std::isxdigit(line[0]) && line.find(':') > line.find(' ')) {
            current = {.start = std::strtoull(line.c_str(), nullptr, 16), .end = std::strtoull(line.c_str() + dash + 1, nullptr, 16)};
        } else if (line.rfind("AnonHugePages:", 0) == 0) {
            current.huge_bytes = std::strtoull(line.c_str() + 14, nullptr, 10) * 1024;
            if (current.huge_bytes > 0) {
                out.push_back(current);
            }
        }
    }
    std::ranges::sort(out, {}, &Mapping::start);
    return out;
}

#else

auto topology() -> Topology const & {
    static Topology const topo;
    return topo;
}

auto placement(uint64_t /*address*/, int64_t /*bytes*/, int /*samples*/) -> Placement {
    return {};
}

auto tasks() -> std::vector<Task> {
    return {};
}

auto ticks_per_second() -> double {
    return 100.0;
}

auto huge_mappings() -> std::vector<Mapping> {
    return {};
}

#endif

auto huge_percent(std::vector<Mapping> const &mappings, uint64_t address, int64_t bytes) -> double {
    if (bytes <= 0) {
        return 0.0;
    }
    uint64_t const end  = address + static_cast<uint64_t>(bytes);
    uint64_t       huge = 0;
    for (auto const &m : mappings) {
        if (m.start < end && address < m.end) {
            // Huge pages are spread over a mapping, so the block's share is in proportion.
            uint64_t const overlap = std::min(end, m.end) - std::max(address, m.start);
            huge += static_cast<uint64_t>(static_cast<double>(m.huge_bytes) * static_cast<double>(overlap) /
                                          static_cast<double>(m.end - m.start));
        }
    }
    return std::min(100.0, 100.0 * static_cast<double>(huge) / static_cast<double>(bytes));
}

} // namespace numa

WAGGLE_NAMESPACE_END
