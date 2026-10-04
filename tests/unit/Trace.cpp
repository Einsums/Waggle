//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// The Perfetto trace: the writer directly, and the collector writing one. The packets are read
// back with a small protobuf reader that checks what this file relies on; tests/perfetto_trace.py has
// Perfetto's own trace_processor read the [program] case's trace.

#include "Trace.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Clock.hpp>
#include <Waggle/Waggle.h>
#include <Waggle/Waggle.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Sites.hpp"
#include "StringTable.hpp"

namespace {

// ---- A protobuf reader, enough for the packets the writer makes ----------------------------

struct Field {
    uint32_t         number{0};
    uint32_t         wire{0};
    uint64_t         value{0}; ///< a varint's or fixed64's
    std::string_view bytes;    ///< a length-delimited field's
};

auto read_varint(std::string_view &in) -> uint64_t {
    uint64_t v     = 0;
    int      shift = 0;
    while (true) {
        REQUIRE(!in.empty());
        auto const byte = static_cast<uint8_t>(in.front());
        in.remove_prefix(1);
        v |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return v;
        }
        shift += 7;
        REQUIRE(shift < 64);
    }
}

auto fields(std::string_view in) -> std::vector<Field> {
    std::vector<Field> out;
    while (!in.empty()) {
        uint64_t const key = read_varint(in);
        Field          f{.number = static_cast<uint32_t>(key >> 3), .wire = static_cast<uint32_t>(key & 7)};
        switch (f.wire) {
        case 0:
            f.value = read_varint(in);
            break;
        case 1:
            REQUIRE(in.size() >= 8);
            std::memcpy(&f.value, in.data(), 8);
            in.remove_prefix(8);
            break;
        case 2: {
            uint64_t const n = read_varint(in);
            REQUIRE(in.size() >= n);
            f.bytes = in.substr(0, n);
            in.remove_prefix(n);
            break;
        }
        default:
            FAIL("wire type " << f.wire << " is not one the writer uses");
        }
        out.push_back(f);
    }
    return out;
}

auto first(std::vector<Field> const &fs, uint32_t number) -> std::optional<Field> {
    for (auto const &f : fs) {
        if (f.number == number) {
            return f;
        }
    }
    return std::nullopt;
}

/// A TracePacket, as far as these tests look into one.
struct Packet {
    uint32_t                                      sequence{0};
    uint64_t                                      flags{0};
    std::optional<uint64_t>                       timestamp;
    bool                                          clock_snapshot{false};
    std::optional<uint64_t>                       event_type;
    uint64_t                                      name_iid{0};
    std::string                                   name; ///< an event's own, not interned, name
    std::vector<std::pair<std::string, uint64_t>> args; ///< by name (resolved), with a varint value
    std::map<uint64_t, std::string>               interned_names;
    std::map<uint64_t, std::string>               interned_arg_names;
    std::optional<std::string>                    thread_name; ///< a thread track's descriptor
    std::optional<std::string>                    track_name;  ///< any other track's descriptor
};

auto read_trace(std::filesystem::path const &path) -> std::vector<Packet> {
    std::ifstream       file(path, std::ios::binary);
    std::string const   data{std::istreambuf_iterator<char>(file), {}};
    std::vector<Packet> out;
    for (auto const &top : fields(data)) {
        REQUIRE(top.number == 1); // Trace.packet
        REQUIRE(top.wire == 2);
        auto const fs = fields(top.bytes);
        Packet     p;
        p.sequence       = static_cast<uint32_t>(first(fs, 10).value_or(Field{}).value);
        p.flags          = first(fs, 13).value_or(Field{}).value;
        p.clock_snapshot = first(fs, 6).has_value();
        if (auto ts = first(fs, 8)) {
            p.timestamp = ts->value;
        }
        if (auto interned = first(fs, 12)) {
            for (auto const &entry : fields(interned->bytes)) {
                auto const e    = fields(entry.bytes);
                auto const iid  = first(e, 1).value_or(Field{}).value;
                auto const name = std::string(first(e, 2).value_or(Field{}).bytes);
                if (entry.number == 2) {
                    p.interned_names[iid] = name;
                } else if (entry.number == 3) {
                    p.interned_arg_names[iid] = name;
                }
            }
        }
        if (auto event = first(fs, 11)) {
            auto const e = fields(event->bytes);
            p.event_type = first(e, 9).value_or(Field{}).value;
            p.name_iid   = first(e, 10).value_or(Field{}).value;
            p.name       = std::string(first(e, 23).value_or(Field{}).bytes);
            for (auto const &f : e) {
                if (f.number == 4) { // debug annotation
                    auto const  a    = fields(f.bytes);
                    std::string name = std::string(first(a, 10).value_or(Field{}).bytes);
                    if (name.empty()) {
                        name = "#" + std::to_string(first(a, 1).value_or(Field{}).value);
                    }
                    uint64_t value = 0;
                    for (auto const &v : a) {
                        if (v.number >= 2 && v.number <= 4) {
                            value = v.value;
                        }
                    }
                    p.args.emplace_back(name, value);
                }
            }
        }
        if (auto desc = first(fs, 60)) {
            auto const d = fields(desc->bytes);
            if (auto thread = first(d, 4)) {
                p.thread_name = std::string(first(fields(thread->bytes), 5).value_or(Field{}).bytes);
            } else if (auto name = first(d, 2)) {
                p.track_name = std::string(name->bytes);
            }
        }
        out.push_back(std::move(p));
    }
    return out;
}

/// A sequence's zones, resolved: each begin's name with the arguments its end carried.
struct Zone {
    std::string                                   name;
    size_t                                        depth;
    std::vector<std::pair<std::string, uint64_t>> args;
};

/// Replay sequence @p sequence: interned names, begins and ends matched by nesting. Checks the
/// sequence starts by clearing its state and gives its clock, and that every end has its begin.
auto zones_of(std::vector<Packet> const &packets, uint32_t sequence) -> std::vector<Zone> {
    std::map<uint64_t, std::string> names;
    std::map<uint64_t, std::string> arg_names;
    std::vector<size_t>             open;
    std::vector<Zone>               out;
    bool                            started = false;
    for (auto const &p : packets) {
        if (p.sequence != sequence) {
            continue;
        }
        if (!started) {
            CHECK((p.flags & 1) != 0); // SEQ_INCREMENTAL_STATE_CLEARED
            CHECK(p.clock_snapshot);
            started = true;
        }
        names.insert(p.interned_names.begin(), p.interned_names.end());
        arg_names.insert(p.interned_arg_names.begin(), p.interned_arg_names.end());
        if (p.event_type == 1u) {
            REQUIRE(names.contains(p.name_iid)); // interned in this packet or one before it
            open.push_back(out.size());
            out.push_back({.name = names[p.name_iid], .depth = open.size() - 1, .args = {}});
        } else if (p.event_type == 2u) {
            REQUIRE(!open.empty());
            for (auto [name, value] : p.args) {
                if (name.starts_with('#')) {
                    auto const iid = std::stoull(name.substr(1));
                    REQUIRE(arg_names.contains(iid));
                    name = arg_names[iid];
                }
                out[open.back()].args.emplace_back(name, value);
            }
            open.pop_back();
        }
    }
    CHECK(started);
    return out;
}

auto temp_trace(char const *name) -> std::filesystem::path {
    return std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(waggle_current_thread_id()) + ".pftrace");
}

} // namespace

TEST_CASE("The writer interns names and closes a zone whose end was lost", "[trace][writer]") {
    waggle::StringTable strings;
    waggle::SiteTable   sites;
    waggle::DomainTable domains;
    auto const          path = temp_trace("waggle-writer");

    uint32_t const outer_id = strings.intern("outer");
    uint32_t const inner_id = strings.intern("inner");
    uint32_t const key_id   = strings.intern("n");
    uint32_t const site_id =
        sites.add({.name_id = outer_id, .file_id = strings.intern("a.cpp"), .func_id = 0, .line = 7, .domain = domains.add("lib")});

    uint64_t const t0      = waggle::TickClock::now();
    auto const     span_ns = [&](uint64_t ticks) {
        auto const &clock = waggle::TickClock::instance();
        return static_cast<uint64_t>(std::chrono::nanoseconds(clock.to_time_point(t0 + ticks) - clock.to_time_point(t0)).count());
    };
    {
        waggle::TraceWriter trace(strings, sites);
        REQUIRE(trace.open(path.string(), domains).empty());
        trace.thread(1, 1234, "worker");
        trace.begin(1, t0, site_id, outer_id);
        trace.annotate(1, {.key_id = key_id, .int_val = 3});
        trace.annotate(1, {.key_id = key_id, .int_val = 4}); // the last value for a key is kept
        trace.begin(1, t0 + 10, 0, inner_id);
        trace.end_lost(1, t0 + 20); // the inner zone's end was dropped
        trace.end(1, t0 + 5);       // earlier than the time before it: written as that time
        trace.end(1, t0 + 30);      // nothing open: ignored
        trace.begin(1, t0 + 40, 0, outer_id);
        trace.end(1, t0 + 50);
        trace.close();
        CHECK(trace.zones() == 3);
    }

    auto const packets = read_trace(path);
    auto const zones   = zones_of(packets, 3); // thread 1's sequence
    REQUIRE(zones.size() == 3);
    CHECK(zones[0].name == "outer");
    CHECK(zones[0].args == std::vector<std::pair<std::string, uint64_t>>{{"n", 4}});
    CHECK(zones[1].name == "inner");
    CHECK(zones[1].depth == 1);
    CHECK(zones[1].args == std::vector<std::pair<std::string, uint64_t>>{{"end_lost", 1}});
    CHECK(zones[2].name == "outer");
    CHECK(zones[2].depth == 0);

    // "outer" was interned once on the sequence, though begun twice.
    size_t interned = 0;
    for (auto const &p : packets) {
        interned += p.sequence == 3 && p.interned_names.contains(uint64_t{outer_id} + 1) ? 1 : 0;
    }
    CHECK(interned == 1);

    // Timestamps are deltas in nanoseconds, never going back.
    uint64_t sum = 0;
    for (auto const &p : packets) {
        if (p.sequence == 3 && p.timestamp) {
            sum += *p.timestamp;
        }
    }
    CHECK(sum == span_ns(50));

    bool named = false;
    for (auto const &p : packets) {
        named = named || p.thread_name == "worker";
    }
    CHECK(named);
    std::filesystem::remove(path);
}

TEST_CASE("Reopening the trace starts a new file with nothing carried over", "[trace][writer]") {
    waggle::StringTable strings;
    waggle::SiteTable   sites;
    waggle::DomainTable domains;
    auto const          first_path  = temp_trace("waggle-first");
    auto const          second_path = temp_trace("waggle-second");
    uint32_t const      name_id     = strings.intern("zone");

    waggle::TraceWriter trace(strings, sites);
    REQUIRE(trace.open(first_path.string(), domains).empty());
    trace.begin(2, waggle::TickClock::now(), 0, name_id);
    REQUIRE(trace.open(second_path.string(), domains).empty());
    trace.end(2, waggle::TickClock::now()); // its begin went to the first file
    trace.begin(2, waggle::TickClock::now(), 0, name_id);
    trace.end(2, waggle::TickClock::now());
    trace.close();

    // The second file names the zone itself: interning started over.
    auto const zones = zones_of(read_trace(second_path), 4);
    REQUIRE(zones.size() == 1);
    CHECK(zones[0].name == "zone");
    CHECK(zones_of(read_trace(first_path), 4).size() == 1); // begun, never ended
    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
}

TEST_CASE("A trace that cannot be written says why", "[trace][writer]") {
    waggle::StringTable strings;
    waggle::SiteTable   sites;
    waggle::DomainTable domains;
    waggle::TraceWriter trace(strings, sites);
    auto const          why = trace.open("/nonexistent-directory/trace.pftrace", domains);
    CHECK(why.find("/nonexistent-directory/trace.pftrace") != std::string::npos);
    CHECK_FALSE(trace.is_open());
}

namespace {

/// What tests/perfetto_trace.py checks with trace_processor, and the test below with the reader above.
void record_program() {
    waggle::set_thread_name("trace main");
    {
        waggle::ScopedZone const outer("trace: outer");
        waggle::annotate("n", int64_t{7});
        waggle::annotate("label", "text");
        for (int i = 0; i < 2; ++i) {
            waggle::ScopedZone const inner("trace: inner");
        }
        static char block[4096];
        waggle_mem_alloc(block, sizeof block);
        waggle_mem_free(block, sizeof block);
    }
    uint64_t token = 0;
    {
        waggle::ScopedZone const launch("trace: launch");
        token = waggle_device_submit();
    }
    int64_t const now = waggle_steady_ns();
    waggle_device_span(waggle_intern("trace queue", 11), 0, waggle_intern("trace: kernel", 13), now, now + 1'000'000, token);
    std::thread([] {
        waggle::set_thread_name("trace worker");
        waggle::ScopedZone const work("trace: work");
    }).join();
    // Enough small zones to measure what one costs in the file.
    for (int i = 0; i < 20000; ++i) {
        waggle::ScopedZone const leaf("trace: leaf");
    }
    waggle::flush();
}

} // namespace

TEST_CASE("The collector writes every zone to the trace", "[trace][collector]") {
    auto const path = temp_trace("waggle-collector");
    waggle::override_settings({.trace = path.string()});
    record_program();
    waggle::override_settings({.trace = std::string{}});

    auto const packets = read_trace(path);
    auto const bytes   = std::filesystem::file_size(path);
    uint32_t   main    = 0;
    for (auto const &p : packets) {
        if (p.event_type == 1u && p.sequence > 1 && main == 0) {
            main = p.sequence;
        }
    }
    auto const                    zones = zones_of(packets, main);
    std::map<std::string, size_t> count;
    for (auto const &z : zones) {
        ++count[z.name];
    }
    CHECK(count["trace: outer"] == 1);
    CHECK(count["trace: inner"] == 2);
    CHECK(count["trace: launch"] == 1);
    CHECK(count["trace: leaf"] == 20000);
    CHECK(zones[0].args == std::vector<std::pair<std::string, uint64_t>>{{"n", 7}, {"label", 0}});

    // A zone with nothing inside it costs about 32 bytes: a begin of 18 (its sequence, time,
    // flags, type, name and call site) and an end of 13.
    INFO("bytes " << bytes << " for " << zones.size() << " zones");
    CHECK(bytes < 36 * zones.size());

    bool worker = false;
    bool queue  = false;
    for (auto const &p : packets) {
        worker = worker || p.thread_name == "trace worker";
        queue  = queue || p.track_name == "trace queue";
    }
    CHECK(worker);
    CHECK(queue);
    std::filesystem::remove(path);
}

// For tests/perfetto_trace.py, which runs it with WAGGLE_TRACE set and reads the trace with Perfetto's
// trace_processor. Hidden: it writes wherever WAGGLE_TRACE says.
TEST_CASE("Record the program tests/perfetto_trace.py checks", "[.program]") {
    REQUIRE(std::getenv("WAGGLE_TRACE") != nullptr); // NOLINT(concurrency-mt-unsafe)
    record_program();
}
