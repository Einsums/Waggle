//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#pragma once

// A viewer's side of the server's sockets, for the tests: the calls they make, alike on POSIX and
// Windows.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/ioctl.h>
#    include <sys/socket.h>
#    include <unistd.h>
#    ifdef __linux__
#        include <linux/sockios.h>
#    endif
#endif

namespace waggle_test {

#ifdef _WIN32
using socket_t                      = SOCKET;
inline constexpr socket_t kNoSocket = INVALID_SOCKET;

inline bool start_sockets() {
    static bool const started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

inline void close_socket(socket_t fd) {
    closesocket(fd);
}
#else
using socket_t                      = int;
inline constexpr socket_t kNoSocket = -1;

inline bool start_sockets() {
    return true;
}

inline void close_socket(socket_t fd) {
    ::close(fd);
}
#endif

/// A loopback port nothing is listening on, the system's pick; 0 if there is none.
inline std::uint16_t free_port() {
    start_sockets();
    socket_t const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kNoSocket) {
        return 0;
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t  len       = sizeof(addr);
    bool const ok        = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
                           ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0;
    close_socket(fd);
    return ok ? ntohs(addr.sin_port) : 0;
}

/// A socket connected to @p port on loopback, or kNoSocket.
inline socket_t connect_to(std::uint16_t port) {
    start_sockets();
    socket_t const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kNoSocket) {
        return kNoSocket;
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close_socket(fd);
        return kNoSocket;
    }
    return fd;
}

/// Send all of @p data; whether it went.
inline bool send_all(socket_t fd, std::string_view data) {
    while (!data.empty()) {
        auto const n = ::send(fd, data.data(), static_cast<int>(data.size()), 0);
        if (n <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

/// Receive what comes into @p into, waiting; the byte count, 0 at a closed connection, negative on
/// an error or the receive timeout.
inline long receive(socket_t fd, std::string &into) {
    char       buffer[4096]; // NOLINT(modernize-avoid-c-arrays)
    auto const n = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
    if (n > 0) {
        into.append(buffer, static_cast<std::size_t>(n));
    }
    return static_cast<long>(n);
}

/// Make a waiting receive on @p fd give up after @p seconds.
inline void set_receive_timeout(socket_t fd, int seconds) {
#ifdef _WIN32
    DWORD const ms = static_cast<DWORD>(seconds) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const *>(&ms), sizeof(ms));
#else
    timeval timeout{};
    timeout.tv_sec = seconds;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

/// Returns once the server's kernel holds the connection in its listen backlog. connect() returns
/// when the client has the SYN-ACK, which can be before the server has processed the final ACK
/// (macOS hands loopback input to a separate thread), and until then accept() finds nothing. A byte
/// the server's kernel acknowledged arrived after that ACK, so it proves the connection is queued.
/// Windows has no count of unacknowledged bytes to read; its loopback completes the handshake as
/// connect() returns, and a short pause covers the rest.
inline bool wait_until_queued(socket_t fd) {
    if (!send_all(fd, "\n")) { // an empty request line, which the server skips
        return false;
    }
#ifdef _WIN32
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return true;
#else
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        int n = 0;
#    ifdef __APPLE__
        socklen_t len = sizeof(n);
        if (::getsockopt(fd, SOL_SOCKET, SO_NWRITE, &n, &len) != 0) {
            return false;
        }
#    else
        if (::ioctl(fd, SIOCOUTQ, &n) != 0) {
            return false;
        }
#    endif
        if (n == 0) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
}

} // namespace waggle_test
