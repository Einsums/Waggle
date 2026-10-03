//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Consumer.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Clock.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

WAGGLE_NAMESPACE_BEGIN

Consumer::Consumer(StringTable &strings, SiteTable const &sites) : _strings(strings), _sites(sites), _other_id(strings.intern("(other)")) {
    _running.store(true, std::memory_order_relaxed);
    _thread = std::thread([this] { consumer_loop(); });
}

Consumer::~Consumer() {
    shutdown();
}

auto Consumer::memory_samples(uint64_t after) const -> std::vector<MemorySample> {
    std::vector<MemorySample> out;
    // Oldest first: once the ring has wrapped, the oldest sample is the one about to be overwritten.
    size_t const start = _memory.size() < kMaxMemorySamples ? 0 : _memory_next;
    for (size_t k = 0; k < _memory.size(); ++k) {
        auto const &sample = _memory[(start + k) % _memory.size()];
        if (sample.seq > after) {
            out.push_back(sample);
        }
    }
    return out;
}

auto Consumer::live_allocations(size_t count) const -> std::vector<LiveAllocation> {
    std::vector<std::pair<uint64_t, LiveRecord const *>> all;
    all.reserve(_live.size());
    for (auto const &[address, rec] : _live) {
        all.emplace_back(address, &rec);
    }
    size_t const n = std::min(count, all.size());
    std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n), all.end(),
                      [](auto const &a, auto const &b) { return a.second->bytes > b.second->bytes; });
    std::vector<LiveAllocation> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto const &[address, rec] = all[i];
        out.push_back({.address   = address,
                       .bytes     = rec->bytes,
                       .t_ms      = rec->t_ms,
                       .thread_id = rec->thread_id,
                       .zone      = rec->name_id != 0 ? _strings.get(rec->name_id) : std::string{}});
    }
    return out;
}

auto Consumer::timeline_events() const -> std::vector<TimelineEvent> {
    std::vector<TimelineEvent> out;
    out.reserve(_timeline.size() + _device_timeline.size());
    // Oldest first: once the ring has wrapped, the oldest record is the one about to be overwritten.
    size_t const start = _timeline.size() < kMaxTimelineEvents ? 0 : _timeline_next;
    for (size_t k = 0; k < _timeline.size(); ++k) {
        auto const &rec = _timeline[(start + k) % _timeline.size()];
        out.push_back({.thread_id = rec.thread_id, .name = _strings.get(rec.name_id), .start_ms = rec.start_ms, .end_ms = rec.end_ms});
    }
    size_t const device_start = _device_timeline.size() < kMaxTimelineEvents ? 0 : _device_timeline_next;
    for (size_t k = 0; k < _device_timeline.size(); ++k) {
        auto const &rec = _device_timeline[(device_start + k) % _device_timeline.size()];
        out.push_back({.thread_id = 0,
                       .name      = _strings.get(rec.name_id),
                       .start_ms  = rec.start_ms,
                       .end_ms    = rec.end_ms,
                       .track     = _strings.get(rec.track)});
    }
    return out;
}

auto Consumer::device_work() const -> std::vector<DeviceWork> {
    std::vector<DeviceWork> out;
    out.reserve(_device_stats.size());
    for (auto const &[key, stats] : _device_stats) {
        uint32_t submitter = 0;
        uint64_t most      = 0;
        for (auto const &[name_id, n] : stats.submitters) {
            if (n > most) {
                submitter = name_id;
                most      = n;
            }
        }
        out.push_back({.track     = _strings.get(key.first),
                       .name      = _strings.get(key.second),
                       .count     = stats.count,
                       .total_ms  = stats.total_ms,
                       .min_ms    = stats.min_ms,
                       .max_ms    = stats.max_ms,
                       .submitter = submitter != 0 ? _strings.get(submitter) : std::string{}});
    }
    return out;
}

void Consumer::process_device_span(Event const &evt, uint32_t submitter) {
    using ms         = std::chrono::duration<double, std::milli>;
    auto const at_ms = [&](int64_t ns) {
        return std::chrono::duration_cast<ms>(TimePoint(std::chrono::nanoseconds(ns)) - _program_start).count();
    };
    Site const     site    = _sites.get(evt.site_id);
    uint32_t const name_id = evt.name_id != 0 ? evt.name_id : site.name_id;
    double const   start   = at_ms(evt.device.start_ns);
    double const   end     = std::max(start, at_ms(evt.device.end_ns));

    DeviceRecord const rec{.track = evt.device.track, .name_id = name_id, .start_ms = start, .end_ms = end};
    if (_device_timeline.size() < kMaxTimelineEvents) {
        _device_timeline.push_back(rec);
    } else {
        _device_timeline[_device_timeline_next] = rec;
    }
    _device_timeline_next = (_device_timeline_next + 1) % kMaxTimelineEvents;

    auto        &stats    = _device_stats[{evt.device.track, name_id}];
    double const duration = end - start;
    stats.min_ms          = stats.count == 0 ? duration : std::min(stats.min_ms, duration);
    stats.max_ms          = std::max(stats.max_ms, duration);
    stats.total_ms += duration;
    ++stats.count;
    if (submitter != 0) {
        ++stats.submitters[submitter];
    }
    ++_device_seq;
}

void Consumer::resolve_device_spans() {
    std::erase_if(_pending_spans, [&](PendingSpan &pending) {
        if (auto it = _submissions.find(pending.event.device.token); it != _submissions.end()) {
            process_device_span(pending.event, it->second);
            _submissions.erase(it);
            return true;
        }
        // A submission is written before its work can finish, so a whole pass later it has been
        // read, or was dropped.
        if (++pending.passes > 1) {
            process_device_span(pending.event, 0);
            return true;
        }
        return false;
    });
}

void Consumer::register_thread(uint32_t thread_id, std::shared_ptr<EventRingBuffer> rb) {
    std::scoped_lock const lock(_reg_mutex);
    _registrations.push_back({.thread_id = thread_id, .ring_buffer = std::move(rb)});
}

auto Consumer::dropped_count() const -> uint64_t {
    std::scoped_lock const lock(_reg_mutex);
    uint64_t               total = 0;
    for (auto const &reg : _registrations) {
        total += reg.ring_buffer->refused();
    }
    return total;
}

// A name is kept apart from the thread's tree until the thread records into one: a thread that
// only reports device work, a completion callback's, gets no empty tree in snapshots and viewers.
void Consumer::set_thread_name(uint32_t thread_id, std::string name) {
    std::unique_lock const lock(_tree_mutex);
    if (auto it = _threads.find(thread_id); it != _threads.end()) {
        it->second.name = name;
    }
    _thread_names[thread_id] = std::move(name);
}

void Consumer::name_thread_if_unnamed(uint32_t thread_id, std::string name) {
    std::unique_lock const lock(_tree_mutex);
    if (auto it = _thread_names.find(thread_id); it != _thread_names.end() && !it->second.empty()) {
        return;
    }
    if (auto it = _threads.find(thread_id); it != _threads.end()) {
        it->second.name = name;
    }
    _thread_names[thread_id] = std::move(name);
}

void Consumer::shutdown() {
    if (!_running.exchange(false, std::memory_order_acq_rel))
        return;            // already shut down
    _wake_cv.notify_one(); // Wake consumer thread immediately
    if (_thread.joinable())
        _thread.join();
    // Final drain under exclusive lock
    drain_all();
}

void Consumer::flush() {
    _wake_cv.notify_one(); // Wake consumer thread to drain immediately
    drain_all();
}

namespace {

/// Clear what @p node has recorded, keeping where it was entered and its children.
void clear_statistics(AggNode &node) {
    node.call_count           = 0;
    node.total_exclusive      = ns{0};
    node.total_exclusive_mean = 0.0;
    node.total_exclusive_M2   = 0.0;
    node.exclusive_min        = ns{std::numeric_limits<int64_t>::max()};
    node.exclusive_max        = ns{0};
    node.counters_total.clear();
    node.counters_min.clear();
    node.counters_max.clear();
    node.annotations.clear();
    node.numeric_annotations.clear();
    node.mem_alloc_count   = 0;
    node.mem_free_count    = 0;
    node.mem_alloc_bytes   = 0;
    node.mem_free_bytes    = 0;
    node.mem_current_bytes = 0;
    node.mem_peak_bytes    = 0;
    std::ranges::fill(node.histogram, 0);
    node.folded_names.clear();
}

} // namespace

void Consumer::reset() {
    flush();
    std::unique_lock const lock(_tree_mutex);

    // The nodes open zones accumulate into, which must survive: their frames point at them.
    std::unordered_set<AggNode const *> open;
    for (auto const &[id, ts] : _threads) {
        for (auto const &frame : ts.stack) {
            open.insert(frame.node);
        }
    }

    _reset_ticks        = TickClock::now();
    TimePoint const now = TickClock::instance().to_time_point(_reset_ticks);
    for (auto &[id, ts] : _threads) {
        // An explicit stack, as everywhere the tree is walked.
        std::vector<AggNode *> pending{&ts.root};
        while (!pending.empty()) {
            AggNode *node = pending.back();
            pending.pop_back();
            clear_statistics(*node);
            node->children.erase_if([&](auto const &child) { return !open.contains(child.second.get()); });
            for (auto &child : node->children) {
                pending.push_back(child.second.get());
            }
        }
        // Open zones are timed from now. Their counter readings stay those of their push: a
        // reading is taken only on the zone's own thread.
        for (auto &frame : ts.stack) {
            frame.start      = now;
            frame.child_time = ns{0};
        }
    }

    _timeline.clear();
    _timeline_next = 0;
    // The curve starts again; what is still allocated stays allocated.
    _memory.clear();
    _memory_next = 0;
    // Device work is summed afresh too. Submissions and waiting spans stay: their work is still
    // to be reported.
    _device_timeline.clear();
    _device_timeline_next = 0;
    _device_stats.clear();
    ++_device_seq;
}

void Consumer::consumer_loop() {
    auto last_tick = std::chrono::steady_clock::now();
    // The nap doubles, up to kMaxNap, each time a look finds nothing, and resets to kMinNap when one
    // finds events, so an idle process is not woken a thousand times a second. A ring passing half
    // full, flush() and shutdown() wake it early. The cap stays well under the 50 ms some callers
    // wait for the tree without flushing.
    constexpr auto kMinNap = std::chrono::milliseconds(1);
    constexpr auto kMaxNap = std::chrono::milliseconds(10);
    auto           nap     = kMinNap;
    while (_running.load(std::memory_order_relaxed)) {
        nap = drain_all() > 0 ? kMinNap : std::min(2 * nap, kMaxNap);
        // Tick every ~500 ms: copy the callback under its lock, call it outside.
        auto now = std::chrono::steady_clock::now();
        if ((now - last_tick) >= std::chrono::milliseconds(500)) {
            std::function<void()> tick;
            {
                std::scoped_lock const lock(_tick_mutex);
                tick = _tick_callback;
            }
            last_tick = now;
            if (tick) {
                tick();
            }
        }
        // Nap until notified (by a filling ring, flush() or shutdown()) or the nap ends.
        std::unique_lock lock(_wake_mutex);
        _wake_cv.wait_for(lock, nap);
    }
}

size_t Consumer::drain_all() {
    // Return early if every ring is empty, so an idle process skips the snapshot and the tree lock.
    std::vector<ThreadRegistration> regs;
    {
        std::scoped_lock const lock(_reg_mutex);
        if (std::ranges::all_of(_registrations, [](ThreadRegistration const &r) { return r.ring_buffer->empty(); })) {
            return 0;
        }
        regs = _registrations;
    }

    if (regs.empty())
        return 0;

    // Drain events from all ring buffers under a single exclusive lock
    std::unique_lock const lock(_tree_mutex);
    size_t                 drained = 0;
    for (auto &reg : regs) {
        drained += reg.ring_buffer->drain([&](Event const &evt) { process_event(reg.thread_id, evt); });
    }
    resolve_device_spans();
    return drained;
}

void Consumer::process_event(uint32_t thread_id, Event const &evt) {
    // Device work belongs to no thread's tree: the thread recording it, a completion callback's
    // typically, gets no entry for it.
    if (evt.type == EventType::DeviceSpan) {
        if (evt.device.token == 0) {
            process_device_span(evt, 0);
        } else if (auto it = _submissions.find(evt.device.token); it != _submissions.end()) {
            process_device_span(evt, it->second);
            _submissions.erase(it);
        } else {
            _pending_spans.push_back({.event = evt, .passes = 0});
        }
        return;
    }

    auto [entry, created] = _threads.try_emplace(thread_id);
    auto &ts              = entry->second;
    if (created) {
        if (auto name = _thread_names.find(thread_id); name != _thread_names.end()) {
            ts.name = name->second;
        }
    }

    switch (evt.type) {
    case EventType::Push:
        process_push(ts, evt);
        break;
    case EventType::Pop:
        process_pop(ts, evt, thread_id);
        break;
    case EventType::Annotate:
        process_annotate(ts, evt);
        break;
    case EventType::SetThreadName:
        // Handled via set_thread_name() API, not through ring buffer events
        break;
    case EventType::MemAlloc:
    case EventType::MemFree:
        process_mem(ts, evt, thread_id);
        break;
    case EventType::DeviceSubmit:
        if (_submissions.size() >= kMaxPendingSubmissions) {
            _submissions.clear(); // spans that never came; theirs would be named after nothing anyway
        }
        _submissions[evt.device.token] = ts.stack.empty() ? 0 : ts.stack.back().name_id;
        break;
    case EventType::DeviceSpan:
        break; // handled above
    case EventType::Zone: {
        // Its Push and its Pop, as they would have arrived.
        Event push{};
        push.ticks   = evt.ticks;
        push.type    = EventType::Push;
        push.site_id = evt.site_id;
        push.name_id = evt.name_id;
        push.depth   = evt.depth;
        process_push(ts, push);
        Event pop{};
        pop.ticks = evt.zone.end_ticks;
        pop.type  = EventType::Pop;
        pop.depth = evt.depth;
        process_pop(ts, pop, thread_id);
        break;
    }
    }
}

void Consumer::unwind_stale_frames(ThreadState &ts, size_t depth) {
    if (ts.stack.size() <= depth) {
        return;
    }
    _unmatched_zones.fetch_add(ts.stack.size() - depth, std::memory_order_relaxed);
    // No time is recorded for these zones or charged to their parents: it would be invented.
    ts.stack.resize(depth);
}

void Consumer::process_push(ThreadState &ts, Event const &evt) {
    // Anything open at or below the level this zone opens at lost its Pop.
    if (evt.depth > 0) {
        unwind_stale_frames(ts, evt.depth - 1);
    }

    // The site gives the file, line and function; the name is the event's own when it has one.
    Site const              site    = _sites.get(evt.site_id);
    uint32_t const          name_id = evt.name_id != 0 ? evt.name_id : site.name_id;
    ThreadState::StackFrame frame{};
    frame.name_id    = name_id;
    frame.child_time = ns{0};
    frame.start      = TickClock::instance().to_time_point(std::max(evt.ticks, _reset_ticks));
    for (int i = 0; i < kNumCounterSlots; ++i)
        frame.counters[i] = evt.counters[i];
    frame.has_counters = evt.has_counters;

    // One step down from the parent's node, resolved when the parent was pushed.
    AggNode *parent = ts.stack.empty() ? &ts.root : ts.stack.back().node;
    auto     it     = parent->children.find(name_id);
    if (it == parent->children.end()) {
        // Names built at run time could grow the tree without bound. Past the cap a new name joins
        // the parent's "(other)" node, which still times it.
        std::int64_t const cap   = _max_distinct_children.load(std::memory_order_relaxed);
        auto               other = parent->children.find(_other_id);
        size_t const       named = parent->children.size() - (other != parent->children.end() ? 1 : 0);
        if (cap > 0 && named >= static_cast<size_t>(cap)) {
            if (other == parent->children.end()) {
                auto node      = std::make_unique<AggNode>(_strings.get(_other_id));
                node->file     = _strings.get(site.file_id);
                node->line     = site.line;
                node->function = _strings.get(site.func_id);
                node->domain   = site.domain;

                parent->children[_other_id] = std::move(node);
                other                       = parent->children.find(_other_id);
            }
            AggNode &folded = *other->second;
            if (folded.folded_names.insert(name_id).second) {
                folded.annotations["distinct"] = std::to_string(folded.folded_names.size());
            }
            it = other;
        }
    }
    if (it == parent->children.end()) {
        // The only place ids are resolved to strings: once per distinct call path, not per event.
        auto node      = std::make_unique<AggNode>(_strings.get(name_id));
        node->file     = _strings.get(site.file_id);
        node->line     = site.line;
        node->function = _strings.get(site.func_id);
        node->domain   = site.domain;

        parent->children[name_id] = std::move(node);
        it                        = parent->children.find(name_id);
    }
    frame.node = it->second.get();

    ts.stack.push_back(frame);
}

auto inclusive_time(AggNode const &node) -> ns {
    ns                           total{0};
    std::vector<AggNode const *> pending{&node};
    while (!pending.empty()) {
        AggNode const *n = pending.back();
        pending.pop_back();
        total += n->total_exclusive;
        for (auto const &child : n->children) {
            pending.push_back(child.second.get());
        }
    }
    return total;
}

void AggNode::record_exclusive(ns exclusive) {
    call_count += 1;
    total_exclusive += exclusive;

    // Welford's online variance, in double (see the field declarations).
    double const sample = static_cast<double>(exclusive.count());
    double const delta  = sample - total_exclusive_mean;
    total_exclusive_mean += delta / static_cast<double>(call_count);
    double const delta2 = sample - total_exclusive_mean;
    total_exclusive_M2 += delta * delta2;

    if (exclusive < exclusive_min)
        exclusive_min = exclusive;
    if (exclusive > exclusive_max)
        exclusive_max = exclusive;

    // Per-call log2 histogram (buckets in microseconds: bucket i = [2^i, 2^(i+1)) us)
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(exclusive).count();
    if (us < 1)
        us = 1; // clamp to 1us minimum
    int  bucket = 0;
    auto v      = us;
    while (v > 1 && bucket < AggNode::kHistogramBuckets - 1) {
        v >>= 1;
        ++bucket;
    }
    histogram[bucket]++;
}

void Consumer::process_pop(ThreadState &ts, Event const &evt, uint32_t thread_id) {
    // Anything open below the level being closed lost its Pop. The common case: a burst's pops
    // land after the burst has overrun the ring.
    if (evt.depth > 0) {
        unwind_stale_frames(ts, evt.depth);
    }

    if (ts.stack.empty())
        return;

    auto frame = ts.stack.back();
    ts.stack.pop_back();

    TimePoint const end       = TickClock::instance().to_time_point(evt.ticks);
    ns const        duration  = std::chrono::duration_cast<ns>(end - frame.start);
    ns const        exclusive = duration - frame.child_time;

    // The node was resolved at push.
    AggNode *cur = frame.node;
    if (cur == nullptr) {
        return; // push was never processed for this frame
    }

    cur->record_exclusive(exclusive);

    // Counter deltas only between readings taken: a thread that does not count writes none.
    bool const counted = frame.has_counters && evt.has_counters;
    if (counted && !_counters_checked) {
        auto &backend = get_counter_backend();
        for (int i = 0; i < kNumCounterSlots; ++i) {
            _counter_names[i] = backend.slot_name(i);
        }
        _counters_checked = true;
    }
    for (int i = 0; counted && i < kNumCounterSlots; ++i) {
        if (_counter_names[i].empty()) {
            continue; // a slot the backend leaves unused
        }
        uint64_t const     counter_delta = evt.counters[i] - frame.counters[i];
        std::string const &cname         = _counter_names[i];
        cur->counters_total[cname] += counter_delta;
        auto itmin = cur->counters_min.find(cname);
        if (itmin == cur->counters_min.end()) {
            cur->counters_min[cname] = counter_delta;
            cur->counters_max[cname] = counter_delta;
        } else {
            if (counter_delta < cur->counters_min[cname])
                cur->counters_min[cname] = counter_delta;
            if (counter_delta > cur->counters_max[cname])
                cur->counters_max[cname] = counter_delta;
        }
    }

    // Record timeline event for Gantt chart
    {
        using ms = std::chrono::duration<double, std::milli>;
        TimelineRecord const rec{.thread_id = thread_id,
                                 .name_id   = frame.name_id,
                                 .start_ms  = std::chrono::duration_cast<ms>(frame.start - _program_start).count(),
                                 .end_ms    = std::chrono::duration_cast<ms>(end - _program_start).count()};
        if (_timeline.size() < kMaxTimelineEvents) {
            _timeline.push_back(rec);
        } else {
            _timeline[_timeline_next] = rec;
        }
        _timeline_next = (_timeline_next + 1) % kMaxTimelineEvents;
    }

    // Add duration to parent's child_time
    if (!ts.stack.empty())
        ts.stack.back().child_time += duration;
}

void Consumer::process_annotate(ThreadState &ts, Event const &evt) {
    if (ts.stack.empty())
        return;

    // The innermost frame already knows its node; no walk, no string lookups.
    AggNode *cur = ts.stack.back().node;
    if (cur == nullptr)
        return; // push was never processed for this frame

    std::string const &key = _strings.get(evt.annotation.key_id);

    switch (evt.annotation.value_type) {
    case AnnotateValueType::String: {
        cur->annotations[key] = _strings.get(evt.annotation.string_id);
        break;
    }
    case AnnotateValueType::Int64: {
        cur->annotations[key] = std::to_string(evt.annotation.int_val);
        auto &na              = cur->numeric_annotations[key];
        na.total += static_cast<double>(evt.annotation.int_val);
        na.count += 1;
        auto val = static_cast<double>(evt.annotation.int_val);
        if (val < na.min_val)
            na.min_val = val;
        if (val > na.max_val)
            na.max_val = val;
        break;
    }
    case AnnotateValueType::Float64: {
        cur->annotations[key] = std::to_string(evt.annotation.float_val);
        auto &na              = cur->numeric_annotations[key];
        na.total += evt.annotation.float_val;
        na.count += 1;
        if (evt.annotation.float_val < na.min_val)
            na.min_val = evt.annotation.float_val;
        if (evt.annotation.float_val > na.max_val)
            na.max_val = evt.annotation.float_val;
        break;
    }
    }
}

void Consumer::process_mem(ThreadState &ts, Event const &evt, uint32_t thread_id) {
    bool const alloc = evt.type == EventType::MemAlloc;

    // The allocation track: every allocation and free, in a zone or not.
    double const t_ms = std::chrono::duration<double, std::milli>(TickClock::instance().to_time_point(evt.ticks) - _program_start).count();
    _live_bytes += alloc ? evt.mem.bytes : -evt.mem.bytes;
    MemorySample const sample{.seq = ++_memory_seq, .t_ms = t_ms, .live_bytes = _live_bytes};
    if (_memory.size() < kMaxMemorySamples) {
        _memory.push_back(sample);
    } else {
        _memory[_memory_next] = sample;
    }
    _memory_next = (_memory_next + 1) % kMaxMemorySamples;
    // Only an allocation with an address can be matched to its free.
    if (alloc) {
        if (evt.mem.address != 0 && _live.size() < kMaxLiveAllocations) {
            uint32_t const name_id = !ts.stack.empty() ? ts.stack.back().name_id : 0;
            _live[evt.mem.address] = {.bytes = evt.mem.bytes, .t_ms = t_ms, .thread_id = thread_id, .name_id = name_id};
        } else {
            ++_untracked_allocations;
        }
    } else if (evt.mem.address != 0) {
        _live.erase(evt.mem.address);
    }

    if (ts.stack.empty())
        return;

    // The innermost frame already knows its node; no walk, no string lookups.
    AggNode *cur = ts.stack.back().node;
    if (cur == nullptr)
        return; // push was never processed for this frame

    if (evt.type == EventType::MemAlloc) {
        cur->mem_alloc_count += 1;
        cur->mem_alloc_bytes += evt.mem.bytes;
        cur->mem_current_bytes += evt.mem.bytes;
        if (cur->mem_current_bytes > cur->mem_peak_bytes)
            cur->mem_peak_bytes = cur->mem_current_bytes;
    } else {
        cur->mem_free_count += 1;
        cur->mem_free_bytes += evt.mem.bytes;
        cur->mem_current_bytes -= evt.mem.bytes;
    }
}

WAGGLE_NAMESPACE_END
