// Copyright (c) The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <i2p.h>
#include <netaddress.h>
#include <netbase.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/net.h>
#include <test/util/setup_common.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/threadinterrupt.h>

#include <cassert>
#include <cstdint>
#include <vector>

void initialize_i2p()
{
    static const auto testing_setup = MakeNoLogFileContext<>();
}

FUZZ_TARGET(i2p, .init = initialize_i2p)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};

    SetMockTime(ConsumeTime(fuzzed_data_provider));

    // Mock CreateSock() to create FuzzedSock.
    auto CreateSockOrig = CreateSock;
    CreateSock = [&fuzzed_data_provider](int, int, int) {
        return std::make_unique<FuzzedSock>(fuzzed_data_provider);
    };

    const fs::path private_key_path = gArgs.GetDataDirNet() / "fuzzed_i2p_private_key";
    const CService addr{in6_addr(IN6ADDR_LOOPBACK_INIT), 7656};
    const Proxy sam_proxy{addr, false};
    CThreadInterrupt interrupt;

    i2p::sam::Session session{private_key_path, sam_proxy, &interrupt};
    i2p::Connection conn;

    if (session.Listen(conn)) {
        if (session.Accept(conn)) {
            try {
                (void)conn.sock->RecvUntilTerminator('\n', 10ms, interrupt, i2p::sam::MAX_MSG_SIZE);
            } catch (const std::runtime_error&) {
            }
        }
    }

    bool proxy_error;

    // S0396-A-01: connect to a production-like destination, a .b32.i2p address
    // (52 base32 characters of a 32-byte hash), not CService{}. SAM 3.1 has no
    // ports: any port but I2P_SAM31_PORT is refused before the proxy is asked.
    const std::vector<uint8_t> dest_hash{ConsumeFixedLengthByteVector(fuzzed_data_provider, 32)};
    CNetAddr dest_addr;
    assert(dest_addr.SetSpecial(EncodeBase32(dest_hash, /*pad=*/false) + ".b32.i2p"));
    assert(dest_addr.IsI2P());
    const uint16_t dest_port{fuzzed_data_provider.ConsumeBool() ? I2P_SAM31_PORT : fuzzed_data_provider.ConsumeIntegral<uint16_t>()};
    const CService dest{dest_addr, dest_port};
    const bool connected{session.Connect(dest, conn, proxy_error)};
    if (dest_port != I2P_SAM31_PORT) {
        assert(!connected);
        assert(!proxy_error);
    }
    if (connected) {
        assert(conn.peer == dest);
        try {
            conn.sock->SendComplete("verack\n", 10ms, interrupt);
        } catch (const std::runtime_error&) {
        }
    }

    fs::remove_all(private_key_path);

    CreateSock = CreateSockOrig;
}
