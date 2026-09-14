// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/system.h>
#include <compat/compat.h>
#include <test/util/setup_common.h>
#include <util/sock.h>
#include <util/threadinterrupt.h>

#include <boost/test/unit_test.hpp>

#include <cassert>
#include <optional>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

BOOST_FIXTURE_TEST_SUITE(sock_tests, BasicTestingSetup)

static bool SocketIsClosed(const SOCKET& s)
{
    // Notice that if another thread is running and creates its own socket after `s` has been
    // closed, it may be assigned the same file descriptor number. In this case, our test will
    // wrongly pretend that the socket is not closed.
    int type;
    socklen_t len = sizeof(type);
    return getsockopt(s, SOL_SOCKET, SO_TYPE, (sockopt_arg_type)&type, &len) == SOCKET_ERROR;
}

static bool IsSocketDeniedError(int err)
{
    // WSAGetLastError() is errno on POSIX and a WSA* code on Windows, and the two
    // namespaces do not share values -- WSAEACCES is 10013, EACCES is 13. Comparing
    // against the bare POSIX name would make this silently never match on Windows,
    // i.e. a denied socket would be reported as a test failure instead of a skip.
    return err == WSAEACCES || err == EPERM;
}

static bool WarnIfSocketDenied(std::string_view syscall, int err)
{
    if (!IsSocketDeniedError(err)) return false;
    BOOST_WARN_MESSAGE(false, "Skipping socket test: " << syscall << " failed: " << NetworkErrorString(err));
    return true;
}

static std::optional<SOCKET> CreateSocket()
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == static_cast<SOCKET>(SOCKET_ERROR)) {
        const int err{WSAGetLastError()};
        if (!WarnIfSocketDenied("socket()", err)) {
            BOOST_ERROR("socket() failed: " << NetworkErrorString(err));
        }
        return std::nullopt;
    }
    return s;
}

BOOST_AUTO_TEST_CASE(constructor_and_destructor)
{
    const auto s = CreateSocket();
    if (!s) return;
    Sock* sock = new Sock(*s);
    BOOST_CHECK(*sock == *s);
    BOOST_CHECK(!SocketIsClosed(*s));
    delete sock;
    BOOST_CHECK(SocketIsClosed(*s));
}

BOOST_AUTO_TEST_CASE(move_constructor)
{
    const auto s = CreateSocket();
    if (!s) return;
    Sock* sock1 = new Sock(*s);
    Sock* sock2 = new Sock(std::move(*sock1));
    delete sock1;
    BOOST_CHECK(!SocketIsClosed(*s));
    BOOST_CHECK(*sock2 == *s);
    delete sock2;
    BOOST_CHECK(SocketIsClosed(*s));
}

BOOST_AUTO_TEST_CASE(move_assignment)
{
    const auto s1 = CreateSocket();
    if (!s1) return;
    const auto s2 = CreateSocket();
    if (!s2) {
        Sock cleanup{*s1};
        return;
    }
    Sock* sock1 = new Sock(*s1);
    Sock* sock2 = new Sock(*s2);

    BOOST_CHECK(!SocketIsClosed(*s1));
    BOOST_CHECK(!SocketIsClosed(*s2));

    *sock2 = std::move(*sock1);
    BOOST_CHECK(!SocketIsClosed(*s1));
    BOOST_CHECK(SocketIsClosed(*s2));
    BOOST_CHECK(*sock2 == *s1);

    delete sock1;
    BOOST_CHECK(!SocketIsClosed(*s1));
    BOOST_CHECK(SocketIsClosed(*s2));
    BOOST_CHECK(*sock2 == *s1);

    delete sock2;
    BOOST_CHECK(SocketIsClosed(*s1));
    BOOST_CHECK(SocketIsClosed(*s2));
}

//! A connected pair of stream sockets, on every platform.
//!
//! socketpair(2) is POSIX-only, and that single missing call was the entire reason
//! three of this suite's six cases were `#ifndef WIN32` -- cases that exercise Sock's
//! own send/recv/wait contract, which is transport-agnostic and which Windows is
//! every bit as obliged to keep. A loopback TCP pair is two connected stream sockets
//! by another route, so it serves identically and leaves one code path with no guard
//! to hide behind.
static bool CreateSocketPair(SOCKET s[2])
{
    const auto fail = [](std::string_view syscall) {
        const int err{WSAGetLastError()};
        if (!WarnIfSocketDenied(syscall, err)) {
            BOOST_ERROR(syscall << " failed: " << NetworkErrorString(err));
        }
        return false;
    };

    const SOCKET listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (listener == INVALID_SOCKET) return fail("socket()");
    // Owns the listener for the rest of this function: Sock's destructor is the
    // portable close, and the listener is never handed back to the caller.
    const Sock listen_sock{listener};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // any free port

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        return fail("bind()");
    }
    if (listen(listener, 1) == SOCKET_ERROR) return fail("listen()");

    socklen_t addr_len = sizeof(addr);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) == SOCKET_ERROR) {
        return fail("getsockname()");
    }

    const SOCKET client{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (client == INVALID_SOCKET) return fail("socket()");
    if (connect(client, reinterpret_cast<sockaddr*>(&addr), addr_len) == SOCKET_ERROR) {
        (void)Sock{client}; // close it; a temporary Sock is the portable closesocket
        return fail("connect()");
    }

    const SOCKET server{accept(listener, nullptr, nullptr)};
    if (server == INVALID_SOCKET) {
        (void)Sock{client};
        return fail("accept()");
    }

    s[0] = client;
    s[1] = server;
    return true;
}

static bool SendAndRecvMessage(const Sock& sender, const Sock& receiver)
{
    const char* msg = "abcd";
    constexpr ssize_t msg_len = 4;
    char recv_buf[10];

    const ssize_t sent{sender.Send(msg, msg_len, 0)};
    if (sent == SOCKET_ERROR && WarnIfSocketDenied("send()", WSAGetLastError())) return false;
    BOOST_CHECK_EQUAL(sent, msg_len);
    if (sent != msg_len) return false;

    const ssize_t received{receiver.Recv(recv_buf, sizeof(recv_buf), 0)};
    if (received == SOCKET_ERROR && WarnIfSocketDenied("recv()", WSAGetLastError())) return false;
    BOOST_CHECK_EQUAL(received, msg_len);
    if (received != msg_len) return false;
    BOOST_CHECK_EQUAL(strncmp(msg, recv_buf, msg_len), 0);
    return true;
}

BOOST_AUTO_TEST_CASE(send_and_receive)
{
    SOCKET s[2];
    if (!CreateSocketPair(s)) return;

    Sock* sock0 = new Sock(s[0]);
    Sock* sock1 = new Sock(s[1]);

    if (!SendAndRecvMessage(*sock0, *sock1)) {
        delete sock0;
        delete sock1;
        return;
    }

    Sock* sock0moved = new Sock(std::move(*sock0));
    Sock* sock1moved = new Sock(INVALID_SOCKET);
    *sock1moved = std::move(*sock1);

    delete sock0;
    delete sock1;

    if (!SendAndRecvMessage(*sock1moved, *sock0moved)) {
        delete sock0moved;
        delete sock1moved;
        return;
    }

    delete sock0moved;
    delete sock1moved;

    BOOST_CHECK(SocketIsClosed(s[0]));
    BOOST_CHECK(SocketIsClosed(s[1]));
}

BOOST_AUTO_TEST_CASE(wait)
{
    SOCKET s[2];
    if (!CreateSocketPair(s)) return;

    Sock sock0(s[0]);
    Sock sock1(s[1]);

    const char probe{'x'};
    const ssize_t probe_sent{sock1.Send(&probe, 1, 0)};
    if (probe_sent == SOCKET_ERROR && WarnIfSocketDenied("send()", WSAGetLastError())) return;
    BOOST_REQUIRE_EQUAL(probe_sent, 1);
    char discard;
    BOOST_REQUIRE_EQUAL(sock0.Recv(&discard, 1, 0), 1);

    std::thread waiter([&sock0]() { (void)sock0.Wait(1s, Sock::RECV); });

    BOOST_CHECK_EQUAL(sock1.Send("a", 1, 0), 1);

    waiter.join();
}

BOOST_AUTO_TEST_CASE(recv_until_terminator_limit)
{
    constexpr auto timeout = 1min; // High enough so that it is never hit.
    CThreadInterrupt interrupt;
    SOCKET s[2];
    if (!CreateSocketPair(s)) return;

    Sock sock_send(s[0]);
    Sock sock_recv(s[1]);

    const char probe{'x'};
    const ssize_t probe_sent{sock_send.Send(&probe, 1, 0)};
    if (probe_sent == SOCKET_ERROR && WarnIfSocketDenied("send()", WSAGetLastError())) return;
    BOOST_REQUIRE_EQUAL(probe_sent, 1);
    char discard;
    BOOST_REQUIRE_EQUAL(sock_recv.Recv(&discard, 1, 0), 1);

    std::thread receiver([&sock_recv, &timeout, &interrupt]() {
        constexpr size_t max_data{10};
        bool threw_as_expected{false};
        // BOOST_CHECK_EXCEPTION() writes to some variables shared with the main thread which
        // creates a data race. So mimic it manually.
        try {
            (void)sock_recv.RecvUntilTerminator('\n', timeout, interrupt, max_data);
        } catch (const std::runtime_error& e) {
            threw_as_expected = HasReason("too many bytes without a terminator")(e);
        }
        assert(threw_as_expected);
    });

    BOOST_REQUIRE_NO_THROW(sock_send.SendComplete("1234567", timeout, interrupt));
    BOOST_REQUIRE_NO_THROW(sock_send.SendComplete("89a\n", timeout, interrupt));

    receiver.join();
}

BOOST_AUTO_TEST_SUITE_END()
