//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Sampling.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Clock.hpp>
#include <Waggle/Waggle.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "Process.hpp"
#include "Profiler.hpp"
#include "Sources.hpp"

using namespace waggle;

namespace {

/// A perf record as the kernel writes it: the header, then @p words.
auto record(uint32_t type, std::vector<uint64_t> const &words) -> std::vector<unsigned char> {
    std::vector<unsigned char> bytes(8 + 8 * words.size());
    auto const                 size = static_cast<uint16_t>(bytes.size());
    std::memcpy(bytes.data(), &type, 4);
    std::memcpy(bytes.data() + 6, &size, 2);
    std::memcpy(bytes.data() + 8, words.data(), 8 * words.size());
    return bytes;
}

auto monotonic_ns(std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now()) -> uint64_t {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count());
}

auto find_node(AggNode const &root, std::string const &name) -> AggNode const * {
    std::vector<AggNode const *> pending{&root};
    while (!pending.empty()) {
        auto const *node = pending.back();
        pending.pop_back();
        if (node->name == name) {
            return node;
        }
        for (auto const &child : node->children) {
            pending.push_back(child.second.get());
        }
    }
    return nullptr;
}

/// The samples credited to zone @p name on any thread.
auto samples_in(std::string const &name) -> SampleCounts {
    auto         lock = Profiler::instance().consumer()->lock_shared();
    SampleCounts total;
    for (auto const &[id, ts] : Profiler::instance().consumer()->thread_data()) {
        if (auto const *node = find_node(ts.root, name)) {
            total.total += node->samples.total;
            for (auto const &[address, count] : node->samples.by_address) {
                total.by_address[address] += count;
            }
        }
    }
    return total;
}

auto unzoned_at(uint64_t address) -> uint64_t {
    auto        lock    = Profiler::instance().consumer()->lock_shared();
    auto const &unzoned = Profiler::instance().consumer()->unzoned_samples();
    auto const  it      = unzoned.by_address.find(address);
    return it == unzoned.by_address.end() ? 0 : it->second;
}

/// Ask for the sampling source, as the merge needs: every thread then writes a zone's Push as it
/// opens. On Linux the kernel samples too, so the tests below look for their own made-up addresses,
/// which no real instruction has, rather than at totals.
void ask_for_sampling() {
    SettingsUpdate update;
    update.sources = "sampling";
    configure(update);
}

void spin_for(std::chrono::microseconds length) {
    auto const until = std::chrono::steady_clock::now() + length;
    while (std::chrono::steady_clock::now() < until) {
    }
}

} // namespace

TEST_CASE("Sampling - a sample record gives its address, thread and time", "[sampling]") {
    // ip, then pid and tid in one word, then time
    auto const       bytes = record(9, {0x401234, (uint64_t{4321} << 32) | 1234, 5'000'000'000});
    sampling::Sample sample{};
    uint64_t         lost = 0;
    REQUIRE(sampling::parse_record(bytes.data(), bytes.size(), sample, lost) == sampling::Record::Sample);
    CHECK(sample.ip == 0x401234);
    CHECK(sample.tid == 4321);
    CHECK(sample.time_ns == 5'000'000'000);
    CHECK(lost == 0);
}

TEST_CASE("Sampling - a call chain gives the user frames, innermost first, without the kernel's markers", "[sampling]") {
    constexpr uint64_t kContextUser = ~uint64_t{512} + 1; // PERF_CONTEXT_USER, -512
    auto const bytes = record(9, {0x401234, (uint64_t{4321} << 32) | 1234, 5'000'000'000, 4, kContextUser, 0x401234, 0x402000, 0x403000});
    sampling::Sample sample{};
    uint64_t         lost = 0;
    REQUIRE(sampling::parse_record(bytes.data(), bytes.size(), sample, lost) == sampling::Record::Sample);
    REQUIRE(sample.chain_size == 3);
    CHECK(sample.chain[0] == 0x401234);
    CHECK(sample.chain[1] == 0x402000);
    CHECK(sample.chain[2] == 0x403000);
    // A count past the record's end is cut to what it holds.
    auto const short_chain = record(9, {0x401234, 0, 1, 100, 0x401234});
    REQUIRE(sampling::parse_record(short_chain.data(), short_chain.size(), sample, lost) == sampling::Record::Sample);
    CHECK(sample.chain_size == 1);
}

TEST_CASE("Sampling - lost records add up, and other or short records are passed over", "[sampling]") {
    sampling::Sample sample{};
    uint64_t         lost   = 0;
    auto const       lost_a = record(2, {7, 12});
    auto const       lost_b = record(2, {7, 30});
    CHECK(sampling::parse_record(lost_a.data(), lost_a.size(), sample, lost) == sampling::Record::Lost);
    CHECK(sampling::parse_record(lost_b.data(), lost_b.size(), sample, lost) == sampling::Record::Lost);
    CHECK(lost == 42);

    auto const throttle = record(5, {1, 2, 3});
    CHECK(sampling::parse_record(throttle.data(), throttle.size(), sample, lost) == sampling::Record::Other);

    auto const shortened = record(9, {0x401234});
    CHECK(sampling::parse_record(shortened.data(), shortened.size(), sample, lost) == sampling::Record::Malformed);
    CHECK(sampling::parse_record(shortened.data(), 4, sample, lost) == sampling::Record::Malformed);
}

TEST_CASE("Sampling - a sample's time becomes the ticks zones are stamped with", "[sampling]") {
    // Against a pair of reads taken a millisecond earlier, as the consumer takes one each pass: the
    // second pair's instant converts to its own tick count to within a few microseconds. Each pair
    // is as sure as the gap between its two reads, which a loaded machine stretches past one; what
    // matters is staying far inside the millisecond between samples.
    auto const &clock  = TickClock::instance();
    auto const  anchor = sampling::ClockPair::now();
    spin_for(std::chrono::milliseconds(1));
    auto const later = sampling::ClockPair::now();
    auto const back  = sampling::monotonic_to_ticks(later.ns, anchor);
    auto const error = back > later.ticks ? back - later.ticks : later.ticks - back;
    CHECK(static_cast<double>(error) * clock.ns_per_tick < 10'000.0);
}

TEST_CASE("Sampling - each sample goes to the zone open on its thread when it was taken", "[sampling]") {
    ask_for_sampling();
    auto      &prof   = Profiler::instance();
    auto const thread = static_cast<uint32_t>(kernel_thread_id());
    // A thread's first zone registers it, which takes the tree's lock: done before the lock is held.
    prof.push("sampled_registration", "test.cpp", 0, "test");
    prof.pop();
    prof.flush();

    // The shared lock keeps the consumer from reading these zones' events until the samples are
    // handed over, as a pass reads the sampler's rings before the event rings. Samples are stamped
    // between the events, 50 us clear of each, so no timing of the machine's can reorder them.
    std::vector<sampling::Sample> samples;
    auto const                    sample_now = [&](uint64_t ip) {
        spin_for(std::chrono::microseconds(50));
        samples.push_back({.time_ns = monotonic_ns(), .ip = ip, .tid = thread});
        spin_for(std::chrono::microseconds(50));
    };
    {
        auto lock = prof.consumer()->lock_shared();
        sample_now(0xb0);
        prof.push("sampled_outer", "test.cpp", 1, "test");
        sample_now(0xb1);
        prof.push("sampled_inner", "test.cpp", 2, "test");
        sample_now(0xb2);
        prof.pop();
        sample_now(0xb3);
        prof.pop();
        sample_now(0xb4);
        prof.consumer()->add_samples(samples);
    }
    prof.flush();

    auto const outer = samples_in("sampled_outer");
    auto const inner = samples_in("sampled_inner");
    CHECK(outer.by_address.count(0xb1) == 1);
    CHECK(outer.by_address.count(0xb2) == 0);
    CHECK(outer.by_address.count(0xb3) == 1);
    CHECK(inner.by_address.count(0xb2) == 1);
    CHECK(unzoned_at(0xb0) == 1);
    CHECK(unzoned_at(0xb4) == 1);
}

TEST_CASE("Sampling - a sample read before its zone's events have told where it belongs waits", "[sampling]") {
    ask_for_sampling();
    auto      &prof   = Profiler::instance();
    auto const thread = static_cast<uint32_t>(kernel_thread_id());

    // A sample taken after a pass began is read in that pass, before any event says what was open
    // when it was taken: it waits for the next. Here one is stamped 5 ms ahead, inside a zone that
    // is open then: whichever pass credits it, early or late, must give it to that zone.
    prof.push("sampled_late", "test.cpp", 3, "test");
    auto const due = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    prof.consumer()->add_samples({{.time_ns = monotonic_ns(due), .ip = 0xc1, .tid = thread}});
    prof.flush();
    CHECK(unzoned_at(0xc1) == 0);
    std::this_thread::sleep_until(due + std::chrono::milliseconds(20));
    prof.pop();
    prof.flush();

    CHECK(samples_in("sampled_late").by_address.count(0xc1) == 1);
    CHECK(unzoned_at(0xc1) == 0);
}

TEST_CASE("Sampling - a sample on a thread that never records is in no zone", "[sampling]") {
    auto &prof = Profiler::instance();
    prof.consumer()->add_samples({{.time_ns = monotonic_ns(), .ip = 0xd1, .tid = 0x7ffffff0}});
    prof.flush();
    CHECK(unzoned_at(0xd1) == 1);
}

// Against the kernel: run with WAGGLE_SOURCES=sampling. Skipped, saying why, where perf_event
// cannot sample (macOS, a kernel.perf_event_paranoid above 2, a container's seccomp profile).
namespace {

/// Whether the environment says this machine must sample: WAGGLE_REQUIRE_COUNTERS, which CI's
/// Linux job sets once it lowers kernel.perf_event_paranoid, or WAGGLE_REQUIRE_PMU (the
/// WAGGLE_REQUIRE_PMU_TEST option), on a machine meant to have hardware counters. Linux only:
/// elsewhere there is nothing to sample with.
auto sampling_required() -> bool {
#if defined(__linux__)
    return std::getenv("WAGGLE_REQUIRE_COUNTERS") != nullptr ||
           std::getenv("WAGGLE_REQUIRE_PMU") != nullptr; // NOLINT(concurrency-mt-unsafe)
#else
    return false;
#endif
}

void require_sampling() {
    auto &prof   = Profiler::instance();
    auto  status = sampling::status();
    for (int i = 0; i < 200 && status.state == "waiting"; ++i) {
        prof.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        status = sampling::status();
    }
    std::string const why = "sampling is " + status.state + ": " + status.detail;
    if (status.state != "active") {
        if (sampling_required()) {
            FAIL(why);
        }
        SKIP(why);
    }
    // A machine meant to have a PMU samples cycles, not the software clock it falls back to.
    if (std::getenv("WAGGLE_REQUIRE_PMU") != nullptr &&
        prof.consumer()->sampling_state().event != "cycles") { // NOLINT(concurrency-mt-unsafe)
        FAIL("WAGGLE_REQUIRE_PMU is set, but " + why);
    }
}

} // namespace

TEST_CASE("Sampling - zones alternating 3 to 1 get samples 3 to 1", "[source]") {
    require_sampling();
    auto &prof = Profiler::instance();
    // About a millisecond each round, but not exactly: a fixed period beats against the sampling
    // rate (1000 Hz against 997 Hz), and the samples then drift slowly through the round and
    // overweight whichever zone they linger in.
    std::mt19937                       random(12345);
    std::uniform_int_distribution<int> round_us(600, 1400);
    auto const                         end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end) {
        int const round = round_us(random);
        prof.push("alternating_a", "test.cpp", 10, "test");
        spin_for(std::chrono::microseconds(round * 3 / 4));
        prof.pop();
        prof.push("alternating_b", "test.cpp", 11, "test");
        spin_for(std::chrono::microseconds(round / 4));
        prof.pop();
    }
    prof.flush();

    auto const a = samples_in("alternating_a").total;
    auto const b = samples_in("alternating_b").total;
    INFO("a: " << a << ", b: " << b << ", " << sampling::status().detail);
    // Cycles are sampled only while the thread runs, which a loaded machine cuts short: the share is
    // the check, and a few hundred samples bound it well enough.
    REQUIRE(a + b >= 600);
    double const share = static_cast<double>(a) / static_cast<double>(a + b);
    CHECK(share > 0.70);
    CHECK(share < 0.80);
}

TEST_CASE("Sampling - a thread that records no zones is sampled, in no zone", "[source]") {
    require_sampling();
    auto &prof = Profiler::instance();
    prof.flush();
    uint64_t before = 0;
    {
        auto lock = prof.consumer()->lock_shared();
        before    = prof.consumer()->unzoned_samples().total;
    }
    // A pool thread, as a BLAS library's: never registered, found by the rescan within a second.
    std::thread pool([] { spin_for(std::chrono::milliseconds(2500)); });
    pool.join();
    prof.flush();
    uint64_t after = 0;
    {
        auto lock = prof.consumer()->lock_shared();
        after     = prof.consumer()->unzoned_samples().total;
    }
    INFO(sampling::status().detail);
    CHECK(after - before >= 300); // found within a second, it spins 1.5 s more at 997 Hz, if it gets the CPU
}

// Run by tests/sampling_session.py with WAGGLE_SOURCES=sampling and WAGGLE_SAVE set: zones whose
// shares it knows, for `waggle cpu` to read back from the saved session.
TEST_CASE("Sampling - a program for the saved session", "[.program]") {
    auto                              &prof = Profiler::instance();
    std::mt19937                       random(54321);
    std::uniform_int_distribution<int> round_us(600, 1400);
    auto const                         end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end) {
        int const round = round_us(random);
        prof.push("saved_a", "test.cpp", 20, "test");
        spin_for(std::chrono::microseconds(round * 3 / 4));
        prof.pop();
        prof.push("saved_b", "test.cpp", 21, "test");
        spin_for(std::chrono::microseconds(round / 4));
        prof.pop();
    }
}

namespace {

std::atomic<uint64_t> g_sink{0};

// The two loops of the hot_line program, each on a line of its own, marked so the test finds the
// lines; noinline, so each is a function of its own in the machine code too.
[[gnu::noinline]] auto hot_loop(uint64_t n) -> uint64_t {
    uint64_t x = 1;
    // clang-format off
    for (uint64_t i = 0; i < n; ++i) { x = x * 6364136223846793005ULL + i; } // WAGGLE_TEST_HOT_LINE
    // clang-format on
    return x;
}

[[gnu::noinline]] auto cool_loop(uint64_t n) -> uint64_t {
    uint64_t x = 3;
    // clang-format off
    for (uint64_t i = 0; i < n; ++i) { x = x * 2862933555777941757ULL + i; } // WAGGLE_TEST_COOL_LINE
    // clang-format on
    return x;
}

} // namespace

// Run by tests/sampling_session.py: a zone whose work is nine parts one loop and one part another,
// for `waggle cpu --lines` to find the first loop's line with about 90% of the zone's samples. The
// clock is read once a round, not in the loops, so its time stays out of their lines.
TEST_CASE("Sampling - a program with one hot line", "[.hot_line]") {
    auto      &prof = Profiler::instance();
    auto const end  = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end) {
        prof.push("hot_line", "test.cpp", 30, "test");
        g_sink += hot_loop(9'000'000);
        g_sink += cool_loop(1'000'000);
        prof.pop();
    }
}

extern "C" auto waggle_test_library_work(uint64_t n) -> uint64_t; // tests/unit/SamplingLibrary.cpp

// Run by tests/sampling_session.py: a zone that calls a library without line information from two
// lines, with three times the work from the first, for `waggle cpu --lines` to credit the library's
// samples to the two lines about 3 to 1.
TEST_CASE("Sampling - a program calling a library from two lines", "[.caller_lines]") {
    auto      &prof = Profiler::instance();
    auto const end  = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end) {
        prof.push("caller_lines", "test.cpp", 40, "test");
        g_sink += waggle_test_library_work(3'000'000); // WAGGLE_TEST_CALLER_A
        g_sink += waggle_test_library_work(1'000'000); // WAGGLE_TEST_CALLER_B
        prof.pop();
    }
}
