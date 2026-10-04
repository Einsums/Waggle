//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Process.hpp"

#include <filesystem>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <climits>
#    include <cstdlib>
#    include <pthread.h>
#    include <unistd.h>
#endif
#ifdef __APPLE__
#    include <mach-o/dyld.h>
#endif
#ifdef __linux__
#    include <sys/syscall.h>
#endif

WAGGLE_NAMESPACE_BEGIN

namespace {

auto find_executable_path() -> std::string {
#if defined(__APPLE__)
    char     path[PATH_MAX]; // NOLINT(modernize-avoid-c-arrays)
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        char resolved[PATH_MAX]; // NOLINT(modernize-avoid-c-arrays)
        return realpath(path, resolved) != nullptr ? std::string(resolved) : std::string(path);
    }
#elif defined(__linux__)
    char          path[PATH_MAX]; // NOLINT(modernize-avoid-c-arrays)
    ssize_t const len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        return {path, static_cast<size_t>(len)};
    }
#elif defined(_WIN32)
    char path[MAX_PATH]; // NOLINT(modernize-avoid-c-arrays)
    if (DWORD const len = GetModuleFileNameA(nullptr, path, MAX_PATH); len > 0) {
        return {path, len};
    }
#endif
    return {};
}

} // namespace

auto process_id() -> std::int64_t {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

auto executable_path() -> std::string const & {
    static std::string const path = find_executable_path();
    return path;
}

auto executable_name() -> std::string const & {
    static std::string const name =
        executable_path().empty() ? std::string("unknown") : std::filesystem::path(executable_path()).filename().string();
    return name;
}

auto kernel_thread_id() -> std::uint64_t {
#if defined(__APPLE__)
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#elif defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#elif defined(_WIN32)
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

auto is_main_thread() -> std::optional<bool> {
#if defined(__APPLE__)
    return pthread_main_np() != 0;
#elif defined(__linux__)
    return syscall(SYS_gettid) == getpid();
#else
    return std::nullopt;
#endif
}

WAGGLE_NAMESPACE_END
