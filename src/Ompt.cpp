//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

// The OpenMP source. An OpenMP runtime that supports OMPT (LLVM's libomp, Intel's; not GCC's
// libgomp) looks up `ompt_start_tool` in the process when it starts, once, and a tool that
// declines then cannot attach later. Waggle exports it and declines unless the `sources` setting
// names "openmp" at that moment.
//
// The OMPT declarations are written out here from the OpenMP 5 specification rather than taken
// from omp-tools.h, so the collector builds and links without any OpenMP runtime; a test checks
// them against omp-tools.h wherever that header exists.

#include <Waggle/Waggle.h>

#include <fmt/format.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Duplicates.hpp"
#include "Profiler.hpp"
#include "Sources.hpp"

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <cstdlib>
#    include <cxxabi.h>
#    include <dlfcn.h>
#endif

#include "OmptTypes.hpp"

namespace {

using namespace waggle::ompt_types; // NOLINT(google-build-using-namespace)

/// What became of the runtime's request for a tool.
enum class State : int {
    NotAsked, ///< No OMPT runtime has started.
    Declined, ///< One started while the source was not requested.
    Active,   ///< Callbacks registered.
    Unusable, ///< The runtime could not register the callbacks Waggle needs.
};

std::atomic<State> g_state{State::NotAsked};
std::mutex         g_status_mutex;
std::string        g_runtime; // the runtime's version string, under g_status_mutex
std::string        g_problem; // why the runtime is unusable, under g_status_mutex

/// The zone sites of one parallel region's call site.
struct Region {
    uint32_t region_site; ///< The region, on the thread that encountered it.
    uint32_t work_site;   ///< One thread's share of it, on each thread of the team.
};

uint32_t                                         g_domain = 0;
std::int32_t const                              *g_switch = nullptr;    // the "openmp" domain's switch, global and domain together
uint32_t                                         g_barrier_sites[11]{}; // NOLINT(modernize-avoid-c-arrays): by ompt_sync_region_t
std::mutex                                       g_regions_mutex;
std::deque<Region>                               g_regions;   // stable addresses, under g_regions_mutex
std::unordered_map<void const *, Region const *> g_region_of; // by call site, under g_regions_mutex

/// Whether each zone this thread's callbacks began opened: OMPT pairs every begin with an end on
/// the same thread, innermost first, so a stack keeps them matched even when recording switches
/// in between.
thread_local std::vector<bool> t_opened;

auto recording() -> bool {
    return std::atomic_ref<std::int32_t>(*const_cast<std::int32_t *>(g_switch)).load(std::memory_order_relaxed) != 0; // NOLINT
}

void begin(uint32_t site) {
    t_opened.push_back(recording() && waggle_zone_begin(site, 0) != 0);
}

void end() {
    if (t_opened.empty()) {
        return; // an end whose begin came before the tool attached
    }
    bool const opened = t_opened.back();
    t_opened.pop_back();
    if (opened) {
        waggle_zone_end();
    }
}

auto site(std::string const &name, std::string const &function) -> uint32_t {
    return waggle_register_site(name.data(), name.size(), "", 0, function.c_str(), g_domain); // g_domain is set first
}

} // namespace

WAGGLE_NAMESPACE_BEGIN

auto short_function_name(std::string_view name) -> std::string {
    // Drop the argument list and a template's return type: the first '(' that opens an argument
    // list ends the name, and the last space before it, outside brackets, starts it. A '(' that
    // does not follow a name is part of one, as in "(anonymous namespace)", and so is the "()"
    // of "operator()".
    int    angle = 0;
    size_t start = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        char const c = name[i];
        if (c == '<') {
            ++angle;
        } else if (c == '>') {
            --angle;
        } else if (angle == 0 && c == ' ') {
            start = i + 1;
        } else if (angle == 0 && c == '(') {
            bool const after_name    = i > start && name[i - 1] != ':';
            bool const operator_call = name.substr(0, i).ends_with("operator") && name.substr(i).starts_with("()");
            if (after_name && !operator_call) {
                return std::string(name.substr(start, i - start));
            }
            // Part of the name: skip to its ')'.
            int paren = 0;
            for (; i < name.size(); ++i) {
                paren += name[i] == '(' ? 1 : name[i] == ')' ? -1 : 0;
                if (paren == 0) {
                    break;
                }
            }
        }
    }
    return std::string(name.substr(start));
}

WAGGLE_NAMESPACE_END

namespace {

/// The function a code address is in, without its arguments: "einsums::pack<double>".
///
/// A region is reported by the return address of the call into the runtime, so it is named after
/// the function holding that call. A region that shares nothing with its function, last in it,
/// may be compiled as a jump to the runtime rather than a call; it is then named after the
/// function's caller.
auto function_at(void const *address) -> std::string {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) != 0) {
        char path[MAX_PATH]; // NOLINT(modernize-avoid-c-arrays)
        if (GetModuleFileNameA(module, path, MAX_PATH) > 0) {
            std::string_view file = path;
            file                  = file.substr(file.find_last_of("\\/") + 1);
            return fmt::format("{}+{:#x}", file, static_cast<char const *>(address) - reinterpret_cast<char const *>(module));
        }
    }
    return fmt::format("{}", address);
#else
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_sname == nullptr) {
        return fmt::format("{}", address);
    }
    int               status    = 0;
    char             *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
    std::string const name      = status == 0 && demangled != nullptr ? demangled : info.dli_sname;
    std::free(demangled); // NOLINT(cppcoreguidelines-no-malloc)
    return waggle::short_function_name(name);
#endif
}

auto region_at(void const *codeptr) -> Region const * {
    // A thread's own cache first: the shared map's lock is taken once per call site per thread.
    thread_local std::unordered_map<void const *, Region const *> cache;
    if (auto it = cache.find(codeptr); it != cache.end()) {
        return it->second;
    }
    std::scoped_lock const lock(g_regions_mutex);
    auto [it, added] = g_region_of.try_emplace(codeptr, nullptr);
    if (added) {
        std::string const function = function_at(codeptr);
        it->second = &g_regions.emplace_back(Region{site("omp parallel: " + function, function), site("omp work: " + function, function)});
    }
    cache.emplace(codeptr, it->second);
    return it->second;
}

void on_thread_begin(ompt_thread_t type, ompt_data_t * /*thread_data*/) {
    if (type == ompt_thread_worker) {
        static std::atomic<int> workers{0};
        std::string const       name = fmt::format("OpenMP worker {}", workers.fetch_add(1) + 1);
        waggle_set_thread_name(name.data(), name.size());
    }
}

void on_parallel_begin(ompt_data_t * /*encountering_task*/, ompt_frame_t const * /*frame*/, ompt_data_t *parallel_data,
                       unsigned /*requested*/, int /*flags*/, void const                                *codeptr) {
    Region const *region = region_at(codeptr);
    parallel_data->ptr   = const_cast<Region *>(region); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    begin(region->region_site);
}

void on_parallel_end(ompt_data_t * /*parallel_data*/, ompt_data_t * /*encountering_task*/, int /*flags*/, void const * /*codeptr*/) {
    end();
}

void on_implicit_task(ompt_scope_endpoint_t endpoint, ompt_data_t *parallel_data, ompt_data_t * /*task_data*/, unsigned /*team*/,
                      unsigned /*index*/, int                      flags) {
    // The initial task spans the whole program: as a zone it would wrap every other.
    if ((flags & ompt_task_initial) != 0) {
        return;
    }
    if (endpoint == ompt_scope_begin) {
        auto const *region = parallel_data != nullptr ? static_cast<Region const *>(parallel_data->ptr) : nullptr;
        if (region == nullptr) {
            t_opened.push_back(false); // keeps the end paired
            return;
        }
        begin(region->work_site);
    } else if (endpoint == ompt_scope_end) {
        end(); // a worker's parallel_data may already be gone here
    }
}

void on_sync_region_wait(ompt_sync_region_t kind, ompt_scope_endpoint_t endpoint, ompt_data_t * /*parallel_data*/,
                         ompt_data_t * /*task_data*/, void const * /*codeptr*/) {
    auto const index = static_cast<size_t>(kind);
    if (index >= std::size(g_barrier_sites) || g_barrier_sites[index] == 0) {
        return; // taskwait, taskgroup, reduction: not barriers
    }
    if (endpoint == ompt_scope_begin) {
        begin(g_barrier_sites[index]);
    } else if (endpoint == ompt_scope_end) {
        end();
    }
}

auto initialize(ompt_function_lookup_t lookup, int /*initial_device*/, ompt_data_t * /*tool_data*/) -> int {
    auto const set_callback = reinterpret_cast<ompt_set_callback_t>(lookup("ompt_set_callback"));
    if (set_callback == nullptr) {
        std::scoped_lock const lock(g_status_mutex);
        g_problem = "the OpenMP runtime offers no ompt_set_callback";
        g_state.store(State::Unusable);
        return 0;
    }
    g_domain                                                     = waggle_register_domain("openmp", 6);
    g_switch                                                     = waggle_domain_flag(g_domain);
    auto barrier                                                 = [](char const *name) { return site(name, ""); };
    g_barrier_sites[ompt_sync_region_barrier]                    = barrier("omp barrier wait");
    g_barrier_sites[ompt_sync_region_barrier_implicit]           = barrier("omp barrier wait (implicit)");
    g_barrier_sites[ompt_sync_region_barrier_explicit]           = barrier("omp barrier wait");
    g_barrier_sites[ompt_sync_region_barrier_implementation]     = barrier("omp barrier wait (runtime)");
    g_barrier_sites[ompt_sync_region_barrier_implicit_workshare] = barrier("omp barrier wait (end of loop)");
    g_barrier_sites[ompt_sync_region_barrier_implicit_parallel]  = barrier("omp barrier wait (end of region)");
    g_barrier_sites[ompt_sync_region_barrier_teams]              = barrier("omp barrier wait (teams)");

    struct Wanted {
        ompt_callbacks_t event;
        ompt_callback_t  callback;
        char const      *name;
    };
    Wanted const wanted[] = {
        // NOLINT(modernize-avoid-c-arrays)
        {ompt_callback_thread_begin, reinterpret_cast<ompt_callback_t>(&on_thread_begin), "thread_begin"},
        {ompt_callback_parallel_begin, reinterpret_cast<ompt_callback_t>(&on_parallel_begin), "parallel_begin"},
        {ompt_callback_parallel_end, reinterpret_cast<ompt_callback_t>(&on_parallel_end), "parallel_end"},
        {ompt_callback_implicit_task, reinterpret_cast<ompt_callback_t>(&on_implicit_task), "implicit_task"},
        {ompt_callback_sync_region_wait, reinterpret_cast<ompt_callback_t>(&on_sync_region_wait), "sync_region_wait"},
    };
    std::string missing;
    for (auto const &w : wanted) {
        auto const result = set_callback(w.event, w.callback);
        if (result == ompt_set_error || result == ompt_set_never) {
            missing += missing.empty() ? w.name : fmt::format(", {}", w.name);
        }
    }
    if (!missing.empty()) {
        // Some zones would open without their ends: record none rather than an unbalanced tree.
        for (auto const &w : wanted) {
            set_callback(w.event, nullptr);
        }
        std::scoped_lock const lock(g_status_mutex);
        g_problem = fmt::format("the OpenMP runtime does not support the {} callback(s)", missing);
        g_state.store(State::Unusable);
        return 0;
    }
    g_state.store(State::Active);
    return 1; // nonzero keeps the tool
}

void finalize(ompt_data_t * /*tool_data*/) {
}

} // namespace

WAGGLE_NAMESPACE_BEGIN

namespace ompt {
auto status() -> SourceStatus {
    SourceStatus status{.name = "openmp", .state = "off", .detail = ""};
    if (collector_inactive()) {
        status.detail = "this copy of the collector is switched off";
        return status;
    }
    bool const             requested = source_requested(Profiler::instance().settings(), "openmp");
    std::scoped_lock const lock(g_status_mutex);
    switch (g_state.load()) {
    case State::Active:
        status.state  = "active";
        status.detail = g_runtime;
        break;
    case State::Unusable:
        status.state  = requested ? "unavailable" : "off";
        status.detail = g_problem;
        break;
    case State::Declined:
        if (requested) {
            status.state  = "missed";
            status.detail = "the OpenMP runtime started before the source was requested; set WAGGLE_SOURCES=openmp in the "
                            "environment, or configure Waggle before the first OpenMP call";
        }
        break;
    case State::NotAsked:
        if (requested) {
            status.state  = "waiting";
            status.detail = "no OpenMP runtime with OMPT has started: GCC's libgomp has none, LLVM's libomp and Intel's do";
        }
        break;
    }
    return status;
}
} // namespace ompt

WAGGLE_NAMESPACE_END

extern "C" WAGGLE_C_EXPORT ompt_start_tool_result_t *ompt_start_tool(unsigned int /*omp_version*/, char const *runtime_version) {
    // A copy of the collector that switched itself off leaves OpenMP to the one that did not.
    if (waggle::collector_inactive() || !waggle::source_requested(waggle::Profiler::instance().settings(), "openmp")) {
        g_state.store(State::Declined);
        return nullptr;
    }
    {
        std::scoped_lock const lock(g_status_mutex);
        g_runtime = runtime_version != nullptr ? runtime_version : "";
    }
    static ompt_start_tool_result_t result{&initialize, &finalize, {0}};
    return &result;
}
