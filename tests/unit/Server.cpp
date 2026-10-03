//----------------------------------------------------------------------------------------------
// Copyright (c) The Einsums Developers. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.
//----------------------------------------------------------------------------------------------

#include "Server.hpp"

#include <Waggle/Config.hpp>

#include <Waggle/Waggle.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "Consumer.hpp"
#include "LogQueue.hpp"
#include "Profiler.hpp"
#include "RequestHandlers.hpp"
#include "StringTable.hpp"

#ifndef _WIN32
#    include <csignal>
#endif

#include "Sockets.hpp"

using namespace waggle;
using waggle_test::connect_to;
using waggle_test::free_port;
using waggle_test::receive;

namespace {

using test_socket = waggle_test::socket_t;

void close_client(test_socket fd) {
    waggle_test::close_socket(fd);
}

void wait_until_queued(test_socket fd) {
    REQUIRE(waggle_test::wait_until_queued(fd));
}

/// Everything @p fd receives until the server closes it.
std::string receive_all(test_socket fd) {
    std::string received;
    while (receive(fd, received) > 0) {
    }
    return received;
}

/// Send one request line and return the response to it, waiting up to 10 s for the consumer's next
/// tick (every ~500 ms) to answer. Snapshot and other lines streamed meanwhile are skipped.
std::string request(test_socket fd, std::string const &method) {
    std::string const line = R"({"type":"request","id":"t1","method":")" + method + R"(","params":{}})" + "\n";
    REQUIRE(waggle_test::send_all(fd, line));
    waggle_test::set_receive_timeout(fd, 10);

    std::string received;
    while (true) {
        for (size_t start = 0, end; (end = received.find('\n', start)) != std::string::npos; start = end + 1) {
            std::string const record = received.substr(start, end - start);
            if (record.find(R"("type":"response")") != std::string::npos && record.find(R"("id":"t1")") != std::string::npos) {
                return record;
            }
        }
        REQUIRE(receive(fd, received) > 0); // a timeout or a closed connection fails here
    }
}

} // namespace

// The handler table used to belong to the server, and TaskPool and ComputeGraph registered only if
// a server already existed when they first ran: one started later, or a profiler constructed
// before the options were read, left the viewer's TaskPool and graph panels empty for good. The
// profiler owns the table now, and a server started at any time answers from it.
TEST_CASE("A handler registered before the server starts is answered", "[profiler][server]") {
    auto &prof = Profiler::instance();
    prof.register_handler("test_early_handler", [](std::string const &) { return std::string(R"({"answer":42})"); });

    prof.start_server(free_port()); // a no-op if another case started it first
    REQUIRE(prof.server() != nullptr);
    REQUIRE(prof.server()->is_running());

    test_socket const client = connect_to(prof.server()->port());
    REQUIRE(client != waggle_test::kNoSocket);
    std::string const response = request(client, "test_early_handler");
    CHECK(response.find(R"("data":{"answer":42})") != std::string::npos);

    prof.unregister_handler("test_early_handler");
    CHECK(request(client, "test_early_handler").find("unknown method") != std::string::npos);
    close_client(client);
}

TEST_CASE("A published message reaches a connected viewer with its type", "[profiler][server]") {
    StringTable     strings;
    SiteTable       sites;
    Consumer        consumer(strings, sites);
    RequestHandlers handlers;
    uint16_t const  port = free_port();
    Server          server(consumer, strings, handlers, "127.0.0.1", port);
    REQUIRE(server.is_running());

    server.publish("test_event", R"({"value":7,"label":"x"})");
    server.publish("test_empty", "{}");

    test_socket const client = connect_to(port);
    REQUIRE(client != waggle_test::kNoSocket);
    wait_until_queued(client);
    server.shutdown();

    std::string const received = receive_all(client);
    close_client(client);

    INFO("received: " << received);
    CHECK(received.find(R"({"type":"test_event","value":7,"label":"x"})") != std::string::npos);
    CHECK(received.find(R"({"type":"test_empty"})") != std::string::npos);
}

TEST_CASE("A session file embeds every registered section", "[profiler][server]") {
    StringTable     strings;
    SiteTable       sites;
    Consumer        consumer(strings, sites);
    RequestHandlers handlers;
    handlers.add_session_section("test_section", [] { return std::string(R"([1,2,3])"); });
    Server server(consumer, strings, handlers, "127.0.0.1", free_port());

    auto const path = std::filesystem::temp_directory_path() / "waggle_session_sections.json";
    std::filesystem::remove(path);
    server.export_session(path.string(), "sections");
    server.shutdown();

    std::stringstream contents;
    {
        std::ifstream in(path);
        contents << in.rdbuf();
    } // closed before the remove: Windows will not delete a file that is open
    std::filesystem::remove(path);
    std::string const text = contents.str();
    CHECK(text.find(R"("format": "waggle-session")") != std::string::npos);
    CHECK(text.find(R"("version": 1)") != std::string::npos);
    // A library's data sits under "extensions", by its namespaced key, not beside the profiler's.
    auto const extensions = text.find(R"("extensions": {)");
    REQUIRE(extensions != std::string::npos);
    CHECK(text.find(R"("test_section": [1,2,3])", extensions) != std::string::npos);
}

/// Reads a server's JSON Lines stream, keeping what arrived past the line it returned.
class LineReader {
  public:
    explicit LineReader(test_socket fd) : _fd(fd) { waggle_test::set_receive_timeout(_fd, 1); }

    /// The next line holding every string in @p wanted, skipping others; fails after 10 s. The
    /// deadline is overall, since a server streaming snapshots never lets a per-recv timeout expire.
    std::string next(std::initializer_list<std::string_view> wanted) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (true) {
            for (size_t end; (end = _buffer.find('\n')) != std::string::npos;) {
                std::string record = _buffer.substr(0, end);
                _buffer.erase(0, end + 1);
                if (std::ranges::all_of(wanted, [&](std::string_view w) { return record.find(w) != std::string::npos; })) {
                    return record;
                }
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            REQUIRE(receive(_fd, _buffer) != 0); // a closed connection fails; a timeout reads again
        }
    }

  private:
    test_socket _fd;
    std::string _buffer;
};

#ifndef _WIN32
// A send to a viewer that has disconnected raised SIGPIPE on Linux, which ends a program that has
// not chosen to ignore it; Einsums ignored it, so only a program without Einsums died. macOS sets
// SO_NOSIGPIPE per socket and never showed it.
TEST_CASE("A viewer that disconnects does not end the program", "[profiler][server]") {
    // The default action, which ends the process: as a program that never touched SIGPIPE has it.
    struct sigaction current{};
    REQUIRE(sigaction(SIGPIPE, nullptr, &current) == 0);
    REQUIRE(current.sa_handler == SIG_DFL);

    StringTable     strings;
    SiteTable       sites;
    Consumer        consumer(strings, sites);
    RequestHandlers handlers;
    uint16_t const  port = free_port();
    Server          server(consumer, strings, handlers, "127.0.0.1", port);
    REQUIRE(server.is_running());

    test_socket const gone = connect_to(port);
    REQUIRE(gone != waggle_test::kNoSocket);
    wait_until_queued(gone);
    server.tick(); // accepts the viewer and sends it the meta line
    close_client(gone);

    // Keep sending to the closed socket: the first sends may land before the peer's reset does.
    for (int i = 0; i < 50; ++i) {
        server.publish("test_event", R"({"value":1})");
        server.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Still serving: a new viewer is accepted and answered.
    test_socket const client = connect_to(port);
    REQUIRE(client != waggle_test::kNoSocket);
    wait_until_queued(client);
    server.tick();
    LineReader reader(client);
    CHECK(reader.next({R"("type":"meta")"}).find(R"("type":"meta")") != std::string::npos);
    close_client(client);
    server.shutdown();
}
#endif

TEST_CASE("The meta message lists every client and every switched-off collector", "[profiler][server]") {
    StringTable     strings;
    SiteTable       sites;
    Consumer        consumer(strings, sites);
    RequestHandlers handlers;
    handlers.add_client({.name = "first-lib", .version = "1.2.3", .git_commit = "abc123"});
    handlers.add_client({.name = "second-lib", .version = "0.1"});
    handlers.add_duplicate({.path = "/opt/lib/libwaggle.0.dylib", .abi_major = 0, .abi_minor = 1});
    handlers.add("zeta_method", [](std::string const &) { return std::string("{}"); });
    handlers.add("alpha_method", [](std::string const &) { return std::string("{}"); });
    uint16_t const port = free_port();
    Server         server(consumer, strings, handlers, "127.0.0.1", port);
    REQUIRE(server.is_running());
    CHECK(server.port() == port);

    test_socket const client = connect_to(port);
    REQUIRE(client != waggle_test::kNoSocket);
    wait_until_queued(client);
    server.tick(); // accepts the viewer and sends it the meta line

    LineReader        reader(client);
    std::string const meta = reader.next({R"("type":"meta")"});
    CHECK(meta.find(R"({"name":"first-lib","version":"1.2.3","git_commit":"abc123")") != std::string::npos);
    CHECK(meta.find(R"({"name":"second-lib","version":"0.1")") != std::string::npos);
    // The first client's build stays at the top level, where viewers read it.
    CHECK(meta.find(R"("git_commit":"abc123","git_branch":"","git_dirty":false,"build_type":"","clients":[)") != std::string::npos);
    // What the viewer may ask this program, sorted, so it shows only the panels it can fill.
    CHECK(meta.find(R"("handlers":["alpha_method","zeta_method"])") != std::string::npos);
    // Copies of the collector that switched themselves off: their libraries' zones are missing.
    CHECK(meta.find(R"("duplicates":[{"path":"/opt/lib/libwaggle.0.dylib","abi":"0.1"}])") != std::string::npos);
    // The allocation track comes with the first update, whole.
    CHECK(reader.next({R"("type":"memory")"}).find(R"("samples":[)") != std::string::npos);
    close_client(client);
    server.shutdown();
}

// Einsums wired its log sink and println forwarding only if a server existed while logging was
// set up, so a server started later showed an empty log panel. Both now go through the profiler,
// which forwards to whatever server runs when the message arrives.
TEST_CASE("Log messages and program output reach a server started later", "[profiler][server]") {
    auto &prof = Profiler::instance();
    prof.start_server(free_port()); // a no-op if an earlier case started it
    REQUIRE(prof.server() != nullptr);
    REQUIRE(prof.server()->is_running());

    test_socket const client = connect_to(prof.server()->port());
    REQUIRE(client != waggle_test::kNoSocket);
    // Messages go to the viewers connected when the server next ticks, so wait until the consumer's
    // tick has accepted this one: its meta line says so.
    LineReader reader(client);
    (void)reader.next({R"("type":"meta")"});
    prof.log(3, std::chrono::system_clock::now(), "/some/dir/source.cpp", 42, "a_function", "late-server log line");
    prof.output("late-server output line");

    std::string const log = reader.next({R"("type":"log")", "late-server log line"});
    CHECK(log.find(R"("level":3)") != std::string::npos);
    CHECK(log.find(R"("file":"source.cpp")") != std::string::npos); // the basename
    CHECK(log.find(R"("line":42)") != std::string::npos);
    CHECK_FALSE(reader.next({R"("type":"output")", "late-server output line"}).empty());
    close_client(client);
}

// Regression: shutdown() drained only to clients the server had already accepted, so one still
// in the listen backlog - a client that connected after the last tick, which for a program that
// finishes between two ticks is every client - received nothing, and `einsums bench run` lost
// the benchmark results of every short performance test.
TEST_CASE("Server shutdown delivers queued results to a client it has not yet accepted", "[profiler][server]") {
    StringTable     strings;
    SiteTable       sites;
    Consumer        consumer(strings, sites);
    RequestHandlers handlers;
    uint16_t const  port = free_port();
    Server          server(consumer, strings, handlers, "127.0.0.1", port);
    REQUIRE(server.is_running());

    server.publish("benchmark_result", R"({"label":"short-test N=8","metric":"t_einsum","value_us":12.5})");

    test_socket const client = connect_to(port); // connected, but no tick() has run to accept it
    REQUIRE(client != waggle_test::kNoSocket);
    wait_until_queued(client);
    server.shutdown();

    std::string const received = receive_all(client);
    close_client(client);

    INFO("received: " << received);
    REQUIRE(received.find(R"("type":"benchmark_result")") != std::string::npos);
    REQUIRE(received.find(R"("label":"short-test N=8")") != std::string::npos);
}
