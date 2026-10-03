//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// A zone's domain is found by name lookup from where the zone is written.

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Two libraries, as they would declare themselves. What follows stands in for their headers, so it
// keeps external linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
namespace lib_a {
WAGGLE_DEFINE_DOMAIN("lib_a")

constexpr std::string_view current() {
    return WAGGLE_CURRENT_DOMAIN;
}

namespace detail {
constexpr std::string_view nested() {
    return WAGGLE_CURRENT_DOMAIN;
}
} // namespace detail

/// A zone in a template: lookup happens where it is defined, not where it is used.
template <typename T>
void zone_in_template() {
    WAGGLE_ZONE("domains: lib_a template");
}

inline void zone_in_inline() {
    WAGGLE_ZONE("domains: lib_a inline");
}
} // namespace lib_a

namespace lib_b {
WAGGLE_DEFINE_DOMAIN("lib_b")

constexpr std::string_view current() {
    return WAGGLE_CURRENT_DOMAIN;
}

inline void zone() {
    WAGGLE_ZONE("domains: lib_b");
}

/// A zone of this library with an annotation and memory, entered inside the caller's zone.
inline void annotated() {
    WAGGLE_ZONE("domains: lib_b annotated");
    WAGGLE_ANNOTATE("lib_b key", "lib_b value");
    WAGGLE_MEM_ALLOC(64);
}
} // namespace lib_b

namespace lib_a {
/// lib_a's zone around a call into lib_b.
inline void calls_b() {
    WAGGLE_ZONE("domains: lib_a calls b");
    lib_b::annotated();
}
} // namespace lib_a

// An application namespace that brings both libraries into view: its zones are its own.
namespace app {
using namespace lib_a;
using namespace lib_b;

constexpr std::string_view current() {
    return WAGGLE_CURRENT_DOMAIN;
}
} // namespace app

// Application code at global scope, with both libraries in view: the unnamed domain, and no
// ambiguity between them.
using namespace lib_a;
using namespace lib_b;

constexpr std::string_view global_current() {
    return WAGGLE_CURRENT_DOMAIN;
}

// NOLINTEND(misc-use-internal-linkage)

static_assert(lib_a::current() == "lib_a");
static_assert(lib_a::detail::nested() == "lib_a");
static_assert(lib_b::current() == "lib_b");
static_assert(app::current().empty());
static_assert(global_current().empty());

namespace {

/// The zone named @p name anywhere in a fresh snapshot.
std::optional<std::string> domain_of(std::string const &name) {
    auto const                        snapshot = waggle::Snapshot::take();
    std::vector<waggle::SnapshotNode> pending;
    for (auto const &thread : snapshot.threads()) {
        pending.push_back(thread.root);
    }
    while (!pending.empty()) {
        auto const node = pending.back();
        pending.pop_back();
        for (auto const &child : node.children()) {
            if (child.name() == name) {
                return std::string(child.domain());
            }
            pending.push_back(child);
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("A zone belongs to the library whose namespace it is written in", "[profiler][domains]") {
    waggle::set_enabled(true);
    std::thread([] {
        lib_a::zone_in_template<int>();
        lib_a::zone_in_inline();
        lib_b::zone();
        WAGGLE_ZONE("domains: application");
    }).join();

    CHECK(domain_of("domains: lib_a template") == "lib_a");
    CHECK(domain_of("domains: lib_a inline") == "lib_a");
    CHECK(domain_of("domains: lib_b") == "lib_b");
    CHECK(domain_of("domains: application") == "");

    // Merging threads keeps each zone's domain.
    auto const merged = waggle::Snapshot::take(true);
    auto const zone   = merged.find(0, "domains: lib_b");
    REQUIRE(zone);
    CHECK(zone->domain() == "lib_b");
}

// Per-domain switches: a library can be switched off at run time, and its zones, annotations and
// memory then record nothing, without landing on the zone of the library around them.
TEST_CASE("A switched-off library records nothing, and nothing of it lands on its caller", "[profiler][domains]") {
    waggle::set_enabled(true);
    waggle::set_domain_enabled("lib_b", false);
    CHECK_FALSE(waggle::domain_enabled("lib_b"));
    CHECK(waggle::domain_enabled("lib_a"));
    std::thread([] { lib_a::calls_b(); }).join();
    waggle::set_domain_enabled("lib_b", true);

    auto const snapshot = waggle::Snapshot::take();
    auto const caller   = snapshot.find("domains: lib_a calls b");
    REQUIRE(caller);
    CHECK(caller->children().empty());
    for (auto const &[key, value] : caller->annotations()) {
        CHECK(key != "lib_b key");
    }
    CHECK(caller->stats().mem_alloc_bytes == 0);

    // Switched back on, it records again.
    std::thread([] { lib_a::calls_b(); }).join();
    auto const again = waggle::Snapshot::take().find("domains: lib_a calls b/domains: lib_b annotated");
    REQUIRE(again);
    CHECK(again->stats().call_count == 1);
    CHECK(again->stats().mem_alloc_bytes == 64);
}

TEST_CASE("The disabled_domains setting switches libraries off", "[profiler][domains]") {
    waggle::override_settings({.disabled_domains = "settings_a, settings_b"});
    CHECK_FALSE(waggle::domain_enabled("settings_a"));
    CHECK_FALSE(waggle::domain_enabled("settings_b"));
    CHECK(waggle::domain_enabled("settings_c"));
    CHECK(waggle::settings().disabled_domains == "settings_a, settings_b");
    waggle::set_domain_enabled("settings_a", true);
    waggle::set_domain_enabled("settings_b", true);
}
