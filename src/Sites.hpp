//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

#include <Waggle/Config.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

WAGGLE_NAMESPACE_BEGIN

/// One zone in the source: its name, where it is, and which library it belongs to. The strings are
/// string-table ids.
struct Site {
    uint32_t name_id{0};
    uint32_t file_id{0};
    uint32_t func_id{0};
    int      line{0};
    uint32_t domain{0};
};

/**
 * @brief Every call site the process registered, each named by a 32-bit id.
 *
 * Registration is deduplicated by the whole description. A site is a function-local static, and a
 * zone in an inline function or template in a header gets one static per shared object under hidden
 * visibility; deduplication gives every copy the same id. Id 0 is no site.
 */
class SiteTable {
  public:
    SiteTable() { _sites.emplace_back(); }

    /// The id of @p site, registering it if new. Thread-safe.
    auto add(Site const &site) -> uint32_t {
        auto const key = std::make_tuple(site.name_id, site.file_id, site.func_id, site.line, site.domain);
        {
            std::shared_lock const lock(_mutex);
            if (auto it = _ids.find(key); it != _ids.end()) {
                return it->second;
            }
        }
        std::unique_lock const lock(_mutex);
        if (auto it = _ids.find(key); it != _ids.end()) {
            return it->second;
        }
        auto const id = static_cast<uint32_t>(_sites.size());
        _sites.push_back(site);
        _ids.emplace(key, id);
        return id;
    }

    /// The site with id @p id; an id this table never issued gives the empty site. Thread-safe.
    [[nodiscard]] auto get(uint32_t id) const -> Site {
        std::shared_lock const lock(_mutex);
        return id < _sites.size() ? _sites[id] : Site{};
    }

  private:
    using Key = std::tuple<uint32_t, uint32_t, uint32_t, int, uint32_t>;

    mutable std::shared_mutex _mutex;
    std::deque<Site>          _sites;
    std::map<Key, uint32_t>   _ids;
};

/**
 * @brief The libraries that own sites, by name. Id 0 is the unnamed domain, which sites that name
 * none belong to.
 */
class DomainTable {
  public:
    DomainTable() {
        _names.emplace_back();
        _wanted.emplace_back(true);
        _switches.emplace_back(1);
    }

    /// The id of the domain named @p name, registering it if new; "" is domain 0. Thread-safe.
    auto add(std::string_view name) -> uint32_t {
        if (name.empty()) {
            return 0;
        }
        std::unique_lock const lock(_mutex);
        if (auto it = _ids.find(std::string(name)); it != _ids.end()) {
            return it->second;
        }
        auto const id = static_cast<uint32_t>(_names.size());
        _names.emplace_back(name);
        _wanted.emplace_back(true);
        _switches.emplace_back(_global ? 1 : 0);
        _ids.emplace(std::string(name), id);
        return id;
    }

    /// Domain @p id's recording switch, 1 when the domain and the global switch are both on, at an
    /// address fixed for the life of the table: sites cache it and read it, one relaxed
    /// std::atomic_ref load, in place of reading the two. Domain 0's for an id this table never
    /// issued. Thread-safe.
    [[nodiscard]] auto switch_of(uint32_t id) const -> std::int32_t const * {
        std::scoped_lock const lock(_mutex);
        return &_switches[id < _switches.size() ? id : 0];
    }

    /// Turn domain @p id's recording on or off; an id this table never issued is ignored.
    void set_enabled(uint32_t id, bool on) {
        std::scoped_lock const lock(_mutex);
        if (id >= _switches.size()) {
            return;
        }
        _wanted[id] = on;
        store(_switches[id], _global && on);
    }

    /// The global switch changed: every domain's switch follows. Rare, so it may touch them all.
    void set_global(bool on) {
        std::scoped_lock const lock(_mutex);
        _global = on;
        for (size_t id = 0; id < _switches.size(); ++id) {
            store(_switches[id], on && _wanted[id]);
        }
    }

    /// Whether domain @p id is switched on, its own switch alone, whatever the global one says.
    [[nodiscard]] auto enabled(uint32_t id) const -> bool {
        std::scoped_lock const lock(_mutex);
        return id < _wanted.size() ? _wanted[id] : _wanted[0];
    }

    /// The name of domain @p id; "" for domain 0 or an id this table never issued. Thread-safe.
    [[nodiscard]] auto name(uint32_t id) const -> std::string {
        std::scoped_lock const lock(_mutex);
        return id < _names.size() ? _names[id] : std::string{};
    }

  private:
    mutable std::mutex      _mutex;
    std::deque<std::string> _names;
    static void store(std::int32_t &slot, bool on) { std::atomic_ref<std::int32_t>(slot).store(on ? 1 : 0, std::memory_order_relaxed); }

    /// Whether each domain is wanted on, its own setting, beside its name.
    std::deque<bool> _wanted;
    /// Each domain's switch as sites read it: wanted and the global switch on. A deque, so each
    /// stays where it is as it grows; mutable, as std::atomic_ref takes no const object.
    mutable std::deque<std::int32_t>          _switches;
    bool                                      _global{true};
    std::unordered_map<std::string, uint32_t> _ids;
};

WAGGLE_NAMESPACE_END
