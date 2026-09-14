// Copyright (c) 2013-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <clientversion.h>
#include <key.h>
#include <key_io.h>
#include <util/chaintype.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <string>
#include <vector>

namespace {

struct TestDerivation {
    std::string pub;
    std::string prv;
    unsigned int nChild;
};

struct TestVector {
    std::string strHexMaster;
    std::vector<TestDerivation> vDerive;

    explicit TestVector(std::string strHexMasterIn) : strHexMaster(strHexMasterIn) {}

    TestVector& operator()(std::string pub, std::string prv, unsigned int nChild) {
        vDerive.emplace_back();
        TestDerivation &der = vDerive.back();
        der.pub = pub;
        der.prv = prv;
        der.nChild = nChild;
        return *this;
    }
};

TestVector test1 =
  TestVector("000102030405060708090a0b0c0d0e0f")
    ("qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP",
     "qprv7bCASBhrxHEx6L7PVYqAUQab5THvLxXiczrVJfUsDqxEbPvEH8ETVJ7EZJnpw1kKXnVKG9iksiJPtkMnKYUGtPg4p3JgDK1d9uTkTrLPCAt",
     0x80000000)
    ("qpub3rSvqQYDsuvETRKZoLEoiadRxJgm8VjafqCz59GbkNgVqUYZEu5oAEn8BqzqCD4vTXfpdLHLFeinffX4Zp3JaK8soNqAJ9RSRxC3orGZq7t",
     "qprv7dTaRu1L3YMwEwF6hJhoMSghQGrGj31jJcHPGkrzC39WxgDQhMmYcSTeLa9ALj8HEqP1dT3dG59GcWPVy8kTgTkasuypTGiWrv6bu6rdXyw",
     1)
    ("qpub3td43C67GcoJHsN1Xb8gKKX61gfs8XTsc9spfR3Qh1Ui523gxhW77ogMbvTQc9nGRFHsQkjSNFDGFyFt39DCFxxp6ag1mnRi4giq6qbfYtt",
     "qprv7fdhdgZDSFF15PHYRZbfxBaMTeqNj4k2EvxDs2do8fwjCDiYRABra1MskdLDweuKgv8hSGe4Ln6gkSuj6UYSTdF2PgwYX2VNXiQyRvJaj1u",
     0x80000002)
    ("qpub3wEL5iuxyVeiA3A6nBY3gKZEEqYPQ53rwh7VukLZrsr9UsEomTLY2FEH9weF42DammPc28i3kBnEiJQAkEkjKR7egUe9rjVPnNdyC3fwSK7",
     "qprv7iEygDP5986QwZ5dgA13KBcVgohtzcL1aUBu7MvxJYKAc4ufDv2HUSuoJfYqutQpCPA62jaymBvayeeUgVTdaiggkJW6fxjK8zAC4dLX5bz",
     2)
    ("qpub3yTivA2v9yKhEWdpUSk8ZpiYKeq82fN4PAzoAhs3BKQkqxsAqE2uDphCqGddTpNKF5WnA9BznvgMGikAqYBw31aubGCLgvPDo4YpLcMw1iG",
     "qprv7kUNWeW2KbmQ22ZMNRD8CgmomczddCeD1x5CNKTRcysmyAY2Hgieg2NiyyxJNYpFGBHneepYg1NkSqYvsuzyn7Gj9YbCE3Z2HxdMa88WiTX",
     1000000000)
    ("qpub41BVPqeAH6htkrRGazJrsXiY7tQRrAB6BRPMQzFBPPoFZ6GZMeHZUvrGU6YaK5T2imoKfV7oL8Nws99BwPgmtGk2TocRVPxrc2AjanTc6Zn",
     "qprv7nC8zL7GSj9bYNLoUxmrWPmoZrZwShTEpCTkcbqZq4GGgHwQp6yJw8XncqjWHTwKsah3S1oVX3usT1ZJQz1hu4vGV3MRLxJjaJBiKF6cd8P",
     0);

TestVector test2 =
  TestVector("fffcf9f6f3f0edeae7e4e1dedbd8d5d2cfccc9c6c3c0bdbab7b4b1aeaba8a5a29f9c999693908d8a8784817e7b7875726f6c696663605d5a5754514e4b484542")
    ("qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS",
     "qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb",
     0)
    ("qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9",
     "qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X",
     0xFFFFFFFF)
    ("qpub3tcKN1xEeZoRzZVfFxoyptLAdPEzgWANK8PiVCZXUiERuSx1By62ptx9wXj7hy8tMi2mBqFRcPRXEtaBQ2Qci5o1Gob6UQRHEG7s47MFspL",
     "qprv7fcxxWRLpCF8n5RC9wGyTkPS5MQWH3SWwuU7gp9uvNhT2ecreRmnH6dg6EKfUqsj5ioTgTnadzFpn2TUecyA1HbnhN2KWepMeML4xWpfKzF",
     1)
    ("qpub3wRHn2wApD6d6BAoKcxYnqGPxv7Vnh5ddZtaLGDrnbasaGpAVUcjioLXnKvFVqqAwtVK2ULfFWjMKgqCJvqZyRFus5nwgwrVA2npJa46BAU",
     "qprv7iRwNXQGyqYKsh6LDbRYRhKfQtH1PEMnGLxyXspFEG3thUV1wwJVB123w2XHWZ5RPgAARkry3Xw36jEdhWVY1mR5q8Z8Mknk89koSANf2gq",
     0xFFFFFFFE)
    ("qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ",
     "qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P",
     2)
    ("qpub3yxMeS62wnNa8PbHnAun96LZGi7y5zdDKSEMGmGmG9hi6pUfknUWRtedVP82pniKRPMB6tunBq5CTm45pudz12own9wXBcEdhZ4aWBcxHaK",
     "qprv7ky1EvZ97QpGuuWpg9NmmxPpigHUgXuMxDJkUNs9hpAjE29XDFAFt6L9e8vgHzYmyZJb2LGmt1MJxQDWwmmFkapyTvbwZNx6A76saYvNsFF",
     0);

TestVector test3 =
  TestVector("4b381541583be4423346c643850da4b320e46a87ae3d2a4e6da11eba819cd4acba45d239319ac14f863b8d5ab5a0d0c64d2e8a1e7d1457df2e5a3c51c73235be")
    ("qpub3pBWqhEkneoFHV9ME7wFVjW7kKbwBuffP9LNoHes2FgsjRVqexZcRFpqEg45yk6zXxNuVmH5JkyorMv4agGX4gLEav8awtTj27FJJdAbJSh",
     "qprv7bCASBhrxHEx514t86QF8bZPCHmSnSwp1vQmzuFFTv9trdAh7RFMsTWMPNJ44YXjrXuVaMsK4F5JLvhCbQ1QHEW5njwZZ6GL1skLUmY7x8y",
      0x80000000)
    ("qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF",
     "qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i",
      0);

TestVector test4 =
  TestVector("3ddd5602285899a946114506157c7997e5444528f3003f6134712147db19b678")
    ("qpub3pBWqhEkneoFKYev4rJ4JUy1NZC77A5QdH2nB2LStnXZaKKArsANdUNvvoJm1Bh47VWTeSWGFXkYfEhBztHS9YC9MLRfWCRXvhvXPQEdidY",
     "qprv7bCASBhrxHEx74aSxpm3wM2GpXMchhMZG47BNdvqLSzahWz2KKr85g4T5XaB5GvvRzs7gYkSzaH4ywKVFVGKN4AUHyobby9zJ1cVEtRXxWP",
     0x80000000)
    ("qpub3sLdE5MRAQugxRg3hBDqJt4mpV5NpTbxNaeSWzWRYEbY4rNHRCb6mX4H6PaX2p4BXbGpbUtsrTgoCAPFhfjqErFBPerdP1WuyWUaBqzh2Qc",
     "qprv7eMGpZpXL3MPjwbab9gpwk83GTEtQzt71Miqic6oyu4ZC438sfGrDijoF5VR867HPytZD7vgK2P9vbWarrvxNh3Q9N7GotbzHGeHyp4FTA9",
     0x80000001)
    ("qpub3uUJt4kRg8xHdaXgqT8jQkeczRFTKPJm2WhGHk2dCRgyqwgoHh4oze29zLM3szgs7NtBVFpzJv2nAeiAAGYyx9BYjShgLjzrqvqUFSg9UmK",
     "qprv7gUxUZDXqmPzR6TDjRbj3chtSPQxuvaufHmfVMd1e69zy9Mek9kZSqhg93TWBV97UkHfPike5fV2dZLJFnt5f5WdPhpHuvrK8xBz5RJdWKn",
     0);

const std::vector<std::string> TEST5 = {
    "qpub3pBWqhEkneoFHU6K6c1xysyifQWqWp3xaVMkf9wSJ7vdM5yua2D7eFHVwtUiBmW9BjDLMohkhvzDpzsDukfV5bCuhR7D6WFf3F6ZwP8rpuN",
    "qprv7bCASBhrxHEx4z1qzaUxck2z7NgM7ML7DGS9rmXpjnPeUHem2Uts6Sy26pkJQrUBLaTNxk6GWVSfjS7dYK1z3JvwcCnn1zWdvhkEB4vif9W",
    "qpub3pBWqhEkneoFHU6K6c1xysyifQWqWp3xaVMkf9wSJ7vdM5yua2D7eFHVx2FgthJGbRvEu62aLFUDeHGKaD8MgQaQqyPe571F1s87uv2d2mF",
    "qprv7bCASBhrxHEx4z1qzaUxck2z7NgM7ML7DGS9rmXpjnPeUHem2Uts6Sy26q7QoX9gBhUEzduefVim2qiXgYdovYzFKy5GDnCC61Et9rVXubv",
    "qpub3pBWqhEkneoFHU6K6c1xysyifQWqWp3xaVMkf9wSJ7vdM5yua2D7eFHVwvRTMzxB39tZVdHTcWcDnKTzpsHCjJ3nV4BKLewZ2u6xS2FZ4xE",
    "qprv7bCASBhrxHEx4z1qzaUxck2z7NgM7ML7DGS9rmXpjnPeUHem2Uts6Sy26jHBGpoadRSZbBAXwkrmAsvCwCneySTcy3rwVL8W73DifyZVhwB",
    "qprv7bCbFvBxMNhF9C7meqk1oBTDfoYqre4FhYVEZ9a7sG1cfcnB9oDXYu65XFrFCSgbQajrm2rqMGoPCXn6othUQYWfdRXpHHKs29u3wpZ4TQt",
    "qpub3pBwfRirBkFYMgCEksH2AKPxDqPLG6n74mQqMXyjRbYbYR7KhLXn6hQZNaQPbswpNtsuAsVCVLkkKr86Dn34syR2aDr8uRCwmLKBhXcdZxE",
    "qprv7bCASBhs25G6ve178q2TtXibYEHueCkfby6fdCeeF8SYfSTsMi67g91FptLjss22HtwvXmBx3ozhNEaxmmVgAM5xyGNMM6WH9CTVe5T5Lfz",
    "qpub3pBWqhEkrSpQ985aErZUFffL6G8Q3fUWyC2GRb4FoTyXYEo1uFQNDwKjgCttHJHFGD5xwbpKBsx4VYvxBeqGdmzKv4gfyEPMtNsdPoS4dDM",
    "DMwo58pR1QLEFihHiXPVykYB6fJmsTeHvyTp7hRThAtCX8CvYzgPcn8XnmdfHGMQzT7ayAmfo4z3gY5KfbrZWZ6St24UVf2Qgo6oujFktLHdHY4",
    "DMwo58pR1QLEFihHiXPVykYB6fJmsTeHvyTp7hRThAtCX8CvYzgPcn8XnmdfHPmHJiEDXkTiJTVV9rHEBUem2mwVbbNfvT2MTcAqj3nesx8uBf9",
    "qprv7bCASBhrxHEx4z1qzaUxck2z7NgM7ML7DGS9rmXpjnPeUHem2Uts6Sy26hLS6bMYmzmLTMaq3BEmDZKS26AwKjckBQnqFBSc7PDLA5NktWA",
    "qprv7bCASBhrxHEx4z1qzaUxck2z7NgM7ML7DGS9rmXpjnPeUHem2Uts6Sy26jHBGpoadRSZbBAXwkrmAsv8QLRkSRCqWLbxEURBQkCLtH1zqYC",
    "qpub3pBWqhEkneoFHU6K6c1xysyifQWqWp3xaVMkf9wSJ7vdM5yua2D7eFHVwxNCYEQCtaZndSsAX6EDje4mjytvNztfGhFRaodT2Z7LvAMaYGC",
    "qprv7bCASBhrxHEx6L7PVYqAUQab5THvLxXiczrVJfUsDqxEbPvEH8ETVJ7EZJnpw1kKXnVKG9iksiJPtkMnKYUGtPg4p3JgDK1d9uTkTw89Y7t"
};

void RunTest(const TestVector& test)
{
    std::vector<std::byte> seed{ParseHex<std::byte>(test.strHexMaster)};
    CExtKey key;
    CExtPubKey pubkey;
    key.SetSeed(seed);
    pubkey = key.Neuter();
    for (const TestDerivation &derive : test.vDerive) {
        unsigned char data[74];
        key.Encode(data);
        pubkey.Encode(data);

        // Test private key
        BOOST_CHECK(EncodeExtKey(key) == derive.prv);
        BOOST_CHECK(DecodeExtKey(derive.prv) == key); //ensure a base58 decoded key also matches

        // Test public key
        BOOST_CHECK(EncodeExtPubKey(pubkey) == derive.pub);
        BOOST_CHECK(DecodeExtPubKey(derive.pub) == pubkey); //ensure a base58 decoded pubkey also matches

        // Derive new keys
        CExtKey keyNew;
        BOOST_CHECK(key.Derive(keyNew, derive.nChild));
        CExtPubKey pubkeyNew = keyNew.Neuter();
        if (!(derive.nChild & 0x80000000)) {
            // Compare with public derivation
            CExtPubKey pubkeyNew2;
            BOOST_CHECK(pubkey.Derive(pubkeyNew2, derive.nChild));
            BOOST_CHECK(pubkeyNew == pubkeyNew2);
        }
        key = keyNew;
        pubkey = pubkeyNew;
    }
}

}  // namespace

BOOST_FIXTURE_TEST_SUITE(bip32_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(bip32_test1) {
    RunTest(test1);
}

BOOST_AUTO_TEST_CASE(bip32_test2) {
    RunTest(test2);
}

BOOST_AUTO_TEST_CASE(bip32_test3) {
    RunTest(test3);
}

BOOST_AUTO_TEST_CASE(bip32_test4) {
    RunTest(test4);
}

BOOST_AUTO_TEST_CASE(bip32_test5) {
    for (const auto& str : TEST5) {
        auto dec_extkey = DecodeExtKey(str);
        auto dec_extpubkey = DecodeExtPubKey(str);
        BOOST_CHECK_MESSAGE(!dec_extkey.key.IsValid(), "Decoding '" + str + "' as xprv should fail");
        BOOST_CHECK_MESSAGE(!dec_extpubkey.pubkey.IsValid(), "Decoding '" + str + "' as xpub should fail");
    }
}

BOOST_AUTO_TEST_CASE(bip32_max_depth) {
    CExtKey key_parent{DecodeExtKey(test1.vDerive[0].prv)}, key_child;
    CExtPubKey pubkey_parent{DecodeExtPubKey(test1.vDerive[0].pub)}, pubkey_child;

    // We can derive up to the 255th depth..
    for (auto i = 0; i++ < 255;) {
        BOOST_CHECK(key_parent.Derive(key_child, 0));
        std::swap(key_parent, key_child);
        BOOST_CHECK(pubkey_parent.Derive(pubkey_child, 0));
        std::swap(pubkey_parent, pubkey_child);
    }

    // But trying to derive a non-existent 256th depth will fail!
    BOOST_CHECK(key_parent.nDepth == 255 && pubkey_parent.nDepth == 255);
    BOOST_CHECK(!key_parent.Derive(key_child, 0));
    BOOST_CHECK(!pubkey_parent.Derive(pubkey_child, 0));
}

BOOST_AUTO_TEST_CASE(quicksilver_extkey_prefixes)
{
    // Deterministic master from a fixed seed; check the branded base58 prefix.
    const std::vector<std::byte> seed(32, std::byte{0x2a});
    CExtKey key; key.SetSeed(seed);
    CExtPubKey pub = key.Neuter();
    SelectParams(ChainType::MAIN);
    BOOST_CHECK(EncodeExtKey(key).rfind("qprv", 0) == 0);
    BOOST_CHECK(EncodeExtPubKey(pub).rfind("qpub", 0) == 0);
    SelectParams(ChainType::PUBLIC_TEST);
    BOOST_CHECK(EncodeExtKey(key).rfind("pqrv", 0) == 0);
    BOOST_CHECK(EncodeExtPubKey(pub).rfind("pqub", 0) == 0);
    SelectParams(ChainType::SANDBOX);
    BOOST_CHECK(EncodeExtKey(key).rfind("sqrv", 0) == 0);
    BOOST_CHECK(EncodeExtPubKey(pub).rfind("squb", 0) == 0);
    SelectParams(ChainType::MAIN); // restore
}

BOOST_AUTO_TEST_SUITE_END()
