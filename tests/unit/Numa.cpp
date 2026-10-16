//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// The numa source's queries of the system: the nodes and their CPUs, where a block's pages are,
// and where each thread runs; and, run by tests/numa_session.py with WAGGLE_SOURCES=numa, programs
// whose placement is known, for `waggle cpu --numa` to find. On a machine of one node (most CI
// runners), whatever needs two says so and skips.

#include "Numa.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Process.hpp"

#if defined(__linux__)
#    include <sched.h>
#    include <sys/mman.h>
#endif

using namespace waggle;

namespace {

std::atomic<uint64_t> g_sink{0};

constexpr int64_t kBlock = int64_t{64} << 20; // past the source's 16 MB, a few seconds' work

/// Two CPUs on two different nodes, or nothing on a machine of one.
auto two_nodes() -> std::pair<int, int> {
    auto const &topo  = numa::topology();
    int         first = -1;
    for (int cpu = 0; cpu < static_cast<int>(topo.node_of_cpu.size()); ++cpu) {
        int const node = topo.node_of_cpu[static_cast<size_t>(cpu)];
        if (node < 0) {
            continue;
        }
        if (first < 0) {
            first = cpu;
        } else if (node != topo.node_of_cpu[static_cast<size_t>(first)]) {
            return {first, cpu};
        }
    }
    return {-1, -1};
}

void require_two_nodes() {
    if (numa::topology().nodes < 2 || two_nodes().first < 0) {
        // WAGGLE_REQUIRE_NUMA (the WAGGLE_REQUIRE_NUMA_TEST option): a machine meant to have two.
        if (std::getenv("WAGGLE_REQUIRE_NUMA") != nullptr) { // NOLINT(concurrency-mt-unsafe)
            FAIL("WAGGLE_REQUIRE_NUMA is set, but this machine has one NUMA node");
        }
        SKIP("this machine has one NUMA node");
    }
}

void pin(int cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
#else
    (void)cpu;
#endif
}

void spin_for(std::chrono::milliseconds length) {
    auto const until = std::chrono::steady_clock::now() + length;
    uint64_t   x     = 1;
    while (std::chrono::steady_clock::now() < until) {
        for (int i = 0; i < 10000; ++i) {
            x = x * 6364136223846793005ULL + 1;
        }
    }
    g_sink += x;
}

/// A block no thread has touched yet: on Linux a mapping of its own, its pages made by the first
/// write to each. Not from malloc, which can hand back memory already touched: once a large block
/// it mapped is freed, it raises its threshold for mapping and serves the next from its heap.
/// Elsewhere the source has one node and the cases that place pages skip, so new[] is enough.
struct UntouchedBlock {
    char   *data;
    int64_t bytes;
#if defined(__linux__)
    explicit UntouchedBlock(int64_t size)
        : data(static_cast<char *>(mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))),
          bytes(size) {
        REQUIRE(data != MAP_FAILED);
    }
    ~UntouchedBlock() { munmap(data, static_cast<size_t>(bytes)); }
#else
    explicit UntouchedBlock(int64_t size) : data(new char[static_cast<size_t>(size)]), bytes(size) {}
    ~UntouchedBlock() { delete[] data; }
#endif
    UntouchedBlock(UntouchedBlock const &)               = delete;
    UntouchedBlock    &operator=(UntouchedBlock const &) = delete;
    [[nodiscard]] auto get() const -> char * { return data; }
};

auto untouched_block(int64_t bytes) -> std::unique_ptr<UntouchedBlock> {
    return std::make_unique<UntouchedBlock>(bytes);
}

} // namespace

TEST_CASE("NUMA - the topology names each CPU's node", "[numa]") {
    auto const &topo = numa::topology();
    REQUIRE(topo.nodes >= 1);
    if (topo.nodes < 2) {
        SKIP("this machine has one NUMA node");
    }
    CHECK(static_cast<int>(topo.cpus.size()) == topo.nodes);
    int with_cpus = 0;
    for (auto const &list : topo.cpus) {
        with_cpus += list.empty() ? 0 : 1;
    }
    CHECK(with_cpus >= 2);
}

TEST_CASE("NUMA - a block's pages are on the node of the thread that first wrote them", "[numa]") {
    require_two_nodes();
    auto const [cpu_a, cpu_b] = two_nodes();
    auto const &topo          = numa::topology();
    for (int const cpu : {cpu_a, cpu_b}) {
        auto const block = untouched_block(kBlock);
        auto const where = reinterpret_cast<uint64_t>(block->get());
        CHECK(numa::placement(where, kBlock).untouched == numa::placement(where, kBlock).sampled);
        std::thread([&] {
            pin(cpu);
            std::memset(block->get(), 1, static_cast<size_t>(kBlock));
        }).join();
        auto const placed = numa::placement(where, kBlock);
        int const  node   = topo.node_of_cpu[static_cast<size_t>(cpu)];
        INFO("CPU " << cpu << " on node " << node << ": " << placed.pages[static_cast<size_t>(node)] << " of " << placed.sampled);
        REQUIRE(placed.sampled == 256);
        CHECK(placed.pages[static_cast<size_t>(node)] >= 243); // 95%: memory pressure can put a few elsewhere
        CHECK(placed.untouched == 0);
    }
}

TEST_CASE("NUMA - a block that is gone reads as unmapped, not untouched", "[numa]") {
#if defined(__linux__)
    auto       block = untouched_block(kBlock);
    auto const where = reinterpret_cast<uint64_t>(block->get());
    block.reset(); // unmapped
    auto const placed = numa::placement(where, kBlock);
    CHECK(placed.sampled == 256);
    CHECK(placed.unmapped == placed.sampled);
    CHECK(placed.untouched == 0);
#else
    SKIP("Linux only");
#endif
}

TEST_CASE("NUMA - each thread's CPU and CPU time are read", "[numa]") {
    auto const me     = kernel_thread_id();
    auto const before = numa::tasks();
    auto const find   = [&](std::vector<numa::Task> const &tasks) {
        for (auto const &t : tasks) {
            if (t.tid == me) {
                return t;
            }
        }
        return numa::Task{.tid = 0, .cpu = -1, .busy_ticks = 0};
    };
    spin_for(std::chrono::milliseconds(200));
    auto const after = find(numa::tasks());
#if defined(__linux__)
    REQUIRE(after.tid == me);
    CHECK(after.cpu >= 0);
    CHECK(after.busy_ticks > find(before).busy_ticks);
#else
    CHECK(before.empty());
#endif
}

TEST_CASE("NUMA - a block's share of huge pages is its share of its mappings'", "[numa]") {
    // A 4 MB mapping with 2 MB huge; a block of all of it, and one of its first half.
    std::vector<numa::Mapping> const mappings{{.start = 0x400000, .end = 0x800000, .huge_bytes = 0x200000}};
    CHECK(numa::huge_percent(mappings, 0x400000, 0x400000) == 50.0);
    CHECK(numa::huge_percent(mappings, 0x400000, 0x200000) == 50.0);
    CHECK(numa::huge_percent(mappings, 0x1000000, 0x200000) == 0.0);
    CHECK(numa::huge_percent(mappings, 0x400000, 0) == 0.0);
}

// Run by tests/numa_session.py: a block one thread initializes on one node, another that threads
// on two nodes initialize together, then work on the second node with both live. Only the first is
// the first-touch mistake.
TEST_CASE("NUMA - a program with a serial and a parallel first touch", "[.first_touch]") {
    require_two_nodes();
    auto const [cpu_a, cpu_b] = two_nodes();
    auto const serial         = untouched_block(kBlock);
    auto const parallel       = untouched_block(kBlock);
    std::thread([&] {
        pin(cpu_a);
        waggle::ScopedZone const zone("numa: serial init");
        WAGGLE_MEM_ALLOC_AT(serial->get(), kBlock);
        std::memset(serial->get(), 1, static_cast<size_t>(kBlock));
    }).join();
    {
        waggle::ScopedZone const zone("numa: parallel init");
        WAGGLE_MEM_ALLOC_AT(parallel->get(), kBlock);
        std::thread low([&] {
            pin(cpu_a);
            std::memset(parallel->get(), 1, static_cast<size_t>(kBlock / 2));
        });
        std::thread high([&] {
            pin(cpu_b);
            std::memset(parallel->get() + kBlock / 2, 1, static_cast<size_t>(kBlock / 2));
        });
        low.join();
        high.join();
    }
    // The work, on the other node, over several of the consumer's passes.
    std::thread([&] {
        pin(cpu_b);
        waggle::ScopedZone const zone("numa: compute");
        spin_for(std::chrono::milliseconds(2000));
    }).join();
}

// Run by tests/numa_session.py: a thread moved to another node with its zone open.
TEST_CASE("NUMA - a program whose thread moves to another node mid-zone", "[.migrate]") {
    require_two_nodes();
    auto const [cpu_a, cpu_b] = two_nodes();
    std::thread([&] {
        pin(cpu_a);
        waggle::ScopedZone const zone("numa: migrating");
        spin_for(std::chrono::milliseconds(1200));
        pin(cpu_b);
        spin_for(std::chrono::milliseconds(1200));
    }).join();
}
