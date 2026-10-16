//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Consumer.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Clock.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "Detail/JsonEscape.hpp"
#include "Energy.hpp"
#include "Numa.hpp"
#include "Process.hpp"

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

    _trace.device_span(evt.device.track, name_id, evt.device.start_ns, evt.device.end_ns, evt.device.token);

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

void Consumer::register_thread(uint32_t thread_id, std::shared_ptr<EventRingBuffer> rb, uint64_t kernel_thread) {
    std::scoped_lock const lock(_reg_mutex);
    _registrations.push_back({.thread_id = thread_id, .ring_buffer = std::move(rb), .kernel_thread = kernel_thread});
}

void Consumer::sample_energy(ThreadRegistration const &reg) {
    ThreadEnergy now;
    if (!read_thread_energy(reg.kernel_thread, now)) {
        return;
    }
    _energy_reads.fetch_add(1, std::memory_order_relaxed);
    auto &thread = _energy[reg.thread_id];
    // The first reading is the baseline: energy before the source was on is nobody's.
    // A sample waits for the thread's next event, which says what was open when it was read: a
    // zone with nothing inside is written only when it closes. A thread that waits uses no energy,
    // so it makes no samples; one zone running for seconds makes many, all its own, so past a
    // bound they are merged.
    if (thread.read && now.total_nj > thread.total_nj) {
        uint64_t const ticks = TickClock::now();
        _trace.energy(reg.thread_id, ticks, now.total_nj);
        EnergySample const sample{.ticks     = ticks,
                                  .total_nj  = now.total_nj - thread.total_nj,
                                  .e_core_nj = now.e_core_nj >= thread.e_core_nj ? now.e_core_nj - thread.e_core_nj : 0};
        if (thread.pending.size() < kMaxPendingEnergySamples) {
            thread.pending.push_back(sample);
        } else {
            auto &last = thread.pending.back();
            last.ticks = sample.ticks;
            last.total_nj += sample.total_nj;
            last.e_core_nj += sample.e_core_nj;
        }
    }
    thread.total_nj  = now.total_nj;
    thread.e_core_nj = now.e_core_nj;
    thread.read      = true;
}

void Consumer::credit_energy(uint32_t thread_id, ThreadState &ts, uint64_t ticks) {
    auto it = _energy.find(thread_id);
    if (it == _energy.end()) {
        return;
    }
    auto &pending = it->second.pending;
    while (!pending.empty() && pending.front().ticks < ticks) {
        AggNode *node = ts.stack.empty() || ts.stack.back().node == nullptr ? &ts.root : ts.stack.back().node;
        node->energy_nj += pending.front().total_nj;
        node->e_core_energy_nj += pending.front().e_core_nj;
        pending.pop_front();
    }
}

void Consumer::set_sampling(bool on, uint64_t rate_hz, bool callchain) {
    bool const was        = _sampling_on.exchange(on, std::memory_order_relaxed);
    bool const was_chains = _sampling_callchain.exchange(callchain, std::memory_order_relaxed);
    if (_sampling_rate.exchange(rate_hz, std::memory_order_relaxed) != rate_hz || was != on || was_chains != callchain) {
        _sampling_settings.fetch_add(1, std::memory_order_relaxed);
    }
    if (on) {
        _wake_cv.notify_one();
    }
}

auto Consumer::sampling_state() const -> SamplingState {
    std::scoped_lock const lock(_sampling_state_mutex);
    SamplingState          state = _sampling_state;
    state.wanted                 = _sampling_on.load(std::memory_order_relaxed);
    return state;
}

void Consumer::add_samples(std::vector<sampling::Sample> const &samples) {
    std::scoped_lock const lock(_added_mutex);
    _added_samples.insert(_added_samples.end(), samples.begin(), samples.end());
    _sampling_busy.store(true, std::memory_order_relaxed);
}

void Consumer::queue_sample(sampling::Sample const &sample) {
    // In time order, which crediting relies on: one thread's ring is in order already, so this is
    // an append unless a test's samples are mixed in.
    auto               &waiting = _waiting_samples[sample.tid];
    WaitingSample const next{
        .ticks = sampling::monotonic_to_ticks(sample.time_ns, _sample_anchor), .ip = sample.ip, .caller = _code.caller_of(sample)};
    auto at = waiting.end();
    while (at != waiting.begin() && std::prev(at)->ticks > next.ticks) {
        --at;
    }
    waiting.insert(at, next);
    ++_samples_taken;
}

void Consumer::run_sampler(size_t registrations) {
    auto const     sink     = [this](sampling::Sample const &sample) { queue_sample(sample); };
    bool const     want     = _sampling_on.load(std::memory_order_relaxed);
    uint64_t const settings = _sampling_settings.load(std::memory_order_relaxed);
    auto const     now      = std::chrono::steady_clock::now();
    _sample_anchor          = sampling::ClockPair::now();
    if (!want) {
        if (_sampler.running()) {
            _sampler.stop(sink);
        }
    } else if (_sampling_tried != settings) {
        // Asked for, or asked for at another rate: start afresh, once for these settings.
        if (_sampler.running()) {
            _sampler.stop(sink);
        }
        _sampling_tried = settings;
        if (_sampler.start(_sampling_rate.load(std::memory_order_relaxed), _sampling_callchain.load(std::memory_order_relaxed))) {
            _sampling_ever           = true;
            _last_rescan             = now;
            _rescanned_registrations = registrations;
        }
    }
    {
        // Samples a test handed over, as if just read.
        std::scoped_lock const added_lock(_added_mutex);
        if (!_added_samples.empty()) {
            std::ranges::stable_sort(_added_samples, {}, &sampling::Sample::time_ns);
            for (auto const &sample : _added_samples) {
                queue_sample(sample);
            }
            _added_samples.clear();
            _sampling_ever = true;
        }
    }
    if (want && _sampler.running()) {
        // A thread that registers is picked up at once; one that never records, within a second.
        if (registrations != _rescanned_registrations || now - _last_rescan >= std::chrono::seconds(1)) {
            _sampler.rescan(sink);
            _last_rescan             = now;
            _rescanned_registrations = registrations;
        }
        _sampler.drain(sink);
    }

    std::scoped_lock const lock(_sampling_state_mutex);
    _sampling_state = {.wanted    = want,
                       .running   = _sampler.running(),
                       .event     = _sampler.event(),
                       .rate_hz   = _sampler.rate(),
                       .callchain = _sampler.callchain(),
                       .threads   = _sampler.threads(),
                       .refused   = _sampler.refused(),
                       .samples   = _samples_taken,
                       .lost      = _sampler.lost(),
                       .why_not   = _sampler.why_not()};
}

void Consumer::credit_samples(std::deque<WaitingSample> &waiting, ThreadState *ts, uint64_t ticks) {
    while (!waiting.empty() && waiting.front().ticks < ticks) {
        AggNode *node = ts == nullptr || ts->stack.empty() ? nullptr : ts->stack.back().node;
        (node != nullptr ? node->samples : _unzoned).add(waiting.front().ip, waiting.front().caller);
        waiting.pop_front();
    }
}

void Consumer::credit_waiting_samples(uint64_t ticks) {
    for (auto it = _waiting_samples.begin(); it != _waiting_samples.end();) {
        ThreadState *ts = nullptr;
        if (auto const thread = _thread_of_kernel.find(it->first); thread != _thread_of_kernel.end()) {
            if (auto const state = _threads.find(thread->second); state != _threads.end()) {
                ts = &state->second;
            }
        }
        credit_samples(it->second, ts, ticks);
        // Only a zone opened before sampling was switched on, whose Push is not written until it
        // closes, keeps samples waiting past the pass: past a bound, they go where they best can.
        while (it->second.size() > kMaxWaitingSamples) {
            AggNode *node = ts == nullptr || ts->stack.empty() ? nullptr : ts->stack.back().node;
            (node != nullptr ? node->samples : _unzoned).add(it->second.front().ip, it->second.front().caller);
            it->second.pop_front();
        }
        it = it->second.empty() ? _waiting_samples.erase(it) : std::next(it);
    }
    _sampling_busy.store(_sampler.running() || !_waiting_samples.empty(), std::memory_order_relaxed);
}

namespace {

void write_samples_json(std::string &out, SampleCounts const &samples) {
    out += fmt::format(R"("samples": {}, "addresses": {{)", samples.total);
    // Largest first, so a reader that wants the top few reads the front.
    std::vector<std::pair<uint64_t, uint64_t>> sorted(samples.by_address.begin(), samples.by_address.end());
    std::ranges::sort(sorted, [](auto const &a, auto const &b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    bool first = true;
    for (auto const &[address, count] : sorted) {
        out += fmt::format(R"({}"0x{:x}": {})", first ? "" : ", ", address, count);
        first = false;
    }
    out += "}";
    if (!samples.by_caller.empty()) {
        std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>> callers(samples.by_caller.begin(), samples.by_caller.end());
        std::ranges::stable_sort(callers, [](auto const &a, auto const &b) { return a.second > b.second; });
        out += R"(, "callers": [)";
        for (size_t i = 0; i < callers.size(); ++i) {
            auto const &[pair, count] = callers[i];
            out += fmt::format(R"({}["0x{:x}", "0x{:x}", {}])", i > 0 ? ", " : "", pair.first, pair.second, count);
        }
        out += "]";
    }
}

} // namespace

auto Consumer::cpu_json(bool always) const -> std::string {
    bool const numa = _numa_passes.load(std::memory_order_relaxed) > 0;
    if (!_sampling_ever && !numa && !always) {
        return {};
    }
    std::string out = fmt::format(R"({{"schema": "waggle.cpu/1", "machine": {})", sampling::machine_json());
    if (numa) {
        out += R"(, "numa": )" + numa_json();
    }
    if (!_sampling_ever) {
        return out + "}";
    }
    auto const state = sampling_state();
    out += fmt::format(R"(, "sampling": {{"event": "{}", "rate_hz": {}, "callchain": {}, "threads": {}, "samples": {}, "lost": {}}}, )"
                       R"("modules": {}, "zones": [)",
                       state.event, state.rate_hz, state.callchain, state.threads, state.samples, state.lost, sampling::module_map_json());
    bool first = true;
    for (auto const &[thread_id, ts] : _threads) {
        // Each zone with samples, named by its path from the thread's root; an explicit stack, as
        // everywhere the tree is walked.
        std::vector<std::pair<AggNode const *, std::string>> pending;
        for (auto const &child : ts.root.children) {
            pending.emplace_back(child.second.get(), "\"" + detail::json_escape(child.second->name) + "\"");
        }
        while (!pending.empty()) {
            auto const [node, path] = pending.back();
            pending.pop_back();
            if (node->samples.total > 0) {
                out += fmt::format(R"({}{{"thread": {}, "path": [{}], )", first ? "" : ", ", thread_id, path);
                write_samples_json(out, node->samples);
                out += "}";
                first = false;
            }
            for (auto const &child : node->children) {
                pending.emplace_back(child.second.get(), path + ", \"" + detail::json_escape(child.second->name) + "\"");
            }
        }
    }
    out += R"(], "no_zone": {)";
    write_samples_json(out, _unzoned);
    out += "}}";
    return out;
}

void Consumer::set_numa(bool on, int64_t min_bytes) {
    _numa_min_bytes.store(min_bytes, std::memory_order_relaxed);
    _numa_on.store(on, std::memory_order_relaxed);
}

auto Consumer::numa_state() const -> NumaState {
    return {.wanted = _numa_on.load(std::memory_order_relaxed),
            .nodes  = numa::topology().nodes,
            .passes = _numa_passes.load(std::memory_order_relaxed),
            .blocks = _numa_blocks_checked.load(std::memory_order_relaxed)};
}

void Consumer::numa_pass() {
    auto const &topo = numa::topology();
    if (!_numa_on.load(std::memory_order_relaxed) || topo.nodes < 2) {
        return;
    }
    std::scoped_lock const pass(_numa_pass_mutex);
    auto const             nodes = static_cast<size_t>(topo.nodes);

    // The blocks to check: the largest live ones recorded with an address.
    struct Candidate {
        uint64_t address;
        int64_t  bytes;
    };
    std::vector<Candidate> candidates;
    {
        std::shared_lock const lock(_tree_mutex);
        int64_t const          least = _numa_min_bytes.load(std::memory_order_relaxed);
        for (auto const &[address, rec] : _live) {
            if (rec.bytes >= least) {
                candidates.push_back({.address = address, .bytes = rec.bytes});
            }
        }
    }
    size_t const kept = std::min(candidates.size(), kMaxNumaBlocks);
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(kept), candidates.end(),
                      [](Candidate const &a, Candidate const &b) { return a.bytes > b.bytes; });
    candidates.resize(kept);

    // The system's answers, without the lock: none of them reads the tree.
    auto const                   tasks = numa::tasks();
    std::vector<numa::Placement> placements;
    placements.reserve(candidates.size());
    for (auto const &c : candidates) {
        placements.push_back(numa::placement(c.address, c.bytes));
    }
    auto const   huge = candidates.empty() ? std::vector<numa::Mapping>{} : numa::huge_mappings();
    double const t_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _program_start).count();

    std::unique_lock const lock(_tree_mutex);
    // Each thread's CPU time since the last pass, on the node of the CPU it is on now; and a zone
    // still open on another node than at the last pass.
    std::vector<double>          busy(nodes, 0.0);
    std::unordered_set<uint64_t> seen;
    for (auto const &task : tasks) {
        seen.insert(task.tid);
        int const node =
            task.cpu >= 0 && static_cast<size_t>(task.cpu) < topo.node_of_cpu.size() ? topo.node_of_cpu[static_cast<size_t>(task.cpu)] : -1;
        AggNode const *zone      = nullptr;
        uint32_t       thread_id = 0;
        if (auto const k = _thread_of_kernel.find(task.tid); k != _thread_of_kernel.end()) {
            thread_id = k->second;
            if (auto const t = _threads.find(thread_id); t != _threads.end() && !t->second.stack.empty()) {
                zone = t->second.stack.back().node;
            }
        }
        uint64_t const calls       = zone != nullptr ? zone->call_count : 0;
        auto [entry, first_time]   = _numa_threads.try_emplace(task.tid);
        NumaThread &thread         = entry->second;
        bool const  same_zone_open = !first_time && zone != nullptr && zone == thread.zone && calls == thread.zone_calls;
        if (!first_time && node >= 0 && task.busy_ticks >= thread.busy_ticks) {
            busy[static_cast<size_t>(node)] += static_cast<double>(task.busy_ticks - thread.busy_ticks) / numa::ticks_per_second();
        }
        if (same_zone_open && node >= 0 && thread.node >= 0 && node != thread.node) {
            ++_numa_migrations[{zone->name, thread_id}];
        }
        thread = {.busy_ticks = task.busy_ticks, .zone = zone, .zone_calls = calls, .node = node};
    }
    std::erase_if(_numa_threads, [&](auto const &entry) { return !seen.contains(entry.first); });
    _numa_busy_s.resize(nodes, 0.0);
    for (size_t n = 0; n < nodes; ++n) {
        _numa_busy_s[n] += busy[n];
    }

    // Blocks: freed since, then this pass's placements, kept when they changed.
    for (auto &[address, block] : _numa_blocks) {
        auto const live = _live.find(address);
        block.freed     = block.freed || live == _live.end() || live->second.bytes != block.bytes;
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto const live = _live.find(candidates[i].address);
        if (live == _live.end() || placements[i].sampled == 0) {
            continue;
        }
        // Every page unmapped: freed without a free being recorded. Gone, not untouched.
        if (placements[i].unmapped == placements[i].sampled) {
            if (auto const known = _numa_blocks.find(candidates[i].address); known != _numa_blocks.end()) {
                known->second.freed = true;
            }
            continue;
        }
        auto [entry, created] = _numa_blocks.try_emplace(candidates[i].address);
        NumaBlock &block      = entry->second;
        if (created || block.freed) {
            block = {.address   = candidates[i].address,
                     .bytes     = live->second.bytes,
                     .t_ms      = live->second.t_ms,
                     .thread_id = live->second.thread_id,
                     .name_id   = live->second.name_id,
                     .busy_s    = std::vector<double>(nodes, 0.0)};
        }
        for (size_t n = 0; n < nodes; ++n) {
            block.busy_s[n] += busy[n];
        }
        auto const &p       = placements[i];
        auto const  changed = [&](NumaPlacement const &last) {
            // More than 2% of the sampled pages moved, or were touched.
            auto const threshold = std::max<uint32_t>(1, p.sampled / 50);
            for (size_t n = 0; n < nodes; ++n) {
                if (std::abs(static_cast<int64_t>(p.pages[n]) - static_cast<int64_t>(last.pages[n])) > threshold) {
                    return true;
                }
            }
            return std::abs(static_cast<int64_t>(p.untouched) - static_cast<int64_t>(last.untouched)) > threshold;
        };
        if (block.placements.empty() || changed(block.placements.back())) {
            block.placements.push_back({.t_ms      = t_ms,
                                        .pages     = p.pages,
                                        .untouched = p.untouched,
                                        .sampled   = p.sampled,
                                        .huge_pct  = numa::huge_percent(huge, candidates[i].address, candidates[i].bytes)});
        }
    }
    // Past the bound, the freed blocks go, oldest first.
    while (_numa_blocks.size() > kMaxKeptNumaBlocks) {
        auto oldest = _numa_blocks.end();
        for (auto it = _numa_blocks.begin(); it != _numa_blocks.end(); ++it) {
            if (it->second.freed && (oldest == _numa_blocks.end() || it->second.t_ms < oldest->second.t_ms)) {
                oldest = it;
            }
        }
        if (oldest == _numa_blocks.end()) {
            break;
        }
        _numa_blocks.erase(oldest);
    }
    _numa_blocks_checked.store(candidates.size(), std::memory_order_relaxed);
    _numa_passes.fetch_add(1, std::memory_order_relaxed);
}

auto Consumer::numa_json() const -> std::string {
    auto const &topo = numa::topology();
    auto const  list = [](auto const &values, auto const &format) {
        std::string out = "[";
        for (size_t i = 0; i < values.size(); ++i) {
            out += (i > 0 ? ", " : "") + format(values[i]);
        }
        return out + "]";
    };
    auto const  seconds = [](double v) { return fmt::format("{:.3f}", v); };
    auto const  count   = [](uint32_t v) { return std::to_string(v); };
    auto const  quoted  = [](std::string const &v) { return "\"" + v + "\""; };
    std::string out     = fmt::format(R"({{"nodes": {}, "node_cpus": {}, "passes": {}, "busy_s_by_node": {}, "blocks": [)", topo.nodes,
                                      list(topo.cpus, quoted), _numa_passes.load(std::memory_order_relaxed), list(_numa_busy_s, seconds));
    bool        first   = true;
    for (auto const &[address, block] : _numa_blocks) {
        out += fmt::format(R"({}{{"zone": "{}", "thread": {}, "address": "0x{:x}", "bytes": {}, "t_ms": {:.3f}, "freed": {}, )"
                           R"("busy_s_by_node": {}, "placements": [)",
                           first ? "" : ", ", detail::json_escape(block.name_id != 0 ? _strings.get(block.name_id) : std::string{}),
                           block.thread_id, address, block.bytes, block.t_ms, block.freed, list(block.busy_s, seconds));
        for (size_t i = 0; i < block.placements.size(); ++i) {
            auto const &p = block.placements[i];
            out += fmt::format(R"({}{{"t_ms": {:.3f}, "pages": {}, "untouched": {}, "sampled": {}, "huge_pct": {:.1f}}})",
                               i > 0 ? ", " : "", p.t_ms, list(p.pages, count), p.untouched, p.sampled, p.huge_pct);
        }
        out += "]}";
        first = false;
    }
    out += R"(], "migrations": [)";
    first = true;
    for (auto const &[key, times] : _numa_migrations) {
        out += fmt::format(R"({}{{"zone": "{}", "thread": {}, "count": {}}})", first ? "" : ", ", detail::json_escape(key.first),
                           key.second, times);
        first = false;
    }
    return out + "]}";
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

auto Consumer::set_trace(std::string const &path, DomainTable const &domains) -> std::string {
    std::unique_lock const lock(_tree_mutex);
    if (path == _trace.path() && _trace.is_open() == !path.empty()) {
        return {};
    }
    if (path.empty()) {
        _trace.close();
        return {};
    }
    return _trace.open(path, domains);
}

void Consumer::shutdown() {
    if (!_running.exchange(false, std::memory_order_acq_rel))
        return;            // already shut down
    _wake_cv.notify_one(); // Wake consumer thread immediately
    if (_thread.joinable())
        _thread.join();
    // Final drain under exclusive lock
    drain_all();
    // Energy still waiting for an event that will not come goes to the zone open at the end.
    std::unique_lock const lock(_tree_mutex);
    for (auto &[thread_id, thread] : _energy) {
        if (auto ts = _threads.find(thread_id); ts != _threads.end()) {
            credit_energy(thread_id, ts->second, std::numeric_limits<uint64_t>::max());
        }
    }
    // So are samples, once the rings are read a last time.
    if (_sampler.running()) {
        _sample_anchor = sampling::ClockPair::now();
        _sampler.stop([this](sampling::Sample const &sample) { queue_sample(sample); });
    }
    credit_waiting_samples(std::numeric_limits<uint64_t>::max());
    _trace.close();
}

void Consumer::flush() {
    _wake_cv.notify_one(); // Wake consumer thread to drain immediately
    drain_all();
}

namespace {

/// Clear what @p node has recorded, keeping where it was entered and its children.
void clear_statistics(AggNode &node) {
    node.energy_nj        = 0;
    node.e_core_energy_nj = 0;
    node.samples.clear();
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
    // Energy is credited afresh; the readings stay as the baselines.
    for (auto &[id, thread] : _energy) {
        thread.pending.clear();
    }
    // So are samples; the sampler keeps sampling.
    _waiting_samples.clear();
    _unzoned.clear();
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
    {
        // The sampling source samples the program's threads, not this one.
        std::unique_lock const lock(_tree_mutex);
        _consumer_tid = kernel_thread_id();
        _sampler.exclude(static_cast<uint32_t>(_consumer_tid));
    }
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
            // The numa source's pass, as often: placement moves slowly, and a pass costs a read of
            // every thread's stat and of the process's smaps.
            numa_pass();
        }
        // Nap until notified (by a filling ring, flush() or shutdown()) or the nap ends.
        std::unique_lock lock(_wake_mutex);
        _wake_cv.wait_for(lock, nap);
    }
}

size_t Consumer::drain_all() {
    // Return early if every ring is empty, so an idle process skips the snapshot and the tree lock.
    bool const energy   = _energy_on.load(std::memory_order_relaxed);
    bool const sampling = _sampling_on.load(std::memory_order_relaxed) || _sampling_busy.load(std::memory_order_relaxed);
    // Samples taken before this instant are credited by the pass's end: their threads' events up to
    // it are in the rings once the samples are read. Read before the registrations, so a thread
    // that registers after them registered after it too, and had no zone open at such a sample.
    uint64_t const                  pass_ticks = TickClock::now();
    std::vector<ThreadRegistration> regs;
    {
        std::scoped_lock const lock(_reg_mutex);
        // Sampling energy, and threads, goes on while threads record nothing: a long zone with
        // nothing inside it uses energy and takes samples without writing an event until it closes.
        if (!energy && !sampling &&
            std::ranges::all_of(_registrations, [](ThreadRegistration const &r) { return r.ring_buffer->empty(); })) {
            return 0;
        }
        regs = _registrations;
    }

    if (regs.empty() && !sampling)
        return 0;

    // Drain events from all ring buffers under a single exclusive lock
    std::unique_lock const lock(_tree_mutex);
    size_t                 drained = 0;
    // Samples are read before events, so each finds its thread's events up to it in the ring.
    if (sampling) {
        run_sampler(regs.size());
    }
    for (auto &reg : regs) {
        // Which recording thread a kernel thread is: for samples, and the numa source's thread nodes.
        if (reg.kernel_thread != 0) {
            _thread_of_kernel[reg.kernel_thread] = reg.thread_id;
        }
        if (!_waiting_samples.empty()) {
            auto const waiting = _waiting_samples.find(reg.kernel_thread);
            _draining_samples  = waiting == _waiting_samples.end() ? nullptr : &waiting->second;
        }
        if (_trace.is_open()) {
            auto const name = _thread_names.find(reg.thread_id);
            _trace.thread(reg.thread_id, reg.kernel_thread, name != _thread_names.end() ? name->second : std::string{});
        }
        // Read before draining: every event the thread wrote before the reading is in the ring now,
        // so the zone open at the reading is known once the drain reaches it.
        if (energy) {
            sample_energy(reg);
        }
        drained += reg.ring_buffer->drain([&](Event const &evt) { process_event(reg.thread_id, evt); });
        _draining_samples = nullptr;
    }
    if (sampling) {
        credit_waiting_samples(pass_ticks);
    }
    resolve_device_spans();
    _trace.maybe_flush();
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
    // Energy read, and samples taken, before this event belong to the zone open then.
    if (!_energy.empty()) {
        credit_energy(thread_id, ts, evt.ticks);
    }
    if (_draining_samples != nullptr) {
        credit_samples(*_draining_samples, &ts, evt.ticks);
    }

    switch (evt.type) {
    case EventType::Push:
        process_push(ts, evt, thread_id);
        break;
    case EventType::Pop:
        process_pop(ts, evt, thread_id);
        break;
    case EventType::Annotate:
        process_annotate(ts, evt, thread_id);
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
        _trace.submit(thread_id, evt.ticks, evt.device.token);
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
        process_push(ts, push, thread_id);
        // Energy read while it was open is its own, though it was written only as it closed.
        if (!_energy.empty()) {
            credit_energy(thread_id, ts, evt.zone.end_ticks);
        }
        if (_draining_samples != nullptr) {
            credit_samples(*_draining_samples, &ts, evt.zone.end_ticks);
        }
        Event pop{};
        pop.ticks = evt.zone.end_ticks;
        pop.type  = EventType::Pop;
        pop.depth = evt.depth;
        process_pop(ts, pop, thread_id);
        break;
    }
    }
}

void Consumer::unwind_stale_frames(ThreadState &ts, size_t depth, uint32_t thread_id, uint64_t ticks) {
    if (ts.stack.size() <= depth) {
        return;
    }
    _unmatched_zones.fetch_add(ts.stack.size() - depth, std::memory_order_relaxed);
    for (size_t k = depth; k < ts.stack.size(); ++k) {
        _trace.end_lost(thread_id, ticks);
    }
    // No time is recorded for these zones or charged to their parents: it would be invented.
    ts.stack.resize(depth);
}

void Consumer::process_push(ThreadState &ts, Event const &evt, uint32_t thread_id) {
    // Anything open at or below the level this zone opens at lost its Pop.
    if (evt.depth > 0) {
        unwind_stale_frames(ts, evt.depth - 1, thread_id, evt.ticks);
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
    // The trace has the zone's own name, though the tree may have folded it into "(other)", and
    // its true start, though a reset may have clipped the tree's.
    _trace.begin(thread_id, evt.ticks, evt.site_id, name_id);
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
        unwind_stale_frames(ts, evt.depth, thread_id, evt.ticks);
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
            _counter_names[i]    = backend.slot_name(i);
            _counter_name_ids[i] = _counter_names[i].empty() ? 0 : _strings.intern(_counter_names[i]);
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

    if (_trace.is_open()) {
        std::array<TraceArg, kNumCounterSlots> deltas{};
        size_t                                 n = 0;
        for (int i = 0; counted && i < kNumCounterSlots; ++i) {
            if (!_counter_names[i].empty()) {
                deltas[n++] = {.key_id      = _counter_name_ids[i],
                               .type        = AnnotateValueType::Int64,
                               .is_unsigned = true,
                               .int_val     = static_cast<int64_t>(evt.counters[i] - frame.counters[i])};
            }
        }
        _trace.end(thread_id, evt.ticks, std::span<TraceArg const>(deltas.data(), n));
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

void Consumer::process_annotate(ThreadState &ts, Event const &evt, uint32_t thread_id) {
    if (ts.stack.empty())
        return;
    _trace.annotate(thread_id, {.key_id    = evt.annotation.key_id,
                                .type      = evt.annotation.value_type,
                                .int_val   = evt.annotation.value_type == AnnotateValueType::Int64 ? evt.annotation.int_val : 0,
                                .float_val = evt.annotation.value_type == AnnotateValueType::Float64 ? evt.annotation.float_val : 0.0,
                                .string_id = evt.annotation.value_type == AnnotateValueType::String ? evt.annotation.string_id : 0});

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
    _trace.live_bytes(evt.ticks, _live_bytes);
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
