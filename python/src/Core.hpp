//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

/// @file
/// What ``waggle._core`` binds: the profiler for Python code, over the C interface only. The module
/// links ``libwaggle`` and holds nothing of the collector's (no rings, tables or thread-locals),
/// so a process has one collector however many modules use it.

#include <Waggle/Waggle.h>

#include <apiary/Annotations.hpp>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace waggle_python {

namespace detail {

inline std::uint32_t intern(std::string_view s) {
    return waggle_intern(s.data(), s.size());
}

/// @p s as a JSON string.
inline void json_string(std::string &out, std::string_view s) {
    out += '"';
    for (char const c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8]; // NOLINT(modernize-avoid-c-arrays)
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

inline std::string_view text(char const *(*get)(waggle_node const *, size_t *), waggle_node const *node) {
    size_t      length = 0;
    char const *s      = get(node, &length);
    return {s, length};
}

/// @p node and everything below it as JSON.
inline void node_json(std::string &out, waggle_node const *node) {
    waggle_node_stats stats{};
    stats.size = sizeof(stats);
    waggle_node_stats_get(node, &stats);
    out += "{\"name\":";
    json_string(out, text(waggle_node_name, node));
    out += ",\"file\":";
    json_string(out, text(waggle_node_file, node));
    out += ",\"line\":" + std::to_string(waggle_node_line(node));
    out += ",\"function\":";
    json_string(out, text(waggle_node_function, node));
    out += ",\"domain\":";
    json_string(out, text(waggle_node_domain, node));
    out += ",\"call_count\":" + std::to_string(stats.call_count);
    out += ",\"exclusive_ns\":" + std::to_string(stats.exclusive_ns);
    out += ",\"inclusive_ns\":" + std::to_string(stats.inclusive_ns);
    out += ",\"exclusive_min_ns\":" + std::to_string(stats.exclusive_min_ns);
    out += ",\"exclusive_max_ns\":" + std::to_string(stats.exclusive_max_ns);
    out += ",\"mem_alloc_bytes\":" + std::to_string(stats.mem_alloc_bytes);
    out += ",\"mem_free_bytes\":" + std::to_string(stats.mem_free_bytes);
    out += ",\"annotations\":{";
    struct Annotations {
        std::string *out;
        bool         first;
    } state{&out, true};
    waggle_node_annotations(
        node,
        [](void *user, char const *key, size_t key_length, char const *value, size_t value_length) {
            auto &a = *static_cast<Annotations *>(user);
            if (!a.first) {
                *a.out += ',';
            }
            a.first = false;
            json_string(*a.out, std::string_view(key, key_length));
            *a.out += ':';
            json_string(*a.out, std::string_view(value, value_length));
        },
        &state);
    out += "},\"children\":[";
    size_t const n = waggle_node_child_count(node);
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) {
            out += ',';
        }
        node_json(out, waggle_node_child(node, i));
    }
    out += "]}";
}

} // namespace detail

// ---------------------- Names and sites ----------------------

/// The id of a string; the same characters always give the same id.
APIARY_EXPOSE inline std::uint32_t intern(std::string const &text) {
    return detail::intern(text);
}

/// The id of a call site, registered once: a zone opened at it costs no lookup.
APIARY_EXPOSE inline std::uint32_t register_site(std::string const &name, std::string const &file = "", int line = 0,
                                                 std::string const &function = "", std::string const &domain = "python") {
    return waggle_register_site(name.data(), name.size(), file.c_str(), line, function.c_str(),
                                waggle_register_domain(domain.data(), domain.size()));
}

// ---------------------- Recording ----------------------

/// Open a zone at @p site, named by it or by @p name_id when not 0. Returns whether it opened
/// one; call ``zone_end`` exactly when it did.
APIARY_EXPOSE inline bool zone_begin(std::uint32_t site, std::uint32_t name_id = 0) {
    return waggle_zone_begin(site, name_id) != 0;
}

/// Close the zone the calling thread opened last.
APIARY_EXPOSE inline void zone_end() {
    waggle_zone_end();
}

/// Whether zones and annotations are recorded now.
APIARY_EXPOSE inline bool enabled() {
    return waggle_enabled() != 0;
}

/// Turn recording on or off for the whole process.
APIARY_EXPOSE inline void set_enabled(bool on) {
    waggle_set_enabled(on ? 1 : 0);
}

/// Attach a string annotation to the innermost open zone.
APIARY_EXPOSE inline void annotate(std::string const &key, std::string const &value) {
    if (waggle_enabled() != 0) {
        waggle_annotate_str(detail::intern(key), detail::intern(value));
    }
}

/// Attach an integer annotation to the innermost open zone.
APIARY_EXPOSE inline void annotate(std::string const &key, std::int64_t value) {
    if (waggle_enabled() != 0) {
        waggle_annotate_i64(detail::intern(key), value);
    }
}

/// Attach a floating-point annotation to the innermost open zone.
APIARY_EXPOSE inline void annotate(std::string const &key, double value) {
    if (waggle_enabled() != 0) {
        waggle_annotate_f64(detail::intern(key), value);
    }
}

/// Record an allocation of @p bytes in the innermost open zone.
APIARY_EXPOSE inline void mem_alloc(std::int64_t bytes) {
    if (bytes != 0) {
        waggle_mem_alloc(nullptr, bytes);
    }
}

/// Record a free of @p bytes in the innermost open zone.
APIARY_EXPOSE inline void mem_free(std::int64_t bytes) {
    if (bytes != 0) {
        waggle_mem_free(nullptr, bytes);
    }
}

/// Name the calling thread in reports and viewers.
APIARY_EXPOSE inline void set_thread_name(std::string const &name) {
    waggle_set_thread_name(name.data(), name.size());
}

/// The calling thread's id, as reports and viewers show it.
APIARY_EXPOSE inline std::uint32_t current_thread_id() {
    return waggle_current_thread_id();
}

// ---------------------- Settings and lifecycle ----------------------

/// Set settings by name, each value written as on a command line, all as one change. Returns how
/// many were refused because another library set them first, or -1 for an unknown name or bad
/// value, changing nothing.
APIARY_EXPOSE inline int configure(std::map<std::string, std::string> const &settings) {
    std::vector<char const *> keys;
    std::vector<char const *> values;
    for (auto const &[key, value] : settings) {
        keys.push_back(key.c_str());
        values.push_back(value.c_str());
    }
    return waggle_config_set(keys.data(), values.data(), keys.size());
}

/// As ``configure``, whoever set the settings before.
APIARY_EXPOSE inline int override_settings(std::map<std::string, std::string> const &settings) {
    std::vector<char const *> keys;
    std::vector<char const *> values;
    for (auto const &[key, value] : settings) {
        keys.push_back(key.c_str());
        values.push_back(value.c_str());
    }
    return waggle_config_override(keys.data(), values.data(), keys.size());
}

/// The setting named @p key as text, or None for an unknown name.
APIARY_EXPOSE inline std::optional<std::string> setting(std::string const &key) {
    std::int64_t const length = waggle_config_get(key.c_str(), nullptr, 0);
    if (length < 0) {
        return std::nullopt;
    }
    std::string value(static_cast<size_t>(length) + 1, '\0');
    waggle_config_get(key.c_str(), value.data(), value.size());
    value.resize(static_cast<size_t>(length));
    return value;
}

/// Count @p name as a library using the profiler until its ``finalize``.
APIARY_EXPOSE inline void init(std::string const &name, std::string const &version = "") {
    waggle_init(name.c_str(), version.c_str(), "", "", 0, "");
}

/// Release @p name's ``init``; the last release writes the outputs the settings ask for.
APIARY_EXPOSE inline void finalize(std::string const &name) {
    waggle_finalize(name.c_str());
}

/// The process is exiting: write the outputs the settings ask for, unless a finalize already did.
/// ``waggle`` registers it with Python's ``atexit`` when it first loads this module.
APIARY_EXPOSE inline void at_exit() {
    waggle_at_exit();
}

/// Drain every thread's recorded events into the aggregated trees.
APIARY_EXPOSE inline void flush() {
    waggle_flush();
}

/// Clear every statistic, keeping the zones open now, which are timed from here.
APIARY_EXPOSE inline void reset() {
    waggle_reset();
}

// ---------------------- Reports and the server ----------------------

/// Print the text report to standard output.
APIARY_EXPOSE inline void print_report(bool detailed = false) {
    waggle_print_report(detailed ? 1 : 0);
    std::fflush(stdout);
}

/// Write the aggregated trees as JSON to @p path; whether it succeeded.
APIARY_EXPOSE inline bool export_json(std::string const &path) {
    return waggle_export_json(path.c_str()) == 0;
}

/// The aggregated trees as JSON: ``{"threads": [{"id", "name", "root"}]}``, each root a node with
/// its children; merged across threads into one when @p merge_threads.
APIARY_EXPOSE inline std::string snapshot_json(bool merge_threads = false) {
    waggle_snapshot *const snapshot = waggle_snapshot_take(merge_threads ? WAGGLE_SNAPSHOT_MERGE_THREADS : 0U);
    std::string            out      = "{\"threads\":[";
    size_t const           n        = waggle_snapshot_thread_count(snapshot);
    for (size_t t = 0; t < n; ++t) {
        if (t > 0) {
            out += ',';
        }
        size_t      length = 0;
        char const *name   = waggle_snapshot_thread_name(snapshot, t, &length);
        out += "{\"id\":" + std::to_string(waggle_snapshot_thread_id(snapshot, t)) + ",\"name\":";
        detail::json_string(out, std::string_view(name, length));
        out += ",\"root\":";
        detail::node_json(out, waggle_snapshot_root(snapshot, t));
        out += '}';
    }
    out += "]}";
    waggle_snapshot_release(snapshot);
    return out;
}

/// Start the live server on @p port, or the first free port after it, unless one runs.
APIARY_EXPOSE inline void start_server(std::uint16_t port = 19216) {
    waggle_server_start(port);
}

/// Whether the live server runs.
APIARY_EXPOSE inline bool server_running() {
    return waggle_server_running() != 0;
}

/// The live server's port, 0 when none runs.
APIARY_EXPOSE inline std::uint16_t server_port() {
    return waggle_server_port();
}

/// Send @p json (an object) to every connected viewer as a message of type @p type.
APIARY_EXPOSE inline void publish(std::string const &type, std::string const &json) {
    waggle_publish(type.c_str(), json.data(), json.size());
}

/// Zones opened so far, on every thread, whether or not their events were dropped.
APIARY_EXPOSE inline std::uint64_t total_push_count() {
    return waggle_total_push_count();
}

/// Zones closed so far, on every thread.
APIARY_EXPOSE inline std::uint64_t total_pop_count() {
    return waggle_total_pop_count();
}

/// What opening a recorded zone costs, in nanoseconds, measured once.
APIARY_EXPOSE inline double push_overhead_ns() {
    return waggle_push_overhead_ns();
}

/// What closing a recorded zone costs, in nanoseconds, measured once.
APIARY_EXPOSE inline double pop_overhead_ns() {
    return waggle_pop_overhead_ns();
}

/// The version of the C interface the loaded collector provides: (major, minor).
APIARY_EXPOSE inline std::tuple<std::uint32_t, std::uint32_t> abi_version() {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    waggle_abi_version(&major, &minor);
    return {major, minor};
}

} // namespace waggle_python
