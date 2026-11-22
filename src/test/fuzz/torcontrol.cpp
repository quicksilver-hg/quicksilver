// Copyright (c) 2020-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/setup_common.h>
#include <torcontrol.h>

#include <cstdint>
#include <string>
#include <vector>

void initialize_torcontrol()
{
    static const auto testing_setup = MakeNoLogFileContext<>();
}

FUZZ_TARGET(torcontrol, .init = initialize_torcontrol)
{
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};

    TorController tor_controller;
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 10000) {
        TorControlReply tor_control_reply;
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                tor_control_reply.code = 250;
            },
            [&] {
                tor_control_reply.code = 510;
            },
            [&] {
                tor_control_reply.code = fuzzed_data_provider.ConsumeIntegral<int>();
            });
        tor_control_reply.lines = ConsumeRandomLengthStringVector(fuzzed_data_provider);
        if (tor_control_reply.lines.empty()) {
            break;
        }
        // Command() on this disconnected connection returns false. The methods
        // are nonvirtual, so a derived no-op connection cannot intercept them.
        TorControlConnection dummy_tor_control_connection{nullptr};
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                tor_controller.add_onion_cb(dummy_tor_control_connection, tor_control_reply);
            },
            [&] {
                tor_controller.auth_cb(dummy_tor_control_connection, tor_control_reply);
            },
            [&] {
                tor_controller.authchallenge_cb(dummy_tor_control_connection, tor_control_reply);
            },
            [&] {
                tor_controller.protocolinfo_cb(dummy_tor_control_connection, tor_control_reply);
            });
    }
}
