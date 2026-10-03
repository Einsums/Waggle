//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// The OpenMP source, against a real OpenMP runtime with OMPT (LLVM's libomp). One executable, run
// twice: "[active]" with WAGGLE_SOURCES=openmp in the environment, so the source attaches when the
// runtime starts, and "[missed]" without it.

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <omp.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "OmptTypes.hpp"
#include "Sources.hpp"

#if __has_include(<omp-tools.h>)
#    include <omp-tools.h>

// Waggle writes the OMPT declarations out rather than include this header; they must agree.
namespace own = waggle::ompt_types;
static_assert(static_cast<int>(own::ompt_callback_thread_begin) == ompt_callback_thread_begin);
static_assert(static_cast<int>(own::ompt_callback_parallel_begin) == ompt_callback_parallel_begin);
static_assert(static_cast<int>(own::ompt_callback_parallel_end) == ompt_callback_parallel_end);
static_assert(static_cast<int>(own::ompt_callback_implicit_task) == ompt_callback_implicit_task);
static_assert(static_cast<int>(own::ompt_callback_sync_region_wait) == ompt_callback_sync_region_wait);
static_assert(static_cast<int>(own::ompt_set_never) == ompt_set_never);
static_assert(static_cast<int>(own::ompt_thread_worker) == ompt_thread_worker);
static_assert(static_cast<int>(own::ompt_scope_begin) == ompt_scope_begin);
static_assert(static_cast<int>(own::ompt_scope_end) == ompt_scope_end);
static_assert(static_cast<int>(own::ompt_sync_region_barrier_explicit) == ompt_sync_region_barrier_explicit);
static_assert(static_cast<int>(own::ompt_sync_region_barrier_implicit_parallel) == ompt_sync_region_barrier_implicit_parallel);
static_assert(static_cast<int>(own::ompt_sync_region_barrier_teams) == ompt_sync_region_barrier_teams);
static_assert(static_cast<int>(own::ompt_task_initial) == ompt_task_initial);
static_assert(sizeof(own::ompt_data_t) == sizeof(::ompt_data_t));
static_assert(sizeof(own::ompt_start_tool_result_t) == sizeof(::ompt_start_tool_result_t));
#endif

namespace {

constexpr int kThreads = 4;
/// How the regions' zones name the function below.
constexpr char const *kRegionFunction = "(anonymous namespace)::ompt_test_region";

/// The work the regions below do, so a barrier has someone to wait for.
std::atomic<long> g_sink{0};

// Not inlined, so the region's call site is in a function of this name. The region shares a local
// with it, as real ones do: a region that shares nothing can end its function with a jump to the
// runtime instead of a call, and is then named after the caller (see Ompt.cpp).
[[gnu::noinline]] void ompt_test_region() {
    long total = 0;
    // clang-format off
#pragma omp parallel num_threads(kThreads) reduction(+ : total)
    // clang-format on
    {
        for (int i = 0; i < 100000 * (omp_get_thread_num() + 1); ++i) {
            total += i % 7;
        }
#pragma omp barrier
    }
    g_sink += total;
}

auto child(waggle::SnapshotNode const &node, std::string_view prefix) -> std::optional<waggle::SnapshotNode> {
    for (auto const &c : node.children()) {
        if (c.name().starts_with(prefix)) {
            return c;
        }
    }
    return std::nullopt;
}

auto names(waggle::SnapshotNode const &node) -> std::string {
    std::string out;
    for (auto const &c : node.children()) {
        out += std::string(c.name()) + "; ";
    }
    return out;
}

} // namespace

TEST_CASE("The OpenMP source attaches when the runtime starts", "[ompt][active]") {
    omp_set_num_threads(kThreads); // starts the runtime, if nothing has
    auto const status = waggle::ompt::status();
    INFO(status.detail);
    CHECK(status.state == "active");
    CHECK_FALSE(status.detail.empty()); // the runtime's version
}

TEST_CASE("A parallel region, each thread's share of it and its barrier waits are zones", "[ompt][active]") {
    {
        waggle::ScopedZone const outer("ompt: outer");
        ompt_test_region();
    }

    auto const snapshot = waggle::Snapshot::take();
    auto const outer    = snapshot.find("ompt: outer");
    REQUIRE(outer);
    INFO("below outer: " << names(*outer));
    // On the thread that met the region: the region, its own share, and the waits inside that.
    auto const region = child(*outer, "omp parallel: ");
    REQUIRE(region);
    CHECK(region->name() == std::string("omp parallel: ") + kRegionFunction);
    CHECK(region->domain() == "openmp");
    auto const work = child(*region, "omp work: ");
    REQUIRE(work);
    INFO("below the primary thread's work: " << names(*work));
    CHECK(child(*work, "omp barrier wait"));

    // Every other thread of the team: its share at the root, with its waits.
    int workers = 0;
    for (auto const &thread : snapshot.threads()) {
        if (auto const share = child(thread.root, std::string("omp work: ") + kRegionFunction)) {
            ++workers;
            CHECK(thread.name.starts_with("OpenMP worker"));
            CHECK(child(*share, "omp barrier wait"));
        }
    }
    CHECK(workers == kThreads - 1);
}

TEST_CASE("The openmp domain switch mutes the source and keeps zones paired", "[ompt][active]") {
    waggle_domain_set_enabled("openmp", 6, 0);
    {
        waggle::ScopedZone const outer("ompt: muted");
        ompt_test_region();
    }
    // Switched back on inside a region: its ends must not close zones it never opened.
    {
        waggle::ScopedZone const outer("ompt: switched inside");
#pragma omp parallel num_threads(kThreads)
        {
#pragma omp master
            waggle_domain_set_enabled("openmp", 6, 1);
#pragma omp barrier
        }
    }
    waggle_domain_set_enabled("openmp", 6, 1);
    {
        waggle::ScopedZone const after("ompt: after");
    }

    auto const snapshot = waggle::Snapshot::take();
    auto const muted    = snapshot.find("ompt: muted");
    REQUIRE(muted);
    INFO("below muted: " << names(*muted));
    CHECK(muted->children().empty());
    // Had a stray end closed "switched inside", "after" would sit below something else.
    CHECK(snapshot.find("ompt: after"));
    CHECK(snapshot.find("ompt: switched inside"));
}

TEST_CASE("A source requested after the runtime started says so", "[ompt][missed]") {
    omp_set_num_threads(kThreads);
    ompt_test_region(); // the runtime starts, and the source, not requested, declines

    CHECK(waggle::ompt::status().state == "off");
    waggle::configure({.sources = "openmp"});
    auto const status = waggle::ompt::status();
    CHECK(status.state == "missed");
    CHECK(status.detail.find("WAGGLE_SOURCES=openmp") != std::string::npos);
}
