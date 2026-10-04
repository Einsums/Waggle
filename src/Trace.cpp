//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Trace.hpp"

#include <Waggle/Clock.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "Diagnostics.hpp"
#include "Process.hpp"
#include "Sites.hpp"
#include "StringTable.hpp"

WAGGLE_NAMESPACE_BEGIN

namespace {

// Field numbers from Perfetto's protos (protos/perfetto/trace/...), for the messages written here.
namespace field {
constexpr uint32_t kTracePacket = 1;
namespace packet {
constexpr uint32_t kClockSnapshot     = 6;
constexpr uint32_t kTimestamp         = 8;
constexpr uint32_t kSequenceId        = 10; // trusted_packet_sequence_id
constexpr uint32_t kTrackEvent        = 11;
constexpr uint32_t kInternedData      = 12;
constexpr uint32_t kSequenceFlags     = 13;
constexpr uint32_t kTimestampClockId  = 58;
constexpr uint32_t kDefaults          = 59; // trace_packet_defaults
constexpr uint32_t kTrackDescriptor   = 60;
constexpr uint32_t kIncrementalClear  = 1;  // SEQ_INCREMENTAL_STATE_CLEARED
constexpr uint32_t kNeedsIncremental  = 2;  // SEQ_NEEDS_INCREMENTAL_STATE
constexpr uint32_t kDefaultsEventDefs = 11; // TracePacketDefaults.track_event_defaults
} // namespace packet
namespace event {
constexpr uint32_t kCategoryIids       = 3;
constexpr uint32_t kDebugAnnotations   = 4;
constexpr uint32_t kType               = 9;
constexpr uint32_t kNameIid            = 10;
constexpr uint32_t kTrackUuid          = 11;
constexpr uint32_t kName               = 23;
constexpr uint32_t kCounterValue       = 30;
constexpr uint32_t kSourceLocationIid  = 34;
constexpr uint32_t kDoubleCounterValue = 44;
constexpr uint32_t kFlowIds            = 47;
constexpr uint32_t kTerminatingFlowIds = 48;
constexpr uint64_t kSliceBegin         = 1;
constexpr uint64_t kSliceEnd           = 2;
constexpr uint64_t kInstant            = 3;
constexpr uint64_t kCounter            = 4;
} // namespace event
namespace track {
constexpr uint32_t kUuid       = 1;
constexpr uint32_t kName       = 2;
constexpr uint32_t kProcess    = 3;
constexpr uint32_t kThread     = 4;
constexpr uint32_t kParentUuid = 5;
constexpr uint32_t kCounter    = 8;
} // namespace track
namespace interned {
constexpr uint32_t kCategories      = 1;
constexpr uint32_t kEventNames      = 2;
constexpr uint32_t kArgNames        = 3;
constexpr uint32_t kSourceLocations = 4;
} // namespace interned
namespace arg {
constexpr uint32_t kNameIid     = 1;
constexpr uint32_t kBoolValue   = 2;
constexpr uint32_t kUintValue   = 3;
constexpr uint32_t kIntValue    = 4;
constexpr uint32_t kDoubleValue = 5;
constexpr uint32_t kStringValue = 6;
constexpr uint32_t kName        = 10;
} // namespace arg
// CLOCK_MONOTONIC's id among Perfetto's builtin clocks, and the first id a sequence may define.
constexpr uint64_t kMonotonicClock = 3;
constexpr uint64_t kSequenceClock  = 64;
} // namespace field

/// Appends protobuf fields to a buffer. A nested message is written with a 4-byte length that
/// @ref close shortens to its true size once the message is done.
class Proto {
  public:
    explicit Proto(std::string &out) : _out(out) {}

    void varint(uint64_t v) {
        while (v >= 0x80) {
            _out.push_back(static_cast<char>(v | 0x80));
            v >>= 7;
        }
        _out.push_back(static_cast<char>(v));
    }
    void key(uint32_t f, uint32_t wire) { varint((static_cast<uint64_t>(f) << 3) | wire); }
    void u(uint32_t f, uint64_t v) {
        key(f, 0);
        varint(v);
    }
    void i(uint32_t f, int64_t v) { u(f, static_cast<uint64_t>(v)); }
    void fixed64(uint32_t f, uint64_t v) {
        key(f, 1);
        for (int b = 0; b < 8; ++b) {
            _out.push_back(static_cast<char>((v >> (8 * b)) & 0xff));
        }
    }
    void dbl(uint32_t f, double d) {
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof bits);
        fixed64(f, bits);
    }
    void str(uint32_t f, std::string_view s) {
        key(f, 2);
        varint(s.size());
        _out.append(s);
    }
    /// Start nested message @p f; pass the result to @ref close.
    auto open(uint32_t f) -> size_t {
        key(f, 2);
        _out.append(4, '\0');
        return _out.size();
    }
    void close(size_t mark) {
        size_t n = _out.size() - mark;
        // A packet is far below the 256 MiB four bytes can say.
        char   len[4];
        size_t used = 0;
        do {
            len[used] = static_cast<char>(n & 0x7f);
            n >>= 7;
            if (n != 0) {
                len[used] = static_cast<char>(len[used] | 0x80);
            }
            ++used;
        } while (n != 0 && used < 4);
        std::memcpy(&_out[mark - 4], len, used);
        _out.erase(mark - 4 + used, 4 - used);
    }

  private:
    std::string &_out;
};

auto steady_ns(uint64_t ticks) -> uint64_t {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(TickClock::instance().to_time_point(ticks).time_since_epoch()).count());
}

auto now_ns() -> uint64_t {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Track kinds, in a uuid's bits 28-31 below the pid, so traces of several processes merge.
enum class Kind : uint64_t { Process = 1, Thread = 2, Energy = 3, Device = 4, Memory = 5 };

constexpr uint32_t kProcessSequence = 1;
constexpr size_t   kFlushBytes      = size_t{1} << 20;
constexpr uint64_t kFlushNs         = 1'000'000'000;

} // namespace

TraceWriter::TraceWriter(StringTable const &strings, SiteTable const &sites) : _strings(strings), _sites(sites) {
}

TraceWriter::~TraceWriter() {
    close();
}

namespace {

auto uuid(uint64_t pid, Kind kind, uint64_t id) -> uint64_t {
    return (pid << 32) | (static_cast<uint64_t>(kind) << 28) | (id & 0x0fff'ffffU);
}

} // namespace

auto TraceWriter::open(std::string const &path, DomainTable const &domains) -> std::string {
    close();
    std::FILE *file = std::fopen(path.c_str(), "wb"); // NOLINT(cppcoreguidelines-owning-memory)
    if (file == nullptr) {
        return fmt::format("cannot write the trace to {}: {}", path, std::strerror(errno)); // NOLINT(concurrency-mt-unsafe)
    }
    _file             = file;
    _path             = path;
    _domains          = &domains;
    _pid              = static_cast<uint64_t>(process_id());
    _zones            = 0;
    _process          = {.id = kProcessSequence};
    _memory_described = false;
    _threads.clear();
    _device_tracks.clear();
    _buffer.clear();

    uint64_t const ns = now_ns();
    _last_flush_ns    = ns;
    Proto p(_buffer);
    // The process sequence: absolute CLOCK_MONOTONIC times, which every other clock maps to.
    size_t const pkt = p.open(field::kTracePacket);
    p.u(field::packet::kSequenceId, kProcessSequence);
    p.u(field::packet::kSequenceFlags, field::packet::kIncrementalClear);
    size_t const defaults = p.open(field::packet::kDefaults);
    p.u(field::packet::kTimestampClockId, field::kMonotonicClock);
    p.close(defaults);
    size_t const snapshot = p.open(field::packet::kClockSnapshot);
    size_t const clock    = p.open(1);
    p.u(1, field::kMonotonicClock);
    p.u(2, ns);
    p.close(clock);
    p.u(2, field::kMonotonicClock); // primary_trace_clock
    p.close(snapshot);
    p.close(pkt);
    _process.started = true;

    size_t const desc_pkt = process_packet(ns, false);
    size_t const desc     = p.open(field::packet::kTrackDescriptor);
    p.u(field::track::kUuid, uuid(_pid, Kind::Process, 0));
    size_t const proc = p.open(field::track::kProcess);
    p.i(1, static_cast<int64_t>(_pid));
    p.str(6, executable_name());
    p.close(proc);
    p.close(desc);
    p.close(desc_pkt);
    return {};
}

void TraceWriter::close() {
    if (_file == nullptr) {
        return;
    }
    flush();
    std::fclose(_file); // NOLINT(cppcoreguidelines-owning-memory)
    _file = nullptr;
    diagnostic(DiagnosticLevel::Info, fmt::format("wrote {} zones to the trace {}", _zones, _path));
}

void TraceWriter::flush() {
    if (_file != nullptr && !_buffer.empty()) {
        std::fwrite(_buffer.data(), 1, _buffer.size(), _file);
        std::fflush(_file);
    }
    _buffer.clear();
    _last_flush_ns = now_ns();
}

void TraceWriter::maybe_flush() {
    if (_file != nullptr && (_buffer.size() >= kFlushBytes || now_ns() - _last_flush_ns >= kFlushNs)) {
        flush();
    }
}

auto TraceWriter::thread_state(uint32_t thread_id) -> Thread & {
    auto [it, created] = _threads.try_emplace(thread_id);
    if (created) {
        // 0 is no sequence and 1 the process's.
        it->second.seq.id = thread_id + 2;
        it->second.track  = uuid(_pid, Kind::Thread, thread_id);
    }
    return it->second;
}

void TraceWriter::thread(uint32_t thread_id, uint64_t kernel_thread, std::string const &name) {
    if (_file == nullptr) {
        return;
    }
    Thread &t = thread_state(thread_id);
    if (t.described && t.name == name && t.kernel_thread == kernel_thread) {
        return;
    }
    t.name          = name;
    t.kernel_thread = kernel_thread;
    describe_thread(thread_id, t);
}

void TraceWriter::describe_thread(uint32_t thread_id, Thread &t) {
    Proto        p(_buffer);
    size_t const pkt  = process_packet(now_ns(), false);
    size_t const desc = p.open(field::packet::kTrackDescriptor);
    p.u(field::track::kUuid, t.track);
    size_t const thread = p.open(field::track::kThread);
    p.i(1, static_cast<int64_t>(_pid));
    p.i(2, static_cast<int64_t>(t.kernel_thread != 0 ? t.kernel_thread : thread_id));
    p.str(5, !t.name.empty() ? std::string_view(t.name) : std::string_view(fmt::format("thread {}", thread_id)));
    p.close(thread);
    p.close(desc);
    p.close(pkt);
    t.described = true;
}

void TraceWriter::start_sequence(Thread &t, uint64_t ns) {
    // A clock of the sequence's own whose times are deltas from the packet before, anchored to
    // CLOCK_MONOTONIC here, and the thread's track as every event's default.
    Proto        p(_buffer);
    size_t const pkt = p.open(field::kTracePacket);
    p.u(field::packet::kSequenceId, t.seq.id);
    p.u(field::packet::kSequenceFlags, field::packet::kIncrementalClear);
    size_t const defaults = p.open(field::packet::kDefaults);
    p.u(field::packet::kTimestampClockId, field::kSequenceClock);
    size_t const event_defaults = p.open(field::packet::kDefaultsEventDefs);
    p.u(field::event::kTrackUuid, t.track);
    p.close(event_defaults);
    p.close(defaults);
    size_t const snapshot = p.open(field::packet::kClockSnapshot);
    size_t       clock    = p.open(1);
    p.u(1, field::kSequenceClock);
    p.u(2, ns);
    p.u(3, 1); // is_incremental
    p.close(clock);
    clock = p.open(1);
    p.u(1, field::kMonotonicClock);
    p.u(2, ns);
    p.close(clock);
    p.close(snapshot);
    p.close(pkt);
    t.seq.started = true;
    t.seq.last_ns = ns;
    t.seq.names.clear();
    t.seq.sites.clear();
    t.seq.categories.clear();
    t.seq.arg_names.clear();
}

auto TraceWriter::thread_packet(Thread &t, uint64_t ns) -> size_t {
    if (!t.seq.started) {
        start_sequence(t, ns);
    }
    // A thread's events arrive in the order it recorded them, so times only grow; a delta cannot
    // be negative, so a time that went back is written as the one before it.
    uint64_t const delta = ns > t.seq.last_ns ? ns - t.seq.last_ns : 0;
    t.seq.last_ns += delta;
    Proto        p(_buffer);
    size_t const pkt = p.open(field::kTracePacket);
    p.u(field::packet::kTimestamp, delta);
    p.u(field::packet::kSequenceId, t.seq.id);
    p.u(field::packet::kSequenceFlags, field::packet::kNeedsIncremental);
    return pkt;
}

auto TraceWriter::process_packet(uint64_t ns, bool interned) -> size_t {
    Proto        p(_buffer);
    size_t const pkt = p.open(field::kTracePacket);
    p.u(field::packet::kTimestamp, ns);
    p.u(field::packet::kSequenceId, kProcessSequence);
    if (interned) {
        p.u(field::packet::kSequenceFlags, field::packet::kNeedsIncremental);
    }
    return pkt;
}

void TraceWriter::begin(uint32_t thread_id, uint64_t ticks, uint32_t site_id, uint32_t name_id) {
    if (_file == nullptr) {
        return;
    }
    Thread &t = thread_state(thread_id);
    if (!t.described) {
        describe_thread(thread_id, t);
    }
    Site const   site     = _sites.get(site_id);
    std::string  category = site.domain != 0 ? _domains->name(site.domain) : std::string{};
    size_t const pkt      = thread_packet(t, steady_ns(ticks));

    // Ids become interning ids one up, since 0 is none.
    uint64_t const name_iid     = uint64_t{name_id} + 1;
    uint64_t const site_iid     = uint64_t{site_id} + 1;
    uint64_t const category_iid = uint64_t{site.domain} + 1;
    bool const     new_name     = t.seq.names.insert(name_iid).second;
    // A site with no file or function (a Python zone's, often) has no location worth showing.
    bool const located      = site_id != 0 && (site.file_id != 0 || site.func_id != 0);
    bool const new_site     = located && t.seq.sites.insert(site_iid).second;
    bool const new_category = !category.empty() && t.seq.categories.insert(category_iid).second;

    Proto        p(_buffer);
    size_t const event = p.open(field::packet::kTrackEvent);
    p.u(field::event::kType, field::event::kSliceBegin);
    p.u(field::event::kNameIid, name_iid);
    if (!category.empty()) {
        p.u(field::event::kCategoryIids, category_iid);
    }
    if (located) {
        p.u(field::event::kSourceLocationIid, site_iid);
    }
    p.close(event);

    if (new_name || new_site || new_category) {
        size_t const interned = p.open(field::packet::kInternedData);
        if (new_category) {
            size_t const m = p.open(field::interned::kCategories);
            p.u(1, category_iid);
            p.str(2, category);
            p.close(m);
        }
        if (new_name) {
            size_t const m = p.open(field::interned::kEventNames);
            p.u(1, name_iid);
            p.str(2, _strings.get(name_id));
            p.close(m);
        }
        if (new_site) {
            size_t const m = p.open(field::interned::kSourceLocations);
            p.u(1, site_iid);
            p.str(2, _strings.get(site.file_id));
            p.str(3, _strings.get(site.func_id));
            p.u(4, static_cast<uint64_t>(site.line));
            p.close(m);
        }
        p.close(interned);
    }
    p.close(pkt);
    t.open.emplace_back();
}

void TraceWriter::annotate(uint32_t thread_id, TraceArg const &arg) {
    if (_file == nullptr) {
        return;
    }
    Thread &t = thread_state(thread_id);
    if (t.open.empty()) {
        return;
    }
    // The zone's last value for a key, as its report shows.
    auto &args = t.open.back();
    for (auto &held : args) {
        if (held.key_id == arg.key_id) {
            held = arg;
            return;
        }
    }
    args.push_back(arg);
}

void TraceWriter::write_arg(Sequence &seq, TraceArg const &arg, std::vector<std::pair<uint32_t, std::string const *>> &new_names) {
    Proto          p(_buffer);
    uint64_t const iid = uint64_t{arg.key_id} + 1;
    if (seq.arg_names.insert(iid).second) {
        new_names.emplace_back(arg.key_id, &_strings.get(arg.key_id));
    }
    size_t const m = p.open(field::event::kDebugAnnotations);
    p.u(field::arg::kNameIid, iid);
    switch (arg.type) {
    case AnnotateValueType::String:
        p.str(field::arg::kStringValue, _strings.get(arg.string_id));
        break;
    case AnnotateValueType::Int64:
        if (arg.is_unsigned) {
            p.u(field::arg::kUintValue, static_cast<uint64_t>(arg.int_val));
        } else {
            p.i(field::arg::kIntValue, arg.int_val);
        }
        break;
    case AnnotateValueType::Float64:
        p.dbl(field::arg::kDoubleValue, arg.float_val);
        break;
    }
    p.close(m);
}

void TraceWriter::end(uint32_t thread_id, uint64_t ticks, std::span<TraceArg const> extra) {
    if (_file == nullptr) {
        return;
    }
    Thread &t = thread_state(thread_id);
    if (t.open.empty()) {
        return; // its begin went to a trace closed since, or to none
    }
    std::vector<TraceArg> const args = std::move(t.open.back());
    t.open.pop_back();

    size_t const                                          pkt = thread_packet(t, steady_ns(ticks));
    Proto                                                 p(_buffer);
    std::vector<std::pair<uint32_t, std::string const *>> new_names;
    size_t const                                          event = p.open(field::packet::kTrackEvent);
    p.u(field::event::kType, field::event::kSliceEnd);
    for (auto const &arg : args) {
        write_arg(t.seq, arg, new_names);
    }
    for (auto const &arg : extra) {
        write_arg(t.seq, arg, new_names);
    }
    p.close(event);
    if (!new_names.empty()) {
        size_t const interned = p.open(field::packet::kInternedData);
        for (auto const &[key_id, name] : new_names) {
            size_t const m = p.open(field::interned::kArgNames);
            p.u(1, uint64_t{key_id} + 1);
            p.str(2, *name);
            p.close(m);
        }
        p.close(interned);
    }
    p.close(pkt);
    ++_zones;
}

void TraceWriter::end_lost(uint32_t thread_id, uint64_t ticks) {
    if (_file == nullptr) {
        return;
    }
    Thread &t = thread_state(thread_id);
    if (t.open.empty()) {
        return;
    }
    t.open.pop_back();
    size_t const pkt = thread_packet(t, steady_ns(ticks));
    Proto        p(_buffer);
    size_t const event = p.open(field::packet::kTrackEvent);
    p.u(field::event::kType, field::event::kSliceEnd);
    size_t const m = p.open(field::event::kDebugAnnotations);
    p.str(field::arg::kName, "end_lost");
    p.u(field::arg::kBoolValue, 1);
    p.close(m);
    p.close(event);
    p.close(pkt);
    ++_zones;
}

namespace {

/// A submission's flow id, distinct from other processes' in a merged trace.
auto flow_id(uint64_t pid, uint64_t token) -> uint64_t {
    return (pid << 40) ^ token;
}

} // namespace

void TraceWriter::submit(uint32_t thread_id, uint64_t ticks, uint64_t token) {
    if (_file == nullptr || token == 0) {
        return;
    }
    Thread &t = thread_state(thread_id);
    if (!t.described) {
        describe_thread(thread_id, t);
    }
    size_t const pkt = thread_packet(t, steady_ns(ticks));
    Proto        p(_buffer);
    size_t const event = p.open(field::packet::kTrackEvent);
    p.u(field::event::kType, field::event::kInstant);
    p.str(field::event::kName, "submit");
    p.fixed64(field::event::kFlowIds, flow_id(_pid, token));
    p.close(event);
    p.close(pkt);
}

void TraceWriter::device_span(uint32_t track_id, uint32_t name_id, int64_t start_ns, int64_t end_ns, uint64_t token) {
    if (_file == nullptr) {
        return;
    }
    Proto          p(_buffer);
    uint64_t const track = uuid(_pid, Kind::Device, track_id);
    if (_device_tracks.insert(track_id).second) {
        size_t const pkt  = process_packet(static_cast<uint64_t>(start_ns), false);
        size_t const desc = p.open(field::packet::kTrackDescriptor);
        p.u(field::track::kUuid, track);
        p.u(field::track::kParentUuid, uuid(_pid, Kind::Process, 0));
        p.str(field::track::kName, _strings.get(track_id));
        p.close(desc);
        p.close(pkt);
    }
    uint64_t const name_iid = uint64_t{name_id} + 1;
    bool const     new_name = _process.names.insert(name_iid).second;

    size_t const begin = process_packet(static_cast<uint64_t>(start_ns), true);
    size_t       event = p.open(field::packet::kTrackEvent);
    p.u(field::event::kType, field::event::kSliceBegin);
    p.u(field::event::kTrackUuid, track);
    p.u(field::event::kNameIid, name_iid);
    if (token != 0) {
        p.fixed64(field::event::kTerminatingFlowIds, flow_id(_pid, token));
    }
    p.close(event);
    if (new_name) {
        size_t const interned = p.open(field::packet::kInternedData);
        size_t const m        = p.open(field::interned::kEventNames);
        p.u(1, name_iid);
        p.str(2, _strings.get(name_id));
        p.close(m);
        p.close(interned);
    }
    p.close(begin);

    size_t const end = process_packet(static_cast<uint64_t>(std::max(start_ns, end_ns)), false);
    event            = p.open(field::packet::kTrackEvent);
    p.u(field::event::kType, field::event::kSliceEnd);
    p.u(field::event::kTrackUuid, track);
    p.close(event);
    p.close(end);
}

void TraceWriter::live_bytes(uint64_t ticks, int64_t bytes) {
    if (_file == nullptr) {
        return;
    }
    Proto          p(_buffer);
    uint64_t const ns    = steady_ns(ticks);
    uint64_t const track = uuid(_pid, Kind::Memory, 0);
    if (!_memory_described) {
        size_t const pkt  = process_packet(ns, false);
        size_t const desc = p.open(field::packet::kTrackDescriptor);
        p.u(field::track::kUuid, track);
        p.u(field::track::kParentUuid, uuid(_pid, Kind::Process, 0));
        p.str(field::track::kName, "live bytes");
        size_t const counter = p.open(field::track::kCounter);
        p.u(3, 3); // unit: UNIT_SIZE_BYTES
        p.close(counter);
        p.close(desc);
        p.close(pkt);
        _memory_described = true;
    }
    size_t const pkt   = process_packet(ns, false);
    size_t const event = p.open(field::packet::kTrackEvent);
    p.u(field::event::kType, field::event::kCounter);
    p.u(field::event::kTrackUuid, track);
    p.i(field::event::kCounterValue, bytes);
    p.close(event);
    p.close(pkt);
}

void TraceWriter::energy(uint32_t thread_id, uint64_t ticks, uint64_t total_nj) {
    if (_file == nullptr) {
        return;
    }
    Thread &t = thread_state(thread_id);
    if (!t.described) {
        describe_thread(thread_id, t);
    }
    Proto          p(_buffer);
    uint64_t const ns    = steady_ns(ticks);
    uint64_t const track = uuid(_pid, Kind::Energy, thread_id);
    if (!t.energy_described) {
        size_t const pkt  = process_packet(ns, false);
        size_t const desc = p.open(field::packet::kTrackDescriptor);
        p.u(field::track::kUuid, track);
        p.u(field::track::kParentUuid, t.track);
        p.str(field::track::kName, "energy");
        size_t const counter = p.open(field::track::kCounter);
        p.str(6, "mJ"); // unit_name
        p.close(counter);
        p.close(desc);
        p.close(pkt);
        t.energy_described = true;
    }
    size_t const pkt   = process_packet(ns, false);
    size_t const event = p.open(field::packet::kTrackEvent);
    p.u(field::event::kType, field::event::kCounter);
    p.u(field::event::kTrackUuid, track);
    p.dbl(field::event::kDoubleCounterValue, static_cast<double>(total_nj) / 1e6);
    p.close(event);
    p.close(pkt);
}

WAGGLE_NAMESPACE_END
