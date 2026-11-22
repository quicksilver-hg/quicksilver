// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <i2p.h>
#include <logging.h>
#include <netaddress.h>
#include <netbase.h>
#include <test/util/logging.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <util/readwritefile.h>
#include <util/threadinterrupt.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <functional>
#include <string>
#include <vector>

/// Save the log level and the value of CreateSock and restore them when the test ends.
class EnvTestingSetup : public BasicTestingSetup
{
public:
    explicit EnvTestingSetup(const ChainType chainType = ChainType::MAIN,
                             TestOpts opts = {})
        : BasicTestingSetup{chainType, opts},
          m_prev_log_level{LogInstance().LogLevel()},
          m_create_sock_orig{CreateSock}
    {
        LogInstance().SetLogLevel(HgLog::Level::Trace);
    }

    ~EnvTestingSetup()
    {
        CreateSock = m_create_sock_orig;
        LogInstance().SetLogLevel(m_prev_log_level);
    }

private:
    const HgLog::Level m_prev_log_level;
    const decltype(CreateSock) m_create_sock_orig;
};

BOOST_FIXTURE_TEST_SUITE(i2p_tests, EnvTestingSetup)

BOOST_AUTO_TEST_CASE(unlimited_recv)
{
    CreateSock = [](int, int, int) {
        return std::make_unique<StaticContentsSock>(std::string(i2p::sam::MAX_MSG_SIZE + 1, 'a'));
    };

    CThreadInterrupt interrupt;
    const std::optional<CService> addr{Lookup("127.0.0.1", 9000, false)};
    const Proxy sam_proxy(addr.value(), false);
    i2p::sam::Session session(gArgs.GetDataDirNet() / "test_i2p_private_key", sam_proxy, &interrupt);

    {
        ASSERT_DEBUG_LOG("Creating persistent SAM session");
        ASSERT_DEBUG_LOG("too many bytes without a terminator");

        i2p::Connection conn;
        bool proxy_error;
        BOOST_REQUIRE(!session.Connect(CService{}, conn, proxy_error));
    }
}

BOOST_AUTO_TEST_CASE(listen_ok_accept_fail)
{
    size_t num_sockets{0};
    CreateSock = [&num_sockets](int, int, int) {
        // clang-format off
        ++num_sockets;
        // First socket is the control socket for creating the session.
        if (num_sockets == 1) {
            return std::make_unique<StaticContentsSock>(
                // reply to HELLO
                "HELLO REPLY RESULT=OK VERSION=3.1\n"
                // reply to DEST GENERATE
                "DEST REPLY PUB=WnGOLXRBqHQhdVjFlWqRxJwz9hxx~2~wGc2Vplta1KhacY4tdEGodCF1WMWVapHEnDP2HHH~b~AZzZWmW1rUqFpxji10Qah0IXVYxZVqkcScM~Yccf9v8BnNlaZbWtSoWnGOLXRBqHQhdVjFlWqRxJwz9hxx~2~wGc2Vplta1KhacY4tdEGodCF1WMWVapHEnDP2HHH~b~AZzZWmW1rUqFpxji10Qah0IXVYxZVqkcScM~Yccf9v8BnNlaZbWtSoWnGOLXRBqHQhdVjFlWqRxJwz9hxx~2~wGc2Vplta1KhacY4tdEGodCF1WMWVapHEnDP2HHH~b~AZzZWmW1rUqFpxji10Qah0IXVYxZVqkcScM~Yccf9v8BnNlaZbWtSoWnGOLXRBqHQhdVjFlWqRxJwz9hxx~2~wGc2Vplta1KhacY4tdEGodCF1WMWVapHEnDP2HHH~b~AZzZWmW1rUqLE4SD-yjT48UNI7qiTUfIPiDitCoiTTz2cr4QGfw89rBQAEAAcAAA== PRIV=WnGOLXRBqHQhdVjFlWqRxJwz9hxx~2~wGc2Vplta1KhacY4tdEGodCF1WMWVapHEnDP2HHH~b~AZzZWmW1rUqFpxji10Qah0IXVYxZVqkcScM~Yccf9v8BnNlaZbWtSoWnGOLXRBqHQhdVjFlWqRxJwz9hxx~2~wGc2Vplta1KhacY4tdEGodCF1WMWVapHEnDP2HHH~b~AZzZWmW1rUqFpxji10Qah0IXVYxZVqkcScM~Yccf9v8BnNlaZbWtSoWnGOLXRBqHQhdVjFlWqRxJwz9hxx~2~wGc2Vplta1KhacY4tdEGodCF1WMWVapHEnDP2HHH~b~AZzZWmW1rUqFpxji10Qah0IXVYxZVqkcScM~Yccf9v8BnNlaZbWtSoWnGOLXRBqHQhdVjFlWqRxJwz9hxx~2~wGc2Vplta1KhacY4tdEGodCF1WMWVapHEnDP2HHH~b~AZzZWmW1rUqLE4SD-yjT48UNI7qiTUfIPiDitCoiTTz2cr4QGfw89rBQAEAAcAAOvuCIKTyv5f~1QgGq7XQl-IqBULTB5WzB3gw5yGPtd1p0AeoADrq1ccZggLPQ4ZLUsGK-HVw373rcTfvxrcuwenqVjiN4tbbYLWtP7xXGWj6fM6HyORhU63GphrjEePpMUHDHXd3o7pWGM-ieVVQSK~1MzF9P93pQWI3Do52EeNAayz4HbpPjNhVBzG1hUEFwznfPmUZBPuaOR4-uBm1NEWEuONlNOCctE4-U0Ukh94z-Qb55U5vXjR5G4apmBblr68t6Wm1TKlzpgFHzSqLryh3stWqrOKY1H0z9eZ2z1EkHFOpD5LyF6nf51e-lV7HLMl44TYzoEHK8RRVodtLcW9lacVdBpv~tOzlZERIiDziZODPETENZMz5oy9DQ7UUw==\n"
                // reply to SESSION CREATE
                "SESSION STATUS RESULT=OK\n"
                // dummy to avoid reporting EOF on the socket
                "a"
            );
        }
        // Subsequent sockets are for recreating the session or for listening and accepting incoming connections.
        if (num_sockets % 2 == 0) {
            // Replies to Listen() and Accept()
            return std::make_unique<StaticContentsSock>(
                // reply to HELLO
                "HELLO REPLY RESULT=OK VERSION=3.1\n"
                // reply to STREAM ACCEPT
                "STREAM STATUS RESULT=OK\n"
                // continued reply to STREAM ACCEPT, violating the protocol described at
                // https://geti2p.net/en/docs/api/samv3#Accept%20Response
                // should be base64, something like
                // "IchV608baDoXbqzQKSqFDmTXPVgoDbPAhZJvNRXXxi4hyFXrTxtoOhdurNApKoUOZNc9WCgNs8CFkm81FdfGLiHIVetPG2g6F26s0CkqhQ5k1z1YKA2zwIWSbzUV18YuIchV608baDoXbqzQKSqFDmTXPVgoDbPAhZJvNRXXxi4hyFXrTxtoOhdurNApKoUOZNc9WCgNs8CFkm81FdfGLiHIVetPG2g6F26s0CkqhQ5k1z1YKA2zwIWSbzUV18YuIchV608baDoXbqzQKSqFDmTXPVgoDbPAhZJvNRXXxi4hyFXrTxtoOhdurNApKoUOZNc9WCgNs8CFkm81FdfGLiHIVetPG2g6F26s0CkqhQ5k1z1YKA2zwIWSbzUV18YuIchV608baDoXbqzQKSqFDmTXPVgoDbPAhZJvNRXXxi4hyFXrTxtoOhdurNApKoUOZNc9WCgNs8CFkm81FdfGLlSreVaCuCS5sdb-8ToWULWP7kt~lRPDeUNxQMq3cRSBBQAEAAcAAA==\n"
                "STREAM STATUS RESULT=I2P_ERROR MESSAGE=\"Session was closed\"\n"
            );
        } else {
            // Another control socket, but without creating a destination (it is cached in the session).
            return std::make_unique<StaticContentsSock>(
                // reply to HELLO
                "HELLO REPLY RESULT=OK VERSION=3.1\n"
                // reply to SESSION CREATE
                "SESSION STATUS RESULT=OK\n"
                // dummy to avoid reporting EOF on the socket
                "a"
            );
        }
        // clang-format on
    };

    CThreadInterrupt interrupt;
    const CService addr{in6_addr(IN6ADDR_LOOPBACK_INIT), /*port=*/7656};
    const Proxy sam_proxy(addr, false);
    i2p::sam::Session session(gArgs.GetDataDirNet() / "test_i2p_private_key",
                              sam_proxy,
                              &interrupt);

    i2p::Connection conn;
    for (size_t i = 0; i < 5; ++i) {
        ASSERT_DEBUG_LOG("Creating persistent SAM session");
        ASSERT_DEBUG_LOG("Persistent SAM session" /* ... created */);
        ASSERT_DEBUG_LOG("Error accepting");
        ASSERT_DEBUG_LOG("Destroying SAM session");
        BOOST_REQUIRE(session.Listen(conn));
        BOOST_REQUIRE(!session.Accept(conn));
    }
}

BOOST_AUTO_TEST_CASE(damaged_private_key)
{
    CreateSock = [](int, int, int) {
        return std::make_unique<StaticContentsSock>("HELLO REPLY RESULT=OK VERSION=3.1\n"
                                                    "SESSION STATUS RESULT=OK DESTINATION=\n");
    };

    const auto i2p_private_key_file = m_args.GetDataDirNet() / "test_i2p_private_key_damaged";

    for (const auto& [file_contents, expected_error] : std::vector<std::tuple<std::string, std::string>>{
             {"", "The private key is too short (0 < 387)"},

             {"abcd", "The private key is too short (4 < 387)"},

             {std::string(386, '\0'), "The private key is too short (386 < 387)"},

             {std::string(385, '\0') + '\0' + '\1',
              "Certificate length (1) designates that the private key should be 388 bytes, but it is only "
              "387 bytes"},

             {std::string(385, '\0') + '\0' + '\5' + "abcd",
              "Certificate length (5) designates that the private key should be 392 bytes, but it is only "
              "391 bytes"}}) {
        BOOST_REQUIRE(WriteBinaryFile(i2p_private_key_file, file_contents));

        CThreadInterrupt interrupt;
        const CService addr{in6_addr(IN6ADDR_LOOPBACK_INIT), /*port=*/7656};
        const Proxy sam_proxy{addr, false};
        i2p::sam::Session session(i2p_private_key_file, sam_proxy, &interrupt);

        {
            ASSERT_DEBUG_LOG("Creating persistent SAM session");
            ASSERT_DEBUG_LOG(expected_error);

            i2p::Connection conn;
            bool proxy_error;
            BOOST_CHECK(!session.Connect(CService{}, conn, proxy_error));
        }
    }
}

BOOST_AUTO_TEST_CASE(session_create_error_redacts_private_key)
{
    const auto key_path = m_args.GetDataDirNet() / "test_i2p_private_key_rejected";
    std::string key(387, 'J');
    key[385] = key[386] = '\0';
    BOOST_REQUIRE(WriteBinaryFile(key_path, key));
    CreateSock = [](int, int, int) {
        return std::make_unique<StaticContentsSock>(
            "HELLO REPLY RESULT=OK VERSION=3.1\n"
            "SESSION STATUS RESULT=I2P_ERROR PRIV=reply-private-sentinel\n");
    };

    CThreadInterrupt interrupt;
    const Proxy sam_proxy{CService{in6_addr(IN6ADDR_LOOPBACK_INIT), 7656}, false};
    i2p::sam::Session session(key_path, sam_proxy, &interrupt);
    std::string logged;
    auto callback = LogInstance().PushBackCallback([&](const std::string& line) { logged += line; });
    i2p::Connection conn;
    bool proxy_error;
    BOOST_CHECK(!session.Connect(CService{}, conn, proxy_error));
    LogInstance().DeleteCallback(callback);

    BOOST_CHECK(logged.find("SESSION CREATE ...") != std::string::npos);
    BOOST_CHECK(logged.find("SkpKSkpKSkpKSkpK") == std::string::npos);
    BOOST_CHECK(logged.find("reply-private-sentinel") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(decode_error_redacts_input)
{
    CreateSock = [](int, int, int) {
        return std::make_unique<StaticContentsSock>(
            "HELLO REPLY RESULT=OK VERSION=3.1\n"
            "SESSION STATUS RESULT=OK DESTINATION=invalid-private-sentinel!\n");
    };
    CThreadInterrupt interrupt;
    const Proxy sam_proxy{CService{in6_addr(IN6ADDR_LOOPBACK_INIT), 7656}, false};
    i2p::sam::Session session(sam_proxy, &interrupt);
    std::string logged;
    auto callback = LogInstance().PushBackCallback([&](const std::string& line) { logged += line; });
    i2p::Connection conn;
    bool proxy_error;
    BOOST_CHECK(!session.Connect(CService{}, conn, proxy_error));
    LogInstance().DeleteCallback(callback);
    BOOST_CHECK(logged.find("Cannot decode Base64") != std::string::npos);
    BOOST_CHECK(logged.find("invalid-private-sentinel!") == std::string::npos);
}

class SessionControlSock final : public StaticContentsSock
{
public:
    SessionControlSock(const std::string& contents, bool& connected) : StaticContentsSock(contents), m_connected(connected) {}
    bool IsConnected(std::string&) const override { return m_connected; }
    SessionControlSock& operator=(Sock&&) override { assert(false); return *this; }
private:
    bool& m_connected;
};

class ReplacingReplySock final : public StaticContentsSock
{
public:
    ReplacingReplySock(const std::string& contents, std::function<void()> replace)
        : StaticContentsSock(contents), m_replace(std::move(replace)) {}
    ReplacingReplySock& operator=(Sock&&) override { assert(false); return *this; }
    ssize_t Recv(void* buf, size_t len, int flags) const override
    {
        const ssize_t count = StaticContentsSock::Recv(buf, len, flags);
        if (count > 0 && !(flags & MSG_PEEK) && !m_replaced) {
            m_seen.append(static_cast<const char*>(buf), static_cast<size_t>(count));
            if (m_seen.find("STREAM STATUS RESULT=INVALID_ID\n") != std::string::npos) {
                m_replaced = true;
                m_replace();
            }
        }
        return count;
    }
private:
    const std::function<void()> m_replace;
    mutable std::string m_seen;
    mutable bool m_replaced{false};
};

BOOST_AUTO_TEST_CASE(stale_invalid_id_keeps_replacement_session)
{
    const auto key_path = m_args.GetDataDirNet() / "test_i2p_private_key_stale_id";
    std::string key(387, 'J');
    key[385] = key[386] = '\0';
    BOOST_REQUIRE(WriteBinaryFile(key_path, key));

    CThreadInterrupt interrupt;
    const Proxy sam_proxy{CService{in6_addr(IN6ADDR_LOOPBACK_INIT), 7656}, false};
    i2p::sam::Session session(key_path, sam_proxy, &interrupt);
    bool old_connected{true};
    bool new_connected{true};
    bool replacement_listened{false};
    size_t sockets{0};
    CreateSock = [&](int, int, int) -> std::unique_ptr<Sock> {
        ++sockets;
        if (sockets == 1) return std::make_unique<SessionControlSock>(
            "HELLO REPLY RESULT=OK VERSION=3.1\nSESSION STATUS RESULT=OK\n", old_connected);
        if (sockets == 2) return std::make_unique<ReplacingReplySock>(
            "HELLO REPLY RESULT=OK VERSION=3.1\n"
            "NAMING REPLY RESULT=OK VALUE=public-destination\n"
            "STREAM STATUS RESULT=INVALID_ID\n",
            [&] {
                old_connected = false;
                i2p::Connection listener;
                replacement_listened = session.Listen(listener);
            });
        if (sockets == 3) return std::make_unique<SessionControlSock>(
            "HELLO REPLY RESULT=OK VERSION=3.1\nSESSION STATUS RESULT=OK\n", new_connected);
        return std::make_unique<StaticContentsSock>(
            "HELLO REPLY RESULT=OK VERSION=3.1\nSTREAM STATUS RESULT=OK\n");
    };

    CNetAddr peer;
    BOOST_REQUIRE(peer.SetSpecial(std::string(52, 'a') + ".b32.i2p"));
    i2p::Connection conn;
    bool proxy_error;
    BOOST_CHECK(!session.Connect(CService{peer, I2P_SAM31_PORT}, conn, proxy_error));
    BOOST_CHECK(replacement_listened);
    i2p::Connection later_listener;
    BOOST_CHECK(session.Listen(later_listener));
    BOOST_CHECK_EQUAL(sockets, 5U);
}

BOOST_AUTO_TEST_SUITE_END()
