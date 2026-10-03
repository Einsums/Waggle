//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Sources.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <string>

#include "Profiler.hpp"
#include "Symbols.hpp"

TEST_CASE("The sources setting names sources in a comma list", "[sources]") {
    waggle::Settings s;
    s.sources = " openmp , cuda";
    CHECK(waggle::source_requested(s, "openmp"));
    CHECK(waggle::source_requested(s, "cuda"));
    CHECK_FALSE(waggle::source_requested(s, "open"));
    s.sources.clear();
    CHECK_FALSE(waggle::source_requested(s, "openmp"));
}

// "(anonymous namespace)::f" once came out as "namespace)::f": its '(' was taken for an
// argument list.
TEST_CASE("A zone named after a function drops its arguments and return type", "[sources]") {
    CHECK(waggle::short_function_name("einsums::pack(double*, int)") == "einsums::pack");
    CHECK(waggle::short_function_name("void einsums::pack<double>(double*, int) const") == "einsums::pack<double>");
    CHECK(waggle::short_function_name("(anonymous namespace)::region()") == "(anonymous namespace)::region");
    CHECK(waggle::short_function_name("std::vector<int, std::allocator<int> > f<std::pair<int, int> >()") == "f<std::pair<int, int> >");
    CHECK(waggle::short_function_name("ns::Functor::operator()(int)") == "ns::Functor::operator()");
    CHECK(waggle::short_function_name("plain_c_function") == "plain_c_function");
}

TEST_CASE("Every built-in source reports a state", "[sources]") {
    auto const statuses = waggle::source_statuses();
    auto const openmp   = std::ranges::find(statuses, "openmp", &waggle::SourceStatus::name);
    REQUIRE(openmp != statuses.end());
    // Not requested: off, whatever OpenMP this process has.
    CHECK(openmp->state == "off");

    // Requested in a process with no OpenMP runtime: it waits for one, and says what kind.
    waggle::configure({.sources = "openmp"});
    auto const waiting = waggle::ompt::status();
    CHECK(waiting.state == "waiting");
    CHECK(waiting.detail.find("libgomp") != std::string::npos);

    // The report says it too, where the zones would have been.
    std::ostringstream report;
    waggle::Profiler::instance().print(false, report);
    CHECK(report.str().find("Not recorded: source openmp is waiting") != std::string::npos);
}

namespace {
// Internal linkage: exported by nothing, so dladdr on Linux cannot name it; the module's own
// symbol table can.
[[gnu::noinline]] int waggle_symbol_probe(int x) {
    return x * 3 + 1;
}
} // namespace

// On Linux regions were named by bare addresses: dladdr knows only exported symbols, and a library
// built with hidden visibility exports few.
TEST_CASE("A code address is named after the function it is in, exported or not", "[sources]") {
    auto const       *address = reinterpret_cast<char const *>(&waggle_symbol_probe) + 4; // inside, not at its start
    std::string const name    = waggle::function_at(address);
    INFO(name);
#ifdef _WIN32
    CHECK(name.find(".exe+0x") != std::string::npos); // the module and the offset
#else
    CHECK(name == "(anonymous namespace)::waggle_symbol_probe");
#endif
    CHECK(waggle_symbol_probe(1) == 4); // keeps it
}
