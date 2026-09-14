// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <outputtype.h>
#include <pubkey.h>
#include <script/descriptor.h>
#include <script/sign.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace util::hex_literals;
using util::Split;

namespace {

void CheckUnparsable(const std::string& prv, const std::string& pub, const std::string& expected_error)
{
    FlatSigningProvider keys_priv, keys_pub;
    std::string error;
    auto parse_priv = Parse(prv, keys_priv, error);
    auto parse_pub = Parse(pub, keys_pub, error);
    BOOST_CHECK_MESSAGE(parse_priv.empty(), prv);
    BOOST_CHECK_MESSAGE(parse_pub.empty(), pub);
    BOOST_CHECK_EQUAL(error, expected_error);
}

/** Check that the script is inferred as non-standard */
void CheckInferRaw(const CScript& script)
{
    FlatSigningProvider dummy_provider;
    std::unique_ptr<Descriptor> desc = InferDescriptor(script, dummy_provider);
    BOOST_CHECK(desc->ToString().rfind("raw(", 0) == 0);
}

constexpr int DEFAULT = 0;
constexpr int RANGE = 1; // Expected to be ranged descriptor
constexpr int HARDENED = 2; // Derivation needs access to private keys
constexpr int UNSOLVABLE = 4; // This descriptor is not expected to be solvable
constexpr int SIGNABLE = 8; // We can sign with this descriptor (this is not true when actual BIP32 derivation is used, as that's not integrated in our signing code)
constexpr int DERIVE_HARDENED = 16; // The final derivation is hardened, i.e. ends with *' or *h
constexpr int MIXED_PUBKEYS = 32;
constexpr int XONLY_KEYS = 64; // X-only pubkeys are in use (and thus inferring/caching may swap parity of pubkeys/keyids)
constexpr int MISSING_PRIVKEYS = 128; // Not all private keys are available, so ToPrivateString will fail.
constexpr int SIGNABLE_FAILS = 256; // We can sign with this descriptor, but actually trying to sign will fail

/** Compare two descriptors. If only one of them has a checksum, the checksum is ignored. */
bool EqualDescriptor(std::string a, std::string b)
{
    bool a_check = (a.size() > 9 && a[a.size() - 9] == '#');
    bool b_check = (b.size() > 9 && b[b.size() - 9] == '#');
    if (a_check != b_check) {
        if (a_check) a = a.substr(0, a.size() - 9);
        if (b_check) b = b.substr(0, b.size() - 9);
    }
    return a == b;
}

bool EqualSigningProviders(const FlatSigningProvider& a, const FlatSigningProvider& b)
{
    return a.scripts == b.scripts
        && a.pubkeys == b.pubkeys
        && a.origins == b.origins
        && a.keys == b.keys
        && a.tr_trees == b.tr_trees;
}

std::string UseHInsteadOfApostrophe(const std::string& desc)
{
    std::string ret = desc;
    while (true) {
        auto it = ret.find('\'');
        if (it == std::string::npos) break;
        ret[it] = 'h';
    }

    // GetDescriptorChecksum returns "" if the checksum exists but is bad.
    // Switching apostrophes with 'h' breaks the checksum if it exists - recalculate it and replace the broken one.
    if (GetDescriptorChecksum(ret) == "") {
        ret = ret.substr(0, desc.size() - 9);
        ret += std::string("#") + GetDescriptorChecksum(ret);
    }
    return ret;
}

// Count the number of times an extended pubkey ("qpub" prefix on mainnet) appears in a descriptor string
static size_t CountXpubs(const std::string& desc)
{
    size_t count = 0;
    size_t p = desc.find("qpub", 0);
    while (p != std::string::npos) {
        count++;
        p = desc.find("qpub", p + 1);
    }
    return count;
}

const std::set<std::vector<uint32_t>> ONLY_EMPTY{{}};

std::set<CPubKey> GetKeyData(const FlatSigningProvider& provider, int flags) {
    std::set<CPubKey> ret;
    for (const auto& [_, pubkey] : provider.pubkeys) {
        if (flags & XONLY_KEYS) {
            unsigned char bytes[33];
            BOOST_CHECK_EQUAL(pubkey.size(), 33);
            std::copy(pubkey.begin(), pubkey.end(), bytes);
            bytes[0] = 0x02;
            CPubKey norm_pubkey{bytes};
            ret.insert(norm_pubkey);
        } else {
            ret.insert(pubkey);
        }
    }
    return ret;
}

std::set<std::pair<CPubKey, KeyOriginInfo>> GetKeyOriginData(const FlatSigningProvider& provider, int flags) {
    std::set<std::pair<CPubKey, KeyOriginInfo>> ret;
    for (const auto& [_, data] : provider.origins) {
        if (flags & XONLY_KEYS) {
            unsigned char bytes[33];
            BOOST_CHECK_EQUAL(data.first.size(), 33);
            std::copy(data.first.begin(), data.first.end(), bytes);
            bytes[0] = 0x02;
            CPubKey norm_pubkey{bytes};
            KeyOriginInfo norm_origin = data.second;
            std::fill(std::begin(norm_origin.fingerprint), std::end(norm_origin.fingerprint), 0); // fingerprints don't necessarily match.
            ret.emplace(norm_pubkey, norm_origin);
        } else {
            ret.insert(data);
        }
    }
    return ret;
}

void DoCheck(std::string prv, std::string pub, const std::string& norm_pub, int flags,
             const std::vector<std::vector<std::string>>& scripts, const std::optional<OutputType>& type, std::optional<uint256> op_desc_id = std::nullopt,
             const std::set<std::vector<uint32_t>>& paths = ONLY_EMPTY, bool replace_apostrophe_with_h_in_prv=false,
             bool replace_apostrophe_with_h_in_pub=false, uint32_t spender_nlocktime=0, uint32_t spender_nsequence=CTxIn::SEQUENCE_FINAL,
             std::map<std::vector<uint8_t>, std::vector<uint8_t>> preimages={},
             std::optional<std::string> expected_prv = std::nullopt, std::optional<std::string> expected_pub = std::nullopt, int desc_index = 0)
{
    FlatSigningProvider keys_priv, keys_pub;
    std::set<std::vector<uint32_t>> left_paths = paths;
    std::string error;

    std::vector<std::unique_ptr<Descriptor>> parse_privs;
    std::vector<std::unique_ptr<Descriptor>> parse_pubs;
    // Check that parsing succeeds.
    if (replace_apostrophe_with_h_in_prv) {
        prv = UseHInsteadOfApostrophe(prv);
    }
    parse_privs = Parse(prv, keys_priv, error);
    BOOST_CHECK_MESSAGE(!parse_privs.empty(), error);
    if (replace_apostrophe_with_h_in_pub) {
        pub = UseHInsteadOfApostrophe(pub);
    }
    parse_pubs = Parse(pub, keys_pub, error);
    BOOST_CHECK_MESSAGE(!parse_pubs.empty(), error);

    auto& parse_priv = parse_privs.at(desc_index);
    auto& parse_pub = parse_pubs.at(desc_index);

    // We must be able to estimate the max satisfaction size for any solvable descriptor top descriptor (but combo).
    const bool is_nontop_or_nonsolvable{!parse_priv->IsSolvable() || !parse_priv->GetOutputType()};
    const auto max_sat_maxsig{parse_priv->MaxSatisfactionWeight(true)};
    const auto max_sat_nonmaxsig{parse_priv->MaxSatisfactionWeight(false)};
    BOOST_CHECK(max_sat_nonmaxsig <= max_sat_maxsig);
    const auto max_elems{parse_priv->MaxSatisfactionElems()};
    const bool is_input_size_info_set{max_sat_maxsig && max_sat_nonmaxsig && max_elems};
    BOOST_CHECK_MESSAGE(is_input_size_info_set || is_nontop_or_nonsolvable, prv);

    // The ScriptSize() must match the size of the Script string. (ScriptSize() is set for all descs but 'combo()'.)
    const bool is_combo{!parse_priv->IsSingleType()};
    BOOST_CHECK_MESSAGE(is_combo || parse_priv->ScriptSize() == scripts[0][0].size() / 2, "Invalid ScriptSize() for " + prv);

    // Check that the correct OutputType is inferred
    BOOST_CHECK(parse_priv->GetOutputType() == type);
    BOOST_CHECK(parse_pub->GetOutputType() == type);

    // Check private keys are extracted from the private version but not the public one.
    BOOST_CHECK(keys_priv.keys.size());
    BOOST_CHECK(!keys_pub.keys.size());

    // If expected_pub is provided, check that the serialize matches that.
    // Otherwise check that they serialize back to the public version.
    std::string pub1 = parse_priv->ToString();
    std::string pub2 = parse_pub->ToString();
    if (expected_pub) {
        BOOST_CHECK_MESSAGE(EqualDescriptor(*expected_pub, pub1), "Private ser: " + pub1 + " Public desc: " + *expected_pub);
        BOOST_CHECK_MESSAGE(EqualDescriptor(*expected_pub, pub2), "Public ser: " + pub2 + " Public desc: " + *expected_pub);
    } else {
        BOOST_CHECK_MESSAGE(EqualDescriptor(pub, pub1), "Private ser: " + pub1 + " Public desc: " + pub);
        BOOST_CHECK_MESSAGE(EqualDescriptor(pub, pub2), "Public ser: " + pub2 + " Public desc: " + pub);
    }

    // Check that the COMPAT identifier did not change
    if (op_desc_id) {
        BOOST_CHECK_MESSAGE(DescriptorID(*parse_priv) == *op_desc_id, "DescriptorID() " + DescriptorID(*parse_priv).ToString() + " does not match for priv " + prv);
    }

    // Check that both can be serialized with private key back to the private version, but not without private key.
    if (!(flags & MISSING_PRIVKEYS)) {
        std::string prv1;
        BOOST_CHECK(parse_priv->ToPrivateString(keys_priv, prv1));
        if (expected_prv) {
            BOOST_CHECK_MESSAGE(EqualDescriptor(*expected_prv, prv1), "Private ser: " + prv1 + "Private desc: " + *expected_prv);
        } else {
            BOOST_CHECK_MESSAGE(EqualDescriptor(prv, prv1), "Private ser: " + prv1 + " Private desc: " + prv);
        }
        BOOST_CHECK(!parse_priv->ToPrivateString(keys_pub, prv1));
        BOOST_CHECK(parse_pub->ToPrivateString(keys_priv, prv1));
        if (expected_prv) {
            BOOST_CHECK(EqualDescriptor(*expected_prv, prv1));
            BOOST_CHECK_MESSAGE(EqualDescriptor(*expected_prv, prv1), "Private ser: " + prv1 + " Private desc: " + *expected_prv);
        } else {
            BOOST_CHECK_MESSAGE(EqualDescriptor(prv, prv1), "Private ser: " + prv1 + " Private desc: " + prv);
        }
        BOOST_CHECK(!parse_pub->ToPrivateString(keys_pub, prv1));

        // Check that both can ExpandPrivate and get the same SigningProviders
        FlatSigningProvider priv_prov;
        parse_priv->ExpandPrivate(0, keys_priv, priv_prov);

        FlatSigningProvider pub_prov;
        parse_pub->ExpandPrivate(0, keys_priv, pub_prov);

        BOOST_CHECK_MESSAGE(EqualSigningProviders(priv_prov, pub_prov), "Private desc: " + prv + " Pub desc: " + pub);
    }

    // Check that private can produce the normalized descriptors
    std::string norm1;
    BOOST_CHECK(parse_priv->ToNormalizedString(keys_priv, norm1));
    BOOST_CHECK_MESSAGE(EqualDescriptor(norm1, norm_pub), "priv->ToNormalizedString(): " + norm1 + " Norm. desc: " + norm_pub);
    BOOST_CHECK(parse_pub->ToNormalizedString(keys_priv, norm1));
    BOOST_CHECK_MESSAGE(EqualDescriptor(norm1, norm_pub), "pub->ToNormalizedString(): " + norm1 + " Norm. desc: " + norm_pub);

    // Check whether IsRange on both returns the expected result
    BOOST_CHECK_EQUAL(parse_pub->IsRange(), (flags & RANGE) != 0);
    BOOST_CHECK_EQUAL(parse_priv->IsRange(), (flags & RANGE) != 0);

    // * For ranged descriptors,  the `scripts` parameter is a list of expected result outputs, for subsequent
    //   positions to evaluate the descriptors on (so the first element of `scripts` is for evaluating the
    //   descriptor at 0; the second at 1; and so on). To verify this, we evaluate the descriptors once for
    //   each element in `scripts`.
    // * For non-ranged descriptors, we evaluate the descriptors at positions 0, 1, and 2, but expect the
    //   same result in each case, namely the first element of `scripts`. Because of that, the size of
    //   `scripts` must be one in that case.
    if (!(flags & RANGE)) assert(scripts.size() == 1);
    size_t max = (flags & RANGE) ? scripts.size() : 3;

    // Iterate over the position we'll evaluate the descriptors in.
    for (size_t i = 0; i < max; ++i) {
        // Call the expected result scripts `ref`.
        const auto& ref = scripts[(flags & RANGE) ? i : 0];
        // When t=0, evaluate the `prv` descriptor; when t=1, evaluate the `pub` descriptor.
        for (int t = 0; t < 2; ++t) {
            // When the descriptor is hardened, evaluate with access to the private keys inside.
            const FlatSigningProvider& key_provider = (flags & HARDENED) ? keys_priv : keys_pub;

            // Evaluate the descriptor selected by `t` in position `i`.
            FlatSigningProvider script_provider, script_provider_cached;
            std::vector<CScript> spks, spks_cached;
            DescriptorCache desc_cache;
            BOOST_CHECK((t ? parse_priv : parse_pub)->Expand(i, key_provider, spks, script_provider, &desc_cache));

            // Compare the output with the expected result.
            BOOST_CHECK_EQUAL(spks.size(), ref.size());

            // Try to expand again using cached data, and compare.
            BOOST_CHECK(parse_pub->ExpandFromCache(i, desc_cache, spks_cached, script_provider_cached));
            BOOST_CHECK(spks == spks_cached);
            BOOST_CHECK(GetKeyData(script_provider, flags) == GetKeyData(script_provider_cached, flags));
            BOOST_CHECK(script_provider.scripts == script_provider_cached.scripts);
            BOOST_CHECK(GetKeyOriginData(script_provider, flags) == GetKeyOriginData(script_provider_cached, flags));

            // Check whether keys are in the cache
            const auto& der_xpub_cache = desc_cache.GetCachedDerivedExtPubKeys();
            const auto& parent_xpub_cache = desc_cache.GetCachedParentExtPubKeys();
            const size_t num_xpubs = CountXpubs(pub1);
            if ((flags & RANGE) && !(flags & (DERIVE_HARDENED))) {
                // For ranged, unhardened derivation, None of the keys in origins should appear in the cache but the cache should have parent keys
                // But we can derive one level from each of those parent keys and find them all
                BOOST_CHECK(der_xpub_cache.empty());
                BOOST_CHECK(parent_xpub_cache.size() > 0);
                std::set<CPubKey> pubkeys;
                for (const auto& xpub_pair : parent_xpub_cache) {
                    const CExtPubKey& xpub = xpub_pair.second;
                    CExtPubKey der;
                    BOOST_CHECK(xpub.Derive(der, i));
                    pubkeys.insert(der.pubkey);
                }
                int count_pks = 0;
                for (const auto& origin_pair : script_provider_cached.origins) {
                    const CPubKey& pk = origin_pair.second.first;
                    count_pks += pubkeys.count(pk);
                }
                if (flags & MIXED_PUBKEYS) {
                    BOOST_CHECK_EQUAL(num_xpubs, count_pks);
                } else {
                    BOOST_CHECK_EQUAL(script_provider_cached.origins.size(), count_pks);
                }
            } else if (num_xpubs > 0) {
                // For ranged, hardened derivation, or not ranged, but has an xpub, all of the keys should appear in the cache
                BOOST_CHECK(der_xpub_cache.size() + parent_xpub_cache.size() == num_xpubs);
                if (!(flags & MIXED_PUBKEYS)) {
                    BOOST_CHECK(num_xpubs == script_provider_cached.origins.size());
                }
                // Get all of the derived pubkeys
                std::set<CPubKey> pubkeys;
                for (const auto& xpub_map_pair : der_xpub_cache) {
                    for (const auto& xpub_pair : xpub_map_pair.second) {
                        const CExtPubKey& xpub = xpub_pair.second;
                        pubkeys.insert(xpub.pubkey);
                    }
                }
                // Derive one level from all of the parents
                for (const auto& xpub_pair : parent_xpub_cache) {
                    const CExtPubKey& xpub = xpub_pair.second;
                    pubkeys.insert(xpub.pubkey);
                    CExtPubKey der;
                    BOOST_CHECK(xpub.Derive(der, i));
                    pubkeys.insert(der.pubkey);
                }
                int count_pks = 0;
                for (const auto& origin_pair : script_provider_cached.origins) {
                    const CPubKey& pk = origin_pair.second.first;
                    count_pks += pubkeys.count(pk);
                }
                if (flags & MIXED_PUBKEYS) {
                    BOOST_CHECK_EQUAL(num_xpubs, count_pks);
                } else {
                    BOOST_CHECK_EQUAL(script_provider_cached.origins.size(), count_pks);
                }
            } else if (!(flags & MIXED_PUBKEYS)) {
                // Only const pubkeys, nothing should be cached
                BOOST_CHECK(der_xpub_cache.empty());
                BOOST_CHECK(parent_xpub_cache.empty());
            }

            // Make sure we can expand using cached xpubs for unhardened derivation
            if (!(flags & DERIVE_HARDENED)) {
                // Evaluate the descriptor at i + 1
                FlatSigningProvider script_provider1, script_provider_cached1;
                std::vector<CScript> spks1, spk1_from_cache;
                BOOST_CHECK((t ? parse_priv : parse_pub)->Expand(i + 1, key_provider, spks1, script_provider1, nullptr));

                // Try again but use the cache from expanding i. That cache won't have the pubkeys for i + 1, but will have the parent xpub for derivation.
                BOOST_CHECK(parse_pub->ExpandFromCache(i + 1, desc_cache, spk1_from_cache, script_provider_cached1));
                BOOST_CHECK(spks1 == spk1_from_cache);
                BOOST_CHECK(GetKeyData(script_provider1, flags) == GetKeyData(script_provider_cached1, flags));
                BOOST_CHECK(script_provider1.scripts == script_provider_cached1.scripts);
                BOOST_CHECK(GetKeyOriginData(script_provider1, flags) == GetKeyOriginData(script_provider_cached1, flags));
            }

            // For each of the produced scripts, verify solvability, and when possible, try to sign a transaction spending it.
            for (size_t n = 0; n < spks.size(); ++n) {
                BOOST_CHECK_EQUAL(ref[n], HexStr(spks[n]));

                if (flags & (SIGNABLE | SIGNABLE_FAILS)) {
                    CMutableTransaction spend;
                    spend.nLockTime = spender_nlocktime;
                    spend.vin.resize(1);
                    spend.vin[0].nSequence = spender_nsequence;
                    spend.vout.resize(1);
                    std::vector<CTxOut> utxos(1);
                    PrecomputedTransactionData txdata;
                    txdata.Init(spend, std::move(utxos), /*force=*/true);
                    MutableTransactionSignatureCreator creator{spend, 0, CAmount{0}, &txdata, SIGHASH_DEFAULT};
                    SignatureData sigdata;
                    // We assume there is no collision between the hashes (eg h1=SHA256(SHA256(x)) and h2=SHA256(x))
                    sigdata.sha256_preimages = preimages;
                    sigdata.hash256_preimages = preimages;
                    sigdata.ripemd160_preimages = preimages;
                    sigdata.hash160_preimages = preimages;
                    const auto prod_sig_res = ProduceSignature(FlatSigningProvider{keys_priv}.Merge(FlatSigningProvider{script_provider}), creator, spks[n], sigdata);
                    BOOST_CHECK_MESSAGE(prod_sig_res == !(flags & SIGNABLE_FAILS), prv);
                }

                /* Infer a descriptor from the generated script, and verify its solvability and that it roundtrips. */
                auto inferred = InferDescriptor(spks[n], script_provider);
                BOOST_CHECK_EQUAL(inferred->IsSolvable(), !(flags & UNSOLVABLE));
                std::vector<CScript> spks_inferred;
                FlatSigningProvider provider_inferred;
                BOOST_CHECK(inferred->Expand(0, provider_inferred, spks_inferred, provider_inferred));
                BOOST_CHECK_EQUAL(spks_inferred.size(), 1U);
                BOOST_CHECK(spks_inferred[0] == spks[n]);
                BOOST_CHECK_EQUAL(InferDescriptor(spks_inferred[0], provider_inferred)->IsSolvable(), !(flags & UNSOLVABLE));
                BOOST_CHECK(GetKeyOriginData(provider_inferred, flags) == GetKeyOriginData(script_provider, flags));
            }

            // Test whether the observed key path is present in the 'paths' variable (which contains expected, unobserved paths),
            // and then remove it from that set.
            for (const auto& origin : script_provider.origins) {
                BOOST_CHECK_MESSAGE(paths.count(origin.second.second.path), "Unexpected key path: " + prv);
                left_paths.erase(origin.second.second.path);
            }
        }
    }

    // Verify no expected paths remain that were not observed.
    BOOST_CHECK_MESSAGE(left_paths.empty(), "Not all expected key paths found: " + prv);
}

void Check(const std::string& prv, const std::string& pub, const std::string& norm_pub, int flags,
           const std::vector<std::vector<std::string>>& scripts, const std::optional<OutputType>& type, std::optional<uint256> op_desc_id = std::nullopt,
           const std::set<std::vector<uint32_t>>& paths = ONLY_EMPTY, uint32_t spender_nlocktime=0,
           uint32_t spender_nsequence=CTxIn::SEQUENCE_FINAL, std::map<std::vector<uint8_t>, std::vector<uint8_t>> preimages={},
           std::optional<std::string> expected_prv = std::nullopt, std::optional<std::string> expected_pub = std::nullopt, int desc_index = 0)
{
    // Do not replace apostrophes with 'h' in prv and pub
    DoCheck(prv, pub, norm_pub, flags, scripts, type, op_desc_id, paths, /*replace_apostrophe_with_h_in_prv=*/false,
            /*replace_apostrophe_with_h_in_pub=*/false, /*spender_nlocktime=*/spender_nlocktime,
            /*spender_nsequence=*/spender_nsequence, /*preimages=*/preimages,
            expected_prv, expected_pub, desc_index);

    // Replace apostrophes with 'h' both in prv and in pub, if apostrophes are found in both
    if (prv.find('\'') != std::string::npos && pub.find('\'') != std::string::npos) {
        DoCheck(prv, pub, norm_pub, flags, scripts, type, op_desc_id, paths, /*replace_apostrophe_with_h_in_prv=*/true,
                /*replace_apostrophe_with_h_in_pub=*/true, /*spender_nlocktime=*/spender_nlocktime,
                /*spender_nsequence=*/spender_nsequence, /*preimages=*/preimages,
                expected_prv, expected_pub, desc_index);
    }
}

void CheckMultipath(const std::string& prv,
        const std::string& pub,
        const std::vector<std::string>& expanded_prvs,
        const std::vector<std::string>& expanded_pubs,
        const std::vector<std::string>& expanded_norm_pubs,
        int flags,
        const std::vector<std::vector<std::vector<std::string>>>& scripts,
        const std::optional<OutputType>& type,
        const std::vector<std::set<std::vector<uint32_t>>>& paths)
{
    assert(expanded_prvs.size() == expanded_pubs.size());
    assert(expanded_prvs.size() == expanded_norm_pubs.size());
    assert(expanded_prvs.size() == scripts.size());
    assert(expanded_prvs.size() == paths.size());
    for (size_t i = 0; i < expanded_prvs.size(); ++i) {
        Check(prv, pub, expanded_norm_pubs.at(i), flags, scripts.at(i), type, std::nullopt, paths.at(i),
              /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, /*preimages=*/{},
              expanded_prvs.at(i), expanded_pubs.at(i), i);
    }

    // The descriptor for each path must be standalone. They should not share common references. Test this
    // by parsing a multipath descriptor expression, deallocating all but one of the descriptors and making
    // sure we can perform operations on it.
    FlatSigningProvider prov, out;
    std::string error;
    const auto desc{[&](){
        auto parsed{Parse(pub, prov, error)};
        assert(parsed.size() > 1);
        return std::move(parsed.at(0));
    }()};
    desc->ToString();
    std::vector<CScript> out_scripts;
    desc->Expand(0, prov, out_scripts, out);
}

void CheckInferDescriptor(const std::string& script_hex, const std::string& expected_desc, const std::vector<std::string>& hex_scripts, const std::vector<std::pair<std::string, std::string>>& origin_pubkeys)
{
    std::vector<unsigned char> script_bytes{ParseHex(script_hex)};
    const CScript& script{script_bytes.begin(), script_bytes.end()};

    FlatSigningProvider provider;
    for (const std::string& prov_script_hex : hex_scripts) {
        std::vector<unsigned char> prov_script_bytes{ParseHex(prov_script_hex)};
        const CScript& prov_script{prov_script_bytes.begin(), prov_script_bytes.end()};
        provider.scripts.emplace(CScriptID(prov_script), prov_script);
    }
    for (const auto& [pubkey_hex, origin_str] : origin_pubkeys) {
        CPubKey origin_pubkey{ParseHex(pubkey_hex)};
        provider.pubkeys.emplace(origin_pubkey.GetID(), origin_pubkey);

        if (!origin_str.empty()) {
            KeyOriginInfo info;
            Span<const char> origin_sp{origin_str};
            std::vector<Span<const char>> origin_split = Split(origin_sp, "/");
            std::string fpr_str(origin_split[0].begin(), origin_split[0].end());
            auto fpr_bytes = ParseHex(fpr_str);
            std::copy(fpr_bytes.begin(), fpr_bytes.end(), info.fingerprint);
            for (size_t i = 1; i < origin_split.size(); ++i) {
                Span<const char> elem = origin_split[i];
                bool hardened = false;
                if (elem.size() > 0) {
                    const char last = elem[elem.size() - 1];
                    if (last == '\'' || last == 'h') {
                        elem = elem.first(elem.size() - 1);
                        hardened = true;
                    }
                }
                uint32_t p;
                assert(ParseUInt32(std::string(elem.begin(), elem.end()), &p));
                info.path.push_back(p | (((uint32_t)hardened) << 31));
            }

            provider.origins.emplace(origin_pubkey.GetID(), std::make_pair(origin_pubkey, info));
        }
    }

    std::string checksum{GetDescriptorChecksum(expected_desc)};

    std::unique_ptr<Descriptor> desc = InferDescriptor(script, provider);
    BOOST_CHECK_EQUAL(desc->ToString(), expected_desc + "#" + checksum);
}

}

BOOST_FIXTURE_TEST_SUITE(descriptor_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(descriptor_test)
{
    // Basic single-key compressed
    Check("combo(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "combo(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "combo(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", SIGNABLE, {{"2103a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bdac","76a9149a1c78a507689f6f54b847ad1cef1e614ee23f1e88ac","00149a1c78a507689f6f54b847ad1cef1e614ee23f1e"}}, std::nullopt, /*op_desc_id=*/uint256{"8ef71f7b6ac0918663f6706be469d6109f6922e21f484009d7ab49d77da36e8b"});
    Check("pk(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", SIGNABLE, {{"2103a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bdac"}}, std::nullopt, /*op_desc_id=*/uint256{"5fe175b43c58ac2cdde40521dc7d1dbc607f3dd795d00770206f4fdefb42229e"});
    Check("pkh([deadbeef/1/2'/3/4']RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "pkh([deadbeef/1/2'/3/4']03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "pkh([deadbeef/1/2h/3/4h]03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", SIGNABLE, {{"76a9149a1c78a507689f6f54b847ad1cef1e614ee23f1e88ac"}}, OutputType::BASE58, /*op_desc_id=*/uint256{"628130ae0530f2b24faf1ad2744a83568ac0ffac43e703e30c00d5f137869b84"}, {{1,0x80000002UL,3,0x80000004UL}});
    Check("wpkh(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "wpkh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "wpkh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", SIGNABLE, {{"00149a1c78a507689f6f54b847ad1cef1e614ee23f1e"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"4a47b7f497721bf3fc48c69a5d22bc1f3617238649a8ba7cb96fbd92fec84a7e"});
    CheckUnparsable("sh(wpkh(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))", "sh(wpkh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", "Can only have wpkh() at top level");
    CheckUnparsable("sh(wsh(pk(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)))", "sh(wsh(pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)))", "Can only have wsh() at top level");
    Check("tr(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", SIGNABLE | XONLY_KEYS, {{"512077aab6e066f8a7419c5ab714c12c67d25007ed55a43cadcacb4d7a970a093f11"}}, OutputType::BECH32M, /*op_desc_id=*/uint256{"4290f3d017b270be53b91abc56d9d2f23a3ff361d5b1d39550ba011e6cae0da5"});
    CheckUnparsable("pkh(deadbeef/1/2'/3/4']RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "pkh(deadbeef/1/2h/3/4h]03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "pkh(): Key origin start '[ character expected but not found, got 'd' instead"); // Missing start bracket in key origin
    CheckUnparsable("pkh([deadbeef]/1/2'/3/4']RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "pkh([deadbeef]/1/2'/3/4']03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "pkh(): Multiple ']' characters found for a single pubkey"); // Multiple end brackets in key origin

    // Basic single-key uncompressed
    Check("combo(6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "combo(04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "combo(04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)",SIGNABLE, {{"4104a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235ac","76a914b5bd079c4d57cc7fc28ecf8213a6b791625b818388ac"}}, std::nullopt, /*op_desc_id=*/uint256{"33f6bb5d32c04e9d9e5466a8212836743bd5466aa0b8d5331ce8aa0812371ffd"});
    Check("pk(6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "pk(04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "pk(04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", SIGNABLE, {{"4104a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235ac"}}, std::nullopt, /*op_desc_id=*/uint256{"52306fc1f5d0cb78aacea9d3933092be9252adc27b146f97c16a94d6fcdb652e"});
    Check("pkh(6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "pkh(04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "pkh(04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", SIGNABLE, {{"76a914b5bd079c4d57cc7fc28ecf8213a6b791625b818388ac"}}, OutputType::BASE58, /*op_desc_id=*/uint256{"36657e8690d4015032da1a8c1e37b315c3f7ccb010e6ada12967878711962991"});
    CheckUnparsable("wpkh(6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "wpkh(04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "wpkh(): Uncompressed keys are not allowed"); // No uncompressed keys in witness
    CheckUnparsable("wsh(pk(6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ))", "wsh(pk(04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235))", "pk(): Uncompressed keys are not allowed"); // No uncompressed keys in witness

    // Equivalent single-key hybrid is not allowed
    CheckUnparsable("", "combo(07a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "combo(): Hybrid public keys are not allowed");
    CheckUnparsable("", "pk(0623542d61708e3fc48ba78fbe8fcc983ba94a520bc33f82b8e45e51dbc47af2726bcf181925eee1bdd868b109314f3ea92a6fc23d6b66057d3acfba04d6b08b58)", "pk(): Hybrid public keys are not allowed");
    CheckUnparsable("", "pkh(07a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "pkh(): Hybrid public keys are not allowed");

    // Some unconventional single-key constructions
    Check("sh(pk(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))", "sh(pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", "sh(pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", SIGNABLE, {{"a9141857af51a5e516552b3086430fd8ce55f7c1a52487"}}, OutputType::BASE58);
    Check("sh(pkh(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))", "sh(pkh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", "sh(pkh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", SIGNABLE, {{"a9141a31ad23bf49c247dd531a623c2ef57da3c400c587"}}, OutputType::BASE58);
    Check("wsh(pk(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))", "wsh(pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", "wsh(pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", SIGNABLE, {{"00202e271faa2325c199d25d22e1ead982e45b64eeb4f31e73dbdf41bd4b5fec23fa"}}, OutputType::BECH32);
    Check("wsh(pkh(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))", "wsh(pkh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", "wsh(pkh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", SIGNABLE, {{"0020338e023079b91c58571b20e602d7805fb808c22473cbc391a41b1bd3a192e75b"}}, OutputType::BECH32);
    Check("tr(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5,{pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5),{pk(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW),pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5)}})", "tr(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5,{pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5),{pk(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd),pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5)}})", "tr(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5,{pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5),{pk(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd),pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5)}})", XONLY_KEYS | SIGNABLE | MISSING_PRIVKEYS, {{"51201497ae16f30dacb88523ed9301bff17773b609e8a90518a3f96ea328a47d1500"}}, OutputType::BECH32M);

    // Versions with BIP32 derivations
    Check("combo([01234567]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)", "combo([01234567]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)", "combo([01234567]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)", SIGNABLE, {{"2102d2b36900396c9282fa14628566582f206a5dd0bcc8d5e892611806cafb0301f0ac","76a91431a507b815593dfc51ffc7245ae7e5aee304246e88ac","001431a507b815593dfc51ffc7245ae7e5aee304246e"}}, std::nullopt, /*op_desc_id=*/uint256{"7b1e5b91452b4b6795751286fb4fa52aa9aa04992074383466d60ee2e431e626"});
    Check("pk(qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0)", "pk(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0)", "pk(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0)", DEFAULT, {{"210379e45b3cf75f9c5f9befd8e9506fb962f6a9d185ac87001ec44a8d3df8d4a9e3ac"}}, std::nullopt, /*op_desc_id=*/uint256{"5642a0b019c55fa1222038b46055481422f109b3c4cc762c430de39c40dd0ec3"}, {{0}});
    Check("pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483647'/0)", "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483647'/0)", "pkh([bd16bee5/2147483647h]qpub3sTG7QvzwzhG6r45VpPF4hLC9TP4PZHEDzsW4sgVELEVEqNgefjDoxb7d5kn5HJUHxBLVM3ownPJJGCY56jSce1cnEGxqCRfepQ1FF1R2XS/0)", HARDENED, {{"76a914ebdc90806a9c4356c1c88e42216611e1cb4c1c1788ac"}}, OutputType::BASE58, /*op_desc_id=*/uint256{"a3d550d2a84faae4b20ce7937f07caf7421c23e8b28df097feffe6674b4cb62d"}, {{0xFFFFFFFFUL,0}});

    Check("wpkh([ffffffff/13']qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/1/2/*)", "wpkh([ffffffff/13']qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*)", "wpkh([ffffffff/13h]qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*)", RANGE, {{"0014326b2249e3a25d5dc60935f044ee835d090ba859"},{"0014af0bd98abc2f2cae66e36896a39ffe2d32984fb7"},{"00141fa798efd1cbf95cebf912c031b8a4a6e9fb9f27"}}, OutputType::BECH32, /*op_desc_id=*/std::nullopt, {{0x8000000DUL, 1, 2, 0}, {0x8000000DUL, 1, 2, 1}, {0x8000000DUL, 1, 2, 2}});
    Check("combo(qprv7kUNWeW2KbmQ22ZMNRD8CgmomczddCeD1x5CNKTRcysmyAY2Hgieg2NiyyxJNYpFGBHneepYg1NkSqYvsuzyn7Gj9YbCE3Z2HxdMa88WiTX/*)", "combo(qpub3yTivA2v9yKhEWdpUSk8ZpiYKeq82fN4PAzoAhs3BKQkqxsAqE2uDphCqGddTpNKF5WnA9BznvgMGikAqYBw31aubGCLgvPDo4YpLcMw1iG/*)", "combo(qpub3yTivA2v9yKhEWdpUSk8ZpiYKeq82fN4PAzoAhs3BKQkqxsAqE2uDphCqGddTpNKF5WnA9BznvgMGikAqYBw31aubGCLgvPDo4YpLcMw1iG/*)", RANGE, {{"2102df12b7035bdac8e3bab862a3a83d06ea6b17b6753d52edecba9be46f5d09e076ac","76a914f90e3178ca25f2c808dc76624032d352fdbdfaf288ac","0014f90e3178ca25f2c808dc76624032d352fdbdfaf2"},{"21032869a233c9adff9a994e4966e5b821fd5bac066da6c3112488dc52383b4a98ecac","76a914a8409d1b6dfb1ed2a3e8aa5e0ef2ff26b15b75b788ac","0014a8409d1b6dfb1ed2a3e8aa5e0ef2ff26b15b75b7"}}, std::nullopt, /*op_desc_id=*/std::nullopt, {{0}, {1}});
    Check("tr(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/0/*,pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/1/*))", "tr(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/0/*,pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/*))", "tr(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/0/*,pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/*))", XONLY_KEYS | RANGE, {{"512078bc707124daa551b65af74de2ec128b7525e10f374dc67b64e00ce0ab8b3e12"}, {"512001f0a02a17808c20134b78faab80ef93ffba82261ccef0a2314f5d62b6438f11"}, {"512021024954fcec88237a9386fce80ef2ced5f1e91b422b26c59ccfc174c8d1ad25"}}, OutputType::BECH32M, /*op_desc_id=*/std::nullopt, {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}});
    // Mixed xpubs and const pubkeys
    Check("wsh(multi(1,qprv7kUNWeW2KbmQ22ZMNRD8CgmomczddCeD1x5CNKTRcysmyAY2Hgieg2NiyyxJNYpFGBHneepYg1NkSqYvsuzyn7Gj9YbCE3Z2HxdMa88WiTX/0,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))","wsh(multi(1,qpub3yTivA2v9yKhEWdpUSk8ZpiYKeq82fN4PAzoAhs3BKQkqxsAqE2uDphCqGddTpNKF5WnA9BznvgMGikAqYBw31aubGCLgvPDo4YpLcMw1iG/0,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))","wsh(multi(1,qpub3yTivA2v9yKhEWdpUSk8ZpiYKeq82fN4PAzoAhs3BKQkqxsAqE2uDphCqGddTpNKF5WnA9BznvgMGikAqYBw31aubGCLgvPDo4YpLcMw1iG/0,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", MIXED_PUBKEYS, {{"0020cb155486048b23a6da976d4c6fe071a2dbc8a7b57aaf225b8955f2e2a27b5f00"}},OutputType::BECH32, /*op_desc_id=*/uint256{"7a145bdd02a9ccceca4de25c9ca99c7b08d6085763a3664a99835d0c25e0e421"},{{0},{}});
    // Mixed range xpubs and const pubkeys
    Check("multi(1,qprv7kUNWeW2KbmQ22ZMNRD8CgmomczddCeD1x5CNKTRcysmyAY2Hgieg2NiyyxJNYpFGBHneepYg1NkSqYvsuzyn7Gj9YbCE3Z2HxdMa88WiTX/*,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)","multi(1,qpub3yTivA2v9yKhEWdpUSk8ZpiYKeq82fN4PAzoAhs3BKQkqxsAqE2uDphCqGddTpNKF5WnA9BznvgMGikAqYBw31aubGCLgvPDo4YpLcMw1iG/*,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)","multi(1,qpub3yTivA2v9yKhEWdpUSk8ZpiYKeq82fN4PAzoAhs3BKQkqxsAqE2uDphCqGddTpNKF5WnA9BznvgMGikAqYBw31aubGCLgvPDo4YpLcMw1iG/*,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", RANGE | MIXED_PUBKEYS, {{"512102df12b7035bdac8e3bab862a3a83d06ea6b17b6753d52edecba9be46f5d09e0762103a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd52ae"},{"5121032869a233c9adff9a994e4966e5b821fd5bac066da6c3112488dc52383b4a98ec2103a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd52ae"},{"5121035d30b6c66dc1e036c45369da8287518cf7e0d6ed1e2b905171c605708f14ca032103a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd52ae"}}, std::nullopt, /*op_desc_id=*/std::nullopt,{{2},{1},{0},{}});

    CheckUnparsable("combo([012345678]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)", "combo([012345678]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)", "combo(): Fingerprint is not 4 bytes (9 characters instead of 8 characters)"); // Too long key fingerprint
    CheckUnparsable("pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483648)", "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483648)", "pkh(): Key path value 2147483648 is out of range"); // BIP 32 path element overflow
    CheckUnparsable("pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/1aa)", "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/1aa)", "pkh(): Key path value '1aa' is not a valid uint32"); // Path is not valid uint
    Check("pkh([01234567/10/20]qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483647'/0)", "pkh([01234567/10/20]qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483647'/0)", "pkh([01234567/10/20/2147483647h]qpub3sTG7QvzwzhG6r45VpPF4hLC9TP4PZHEDzsW4sgVELEVEqNgefjDoxb7d5kn5HJUHxBLVM3ownPJJGCY56jSce1cnEGxqCRfepQ1FF1R2XS/0)", HARDENED, {{"76a914ebdc90806a9c4356c1c88e42216611e1cb4c1c1788ac"}}, OutputType::BASE58, /*op_desc_id=*/std::nullopt, {{10, 20, 0xFFFFFFFFUL, 0}});

    // Multipath versions with BIP32 derivations
    CheckMultipath("pk(qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/<0;1>)",
            "pk(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/<0;1>)",
            {
                "pk(qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0)",
                "pk(qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/1)",
            },
            {
                "pk(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0)",
                "pk(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/1)",
            },
            {
                "pk(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0)",
                "pk(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/1)",
            },
            DEFAULT,
            {
                {{"210379e45b3cf75f9c5f9befd8e9506fb962f6a9d185ac87001ec44a8d3df8d4a9e3ac"}},
                {{"21034f8d02282ac6786737d0f37f0df7655f49daa24843bc7de3f4ea88603d26d10aac"}},
            },
            std::nullopt,
            {
                {{0}},
                {{1}},
            }
    );
    CheckMultipath("pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<2147483647h;0>/0)",
            "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<2147483647h;0>/0)",
            {
                "pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483647h/0)",
                "pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/0/0)",
            },
            {
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483647h/0)",
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/0/0)",
            },
            {
                "pkh([bd16bee5/2147483647h]qpub3sTG7QvzwzhG6r45VpPF4hLC9TP4PZHEDzsW4sgVELEVEqNgefjDoxb7d5kn5HJUHxBLVM3ownPJJGCY56jSce1cnEGxqCRfepQ1FF1R2XS/0)",
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/0/0)",
            },
            HARDENED,
            {
                {{"76a914ebdc90806a9c4356c1c88e42216611e1cb4c1c1788ac"}},
                {{"76a914f103317b9f0b758a62cb3879281d23e3b1deb90d88ac"}},
            },
            OutputType::BASE58,
            {
                {{0xFFFFFFFFUL,0}},
                {{0,0}},
            }
    );
    CheckMultipath("wpkh([ffffffff/13h]qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/<1;3>/2/*)",
            "wpkh([ffffffff/13h]qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/<1;3>/2/*)",
            {
                "wpkh([ffffffff/13h]qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/1/2/*)",
                "wpkh([ffffffff/13h]qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/3/2/*)",
            },
            {
                "wpkh([ffffffff/13h]qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*)",
                "wpkh([ffffffff/13h]qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/3/2/*)",
            },
            {
                "wpkh([ffffffff/13h]qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*)",
                "wpkh([ffffffff/13h]qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/3/2/*)",
            },
            RANGE,
            {
                {{"0014326b2249e3a25d5dc60935f044ee835d090ba859"},{"0014af0bd98abc2f2cae66e36896a39ffe2d32984fb7"},{"00141fa798efd1cbf95cebf912c031b8a4a6e9fb9f27"}},
                {{"001426183882ef9c76b9a44386e9b387f33cee7c3a2d"},{"001447c1b9dc215c3f8b47e572981eb97528768cde4e"},{"00146e92cbaa397f9caeccf9a049460258af6ccd67e2"}},
            },
            OutputType::BECH32,
            {
                {{0x8000000DUL, 1, 2, 0}, {0x8000000DUL, 1, 2, 1}, {0x8000000DUL, 1, 2, 2}},
                {{0x8000000DUL, 3, 2, 0}, {0x8000000DUL, 3, 2, 1}, {0x8000000DUL, 3, 2, 2}},
            }
    );
    CheckMultipath("multi(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/<1;2>/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/<3;4>/0/*)",
            "multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/<1;2>/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/<3;4>/0/*)",
            {
                "multi(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/1/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/3/0/*)",
                "multi(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/2/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/4/0/*)",
            },
            {
                "multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/3/0/*)",
                "multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/2/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/4/0/*)",
            },
            {
                "multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/3/0/*)",
                "multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/2/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/4/0/*)",
            },
            RANGE,
            {
                {{"522103095e95d8c50ae3f3fea93fa8e983f710489f60ff681a658c06eba64622c824b121020443e9e729b42628913f1a69b46b7d43ff87c46e86140e12ee420d7e2e8caf8c52ae"},{"5221027512d6bd74e24eeb1ad752d5be800adc5886ded11c5293a9a701db83658b526a2102371e912dea5fefa56158908fe4c9f66bc925a8939b10f3821e8f8be797b9ca8252ae"},{"522102cc9fd211dc0a1c8bb7a106ff831be0e253bc992f21d08fb8a6fd43fae51b9b892103e43eddc68afc9746c9d09ce0bf8067b4f2416287abbc422ed1ac300673b1104952ae"}},
                {{"5221031c0517fff3d483f06ca769bd2326bf30aca1c4de278e676e6ef760c3301244c6210316e171ff4f82dc62ad3f0d84c97865034fc5041eaa508b48c1d7af77f301c8bd52ae"},{"52210240f010ccff4202ade2ef87756f6b9af57bbf5ebcb0393b949e6e5d45d30bff36210229057a7e03510b8cb66727fab3f47a52a02ea94eae03e7c2e81b72a26781bfde52ae"},{"5221034052522058a07b647bd08fa1a9eaedae0222eac76ddd122ff8096ec969398de721038cb8180dd4c956848bcf191e45aaf297146207559fb8737881156aadaf13704152ae"}},
            },
            std::nullopt,
            {
                {{1, 0}, {1, 1}, {1, 2}, {3, 0, 0}, {3, 0, 1}, {3, 0, 2}},
                {{2, 0}, {2, 1}, {2, 2}, {4, 0, 0}, {4, 0, 1}, {4, 0, 2}},
            }
    );
    CheckMultipath("pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<0;1;2>)",
            "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<0;1;2>)",
            {
                "pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/0)",
                "pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/1)",
                "pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2)",
            },
            {
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/0)",
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/1)",
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2)",
            },
            {
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/0)",
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/1)",
                "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2)",
            },
            DEFAULT,
            {
                {{"76a9145a61ff8eb7aaca3010db97ebda76121610b7809688ac"}},
                {{"76a9142f792a782cf4adbb321fe646c8e220563649b8fa88ac"}},
                {{"76a914dcc5b93b52177d78f97b3f2d259b9a86ee1403b188ac"}},
            },
            OutputType::BASE58,
            {
                {{0}},
                {{1}},
                {{2}},
            }
    );
    CheckMultipath("sh(multi(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/<1;2;3>/0/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0/*,qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/<3;4;5>/*))",
            "sh(multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/<1;2;3>/0/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/*,qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/<3;4;5>/*))",
            {
                "sh(multi(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/1/0/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0/*,qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/3/*))",
                "sh(multi(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/2/0/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0/*,qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/4/*))",
                "sh(multi(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/3/0/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0/*,qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/5/*))",
            },
            {
                "sh(multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/0/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/*,qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/3/*))",
                "sh(multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/2/0/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/*,qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/4/*))",
                "sh(multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/3/0/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/*,qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/5/*))",
            },
            {
                "sh(multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/0/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/*,qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/3/*))",
                "sh(multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/2/0/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/*,qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/4/*))",
                "sh(multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/3/0/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/*,qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/5/*))",
            },
            RANGE,
            {
                {{"a914689cdf7de5836ec04fb971d128cc84858f73e11487"},{"a9142ea7dbaf0a77ee19f080cdacb3e13560e3cd9cf587"},{"a9143da854021f58f5e2d3ff6bb4fcd0ced877deb34987"}},
                {{"a9143dd613d162e89b83369bbf08e5f1977cfdc9b02787"},{"a91449eef5d3df5c465b20a630c66058fe689082d8e187"},{"a91492be56babf54ea2109c577f799ba6d73948e8c3287"}},
                {{"a9140093ca92097bdf557fbb0570bb77e1efd2e7529c87"},{"a914e4d0419d3d2ce8f921a800796811ff5462bb151887"},{"a914997bf69841ac444190dc02f5e6031dd6f8feab4587"}},
            },
            OutputType::BASE58,
            {
                {{1, 0, 0}, {1, 0, 1}, {1, 0, 2}, {0, 0}, {0, 1}, {0, 2}, {0, 0, 3, 0}, {0, 0, 3, 1}, {0, 0, 3, 2}},
                {{2, 0, 0}, {2, 0, 1}, {2, 0, 2}, {0, 0}, {0, 1}, {0, 2}, {0, 0, 4, 0}, {0, 0, 4, 1}, {0, 0, 4, 2}},
                {{3, 0, 0}, {3, 0, 1}, {3, 0, 2}, {0, 0}, {0, 1}, {0, 2}, {0, 0, 5, 0}, {0, 0, 5, 1}, {0, 0, 5, 2}},
            }
    );
    CheckMultipath("tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/<6;7;8>/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/<1;2;3>/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/<3;4;5>/*)})",
            "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/<6;7;8>/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/<1;2;3>/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/<3;4;5>/*)})",
            {
                "tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/6/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/1/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/3/*)})",
                "tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/7/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/2/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/4/*)})",
                "tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/8/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/3/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/5/*)})",
            },
            {
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/3/*)})",
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/7/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/2/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/4/*)})",
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/8/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/3/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/5/*)})",
            },
            {
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/3/*)})",
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/7/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/2/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/4/*)})",
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/8/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/3/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/5/*)})",
            },
            XONLY_KEYS | RANGE,
            {
                {{"5120993e5b1d71d14cbb0a90c57ea0fed1d5bf77d5804cee206c3dbd7e4d2c67d869"},{"51207b8f629f6d406b92ffa6284f5545085eafb837c469018b715755f619b587163b"},{"512061f52925826e51e4615007557ddbea55b22c817909d7ebcfd3c454c634643ece"}},
                {{"5120633808b2156d0a6597e8b07f59c387bb4c2d5c02c4cb98f1802748e64c6abf5f"},{"5120fc5f06ded29328c170bf7e49e71c9cc8699befa2bf0a2a80802a1f32ab72d291"},{"5120fd05e2227e0dac972dff9941e332db8461bedc320c2a74def44e469ddbad9d21"}},
                {{"51205d19538c7c0901520eb712d079ae6eebed4f691021da466dc24e9575d9815ad0"},{"5120b9fc348ede2b7b9fb1f84c21741bb36bb3fa0905d0bc9417e07145d3142673f7"},{"51203a655bc5181b12efac82a5a5d1d0969b2ceb92c6fc37f505fdf00ee8afa09b33"}},
            },
            OutputType::BECH32M,
            {
                {{6, 0}, {6, 1}, {6, 2}, {1, 0, 0}, {1, 0, 1}, {1, 0, 2}, {0, 0, 3, 0}, {0, 0, 3, 1}, {0, 0, 3, 2}},
                {{7, 0}, {7, 1}, {7, 2}, {2, 0, 0}, {2, 0, 1}, {2, 0, 2}, {0, 0, 4, 0}, {0, 0, 4, 1}, {0, 0, 4, 2}},
                {{8, 0}, {8, 1}, {8, 2}, {3, 0, 0}, {3, 0, 1}, {3, 0, 2}, {0, 0, 5, 0}, {0, 0, 5, 1}, {0, 0, 5, 2}},
            }
    );
    CheckMultipath("tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/6/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/<1;2;3>/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/<3;4;5>/*)})",
            "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/<1;2;3>/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/<3;4;5>/*)})",
            {
                "tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/6/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/1/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/3/*)})",
                "tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/6/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/2/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/4/*)})",
                "tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/6/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/3/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/5/*)})",
            },
            {
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/3/*)})",
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/2/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/4/*)})",
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/3/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/5/*)})",
            },
            {
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/3/*)})",
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/2/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/4/*)})",
                "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/3/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/5/*)})",
            },
            XONLY_KEYS | RANGE,
            {
                {{"5120993e5b1d71d14cbb0a90c57ea0fed1d5bf77d5804cee206c3dbd7e4d2c67d869"},{"51207b8f629f6d406b92ffa6284f5545085eafb837c469018b715755f619b587163b"},{"512061f52925826e51e4615007557ddbea55b22c817909d7ebcfd3c454c634643ece"}},
                {{"5120c481a8ada38d1070094f62af526d4f8aae2eb1e44d1fd961be6a25198b4da77b"},{"512034a2d31c091905e62def62b575b88beff41723d83acb02dfada2e73d9c529b40"},{"5120e0ecc278655b092962ded92a5781bd8e86e8408055de05f121e107fa211e5dfb"}},
                {{"51206052cff5efc848e4b38a947803943eb1eb0076523eec1041969851ebcd265555"},{"512009ed83d758c0bdd36e225c961810761c7a360533434a41a17bba709e331e6cd1"},{"5120fcd77851ebaac37564b87e9b351c54492a8fbb1d6afdf7f3a9317703a002b22b"}},
            },
            OutputType::BECH32M,
            {
                {{6, 0}, {6, 1}, {6, 2}, {1, 0, 0}, {1, 0, 1}, {1, 0, 2}, {0, 0, 3, 0}, {0, 0, 3, 1}, {0, 0, 3, 2}},
                {{6, 0}, {6, 1}, {6, 2}, {2, 0, 0}, {2, 0, 1}, {2, 0, 2}, {0, 0, 4, 0}, {0, 0, 4, 1}, {0, 0, 4, 2}},
                {{6, 0}, {6, 1}, {6, 2}, {3, 0, 0}, {3, 0, 1}, {3, 0, 2}, {0, 0, 5, 0}, {0, 0, 5, 1}, {0, 0, 5, 2}},
            }
    );
    CheckMultipath("wsh(or_d(pk([2557c640/48h/1h/0h/2h]qprv7g3GZbYzLpRKd252Px5stVcKQLjsV8SJtKfhbzV23vv2ouTmQEGR7mNvFcbF2RkzhSTL1QWBFHhPUNZR27tUfyot3MXqJkFjrBM7LAEjDcC/<0;1>/*),and_v(v:pkh([00aabb22/48h/1h/0h/2h]qprv7g3GZbYzLpRKe8aYn2zHp2bnvhSJfREDPH4d3oPoPvmKw4bJYgDDSuvEmgHmKRYmmN7CihYzTd2KWDLXSeCkVqZASNYVjamPWDehsv4Kuv4/<0;1>/*),older(2))))",
            "wsh(or_d(pk([2557c640/48h/1h/0h/2h]qpub3u2cy75tBBycqW9VVyctFdZ3xNaMtbAAFYbJQNtdcGT1ghnuwmaffZhQ6t3RPP71GtNqXV5uDoi1MFbZCHwB9CpdNgR3W3XQctjGpcfiMFk/<0;1>/*),and_v(v:pkh([00aabb22/48h/1h/0h/2h]qpub3u2cy75tBBycrcf1t4XJBAYXUjGo4sx4kVzDrBoQxGJJorvT6DXTziEicwTg2RG1BVsUmakM4BDWZfkiLTuRzLzuo3v6UdndRhkemiiV1z8/<0;1>/*),older(2))))",
            {
                "wsh(or_d(pk([2557c640/48h/1h/0h/2h]qprv7g3GZbYzLpRKd252Px5stVcKQLjsV8SJtKfhbzV23vv2ouTmQEGR7mNvFcbF2RkzhSTL1QWBFHhPUNZR27tUfyot3MXqJkFjrBM7LAEjDcC/0/*),and_v(v:pkh([00aabb22/48h/1h/0h/2h]qprv7g3GZbYzLpRKe8aYn2zHp2bnvhSJfREDPH4d3oPoPvmKw4bJYgDDSuvEmgHmKRYmmN7CihYzTd2KWDLXSeCkVqZASNYVjamPWDehsv4Kuv4/0/*),older(2))))",
                "wsh(or_d(pk([2557c640/48h/1h/0h/2h]qprv7g3GZbYzLpRKd252Px5stVcKQLjsV8SJtKfhbzV23vv2ouTmQEGR7mNvFcbF2RkzhSTL1QWBFHhPUNZR27tUfyot3MXqJkFjrBM7LAEjDcC/1/*),and_v(v:pkh([00aabb22/48h/1h/0h/2h]qprv7g3GZbYzLpRKe8aYn2zHp2bnvhSJfREDPH4d3oPoPvmKw4bJYgDDSuvEmgHmKRYmmN7CihYzTd2KWDLXSeCkVqZASNYVjamPWDehsv4Kuv4/1/*),older(2))))",
            },
            {
                "wsh(or_d(pk([2557c640/48h/1h/0h/2h]qpub3u2cy75tBBycqW9VVyctFdZ3xNaMtbAAFYbJQNtdcGT1ghnuwmaffZhQ6t3RPP71GtNqXV5uDoi1MFbZCHwB9CpdNgR3W3XQctjGpcfiMFk/0/*),and_v(v:pkh([00aabb22/48h/1h/0h/2h]qpub3u2cy75tBBycrcf1t4XJBAYXUjGo4sx4kVzDrBoQxGJJorvT6DXTziEicwTg2RG1BVsUmakM4BDWZfkiLTuRzLzuo3v6UdndRhkemiiV1z8/0/*),older(2))))",
                "wsh(or_d(pk([2557c640/48h/1h/0h/2h]qpub3u2cy75tBBycqW9VVyctFdZ3xNaMtbAAFYbJQNtdcGT1ghnuwmaffZhQ6t3RPP71GtNqXV5uDoi1MFbZCHwB9CpdNgR3W3XQctjGpcfiMFk/1/*),and_v(v:pkh([00aabb22/48h/1h/0h/2h]qpub3u2cy75tBBycrcf1t4XJBAYXUjGo4sx4kVzDrBoQxGJJorvT6DXTziEicwTg2RG1BVsUmakM4BDWZfkiLTuRzLzuo3v6UdndRhkemiiV1z8/1/*),older(2))))"
            },
            {
                "wsh(or_d(pk([2557c640/48h/1h/0h/2h]qpub3u2cy75tBBycqW9VVyctFdZ3xNaMtbAAFYbJQNtdcGT1ghnuwmaffZhQ6t3RPP71GtNqXV5uDoi1MFbZCHwB9CpdNgR3W3XQctjGpcfiMFk/0/*),and_v(v:pkh([00aabb22/48h/1h/0h/2h]qpub3u2cy75tBBycrcf1t4XJBAYXUjGo4sx4kVzDrBoQxGJJorvT6DXTziEicwTg2RG1BVsUmakM4BDWZfkiLTuRzLzuo3v6UdndRhkemiiV1z8/0/*),older(2))))",
                "wsh(or_d(pk([2557c640/48h/1h/0h/2h]qpub3u2cy75tBBycqW9VVyctFdZ3xNaMtbAAFYbJQNtdcGT1ghnuwmaffZhQ6t3RPP71GtNqXV5uDoi1MFbZCHwB9CpdNgR3W3XQctjGpcfiMFk/1/*),and_v(v:pkh([00aabb22/48h/1h/0h/2h]qpub3u2cy75tBBycrcf1t4XJBAYXUjGo4sx4kVzDrBoQxGJJorvT6DXTziEicwTg2RG1BVsUmakM4BDWZfkiLTuRzLzuo3v6UdndRhkemiiV1z8/1/*),older(2))))"
            },
            RANGE,
            {
                {{"0020538436a60f2a638ea9e1e1342e9b93374aa7ec559ff0a805b3a185d4ba855d7f"},{"00203a588d107d604b6913201c7c1e1722f07a0f8fb3a382744f17b9ae5f6ccfcdd7"},{"0020d30fb375f7c491a208e77c7b5d0996ca14cf4a770c2ab5981f915c0e4565c74a"}},
                {{"002072b5fc3a691c48fdbaf485f27e787b4094055d4b434c90c81ed1090f3d48733b"},{"0020a9ccdf4496e5d60db4704b27494d9d74f54a16c180ff954a43ce5e3aa465113a"},{"0020d17e21820a0069ca87049513eca763f08a74b586724441e7d76fc5142bcc327c"}},
            },
            OutputType::BECH32,
            {
                {{0x80000000UL + 48, 0x80000000UL + 1, 0x80000000UL, 0x80000000UL + 2, 0, 0}, {0x80000000UL + 48, 0x80000000UL + 1, 0x80000000UL, 0x80000000UL + 2, 0, 1}, {0x80000000UL + 48, 0x80000000UL + 1, 0x80000000UL, 0x80000000UL + 2, 0, 2}},
                {{0x80000000UL + 48, 0x80000000UL + 1, 0x80000000UL, 0x80000000UL + 2, 1, 0}, {0x80000000UL + 48, 0x80000000UL + 1, 0x80000000UL, 0x80000000UL + 2, 1, 1}, {0x80000000UL + 48, 0x80000000UL + 1, 0x80000000UL, 0x80000000UL + 2, 1, 2}},
            }
    );
    CheckMultipath("tr(qprv7hiqWPh22HjPbNoVsq9pyKwc6strXsy8YcdMRUPQeWuXwg2Epnobw1o2PTAendvjzA27Aq6xU4tvNz5vdb8rJEnxy9rFJsfWB1g4gUjjGqk,l:pk(qprv7jLNbufiEGUuqV6x3Yysevxzbi8fnVCFJ44NPCcetvy1nYcUJUTHrMyZsacQtjc1ZtXGgURarrvc47rSLAp4UUJ86JEgjDGF2KYDUckhDpK/<2;3>))",
        "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ/<2;3>))",
        {
            "tr(qprv7hiqWPh22HjPbNoVsq9pyKwc6strXsy8YcdMRUPQeWuXwg2Epnobw1o2PTAendvjzA27Aq6xU4tvNz5vdb8rJEnxy9rFJsfWB1g4gUjjGqk,l:pk(qprv7jLNbufiEGUuqV6x3Yysevxzbi8fnVCFJ44NPCcetvy1nYcUJUTHrMyZsacQtjc1ZtXGgURarrvc47rSLAp4UUJ86JEgjDGF2KYDUckhDpK/2))",
            "tr(qprv7hiqWPh22HjPbNoVsq9pyKwc6strXsy8YcdMRUPQeWuXwg2Epnobw1o2PTAendvjzA27Aq6xU4tvNz5vdb8rJEnxy9rFJsfWB1g4gUjjGqk,l:pk(qprv7jLNbufiEGUuqV6x3Yysevxzbi8fnVCFJ44NPCcetvy1nYcUJUTHrMyZsacQtjc1ZtXGgURarrvc47rSLAp4UUJ86JEgjDGF2KYDUckhDpK/3))",
        },
        {
            "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ/2))",
            "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ/3))",
        },
        {
            "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ/2))",
            "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ/3))",
        },
        XONLY_KEYS,
        {
            {{"512094cb097990da64eebbad7b979b1326f3cbe356357abf4deb4c4ff80c7acbe902"}},
            {{"5120f091450b88c606f5cbc3f0cebe89e00bc5dd27f92e22f54da06439bc0c401f41"}},
        },
        OutputType::BECH32M,
        {
            {{2}, {}},
            {{3}, {}},
        }
    );
    CheckMultipath("tr(qprv7hiqWPh22HjPbNoVsq9pyKwc6strXsy8YcdMRUPQeWuXwg2Epnobw1o2PTAendvjzA27Aq6xU4tvNz5vdb8rJEnxy9rFJsfWB1g4gUjjGqk/<2;3>,l:pk(qprv7jLNbufiEGUuqV6x3Yysevxzbi8fnVCFJ44NPCcetvy1nYcUJUTHrMyZsacQtjc1ZtXGgURarrvc47rSLAp4UUJ86JEgjDGF2KYDUckhDpK))",
            "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN/<2;3>,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ))",
            {
                "tr(qprv7hiqWPh22HjPbNoVsq9pyKwc6strXsy8YcdMRUPQeWuXwg2Epnobw1o2PTAendvjzA27Aq6xU4tvNz5vdb8rJEnxy9rFJsfWB1g4gUjjGqk/2,l:pk(qprv7jLNbufiEGUuqV6x3Yysevxzbi8fnVCFJ44NPCcetvy1nYcUJUTHrMyZsacQtjc1ZtXGgURarrvc47rSLAp4UUJ86JEgjDGF2KYDUckhDpK))",
                "tr(qprv7hiqWPh22HjPbNoVsq9pyKwc6strXsy8YcdMRUPQeWuXwg2Epnobw1o2PTAendvjzA27Aq6xU4tvNz5vdb8rJEnxy9rFJsfWB1g4gUjjGqk/3,l:pk(qprv7jLNbufiEGUuqV6x3Yysevxzbi8fnVCFJ44NPCcetvy1nYcUJUTHrMyZsacQtjc1ZtXGgURarrvc47rSLAp4UUJ86JEgjDGF2KYDUckhDpK))",
            },
            {
                "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN/2,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ))",
                "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN/3,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ))",
            },
            {
                "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN/2,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ))",
                "tr(qpub3viBuuDurfHgorsxyrgqLTtLeujLwLgyuqYxDro2CrSWpUMPNL7rUp7WEhRTajF6b4RS5SkG4q4CEZqJ8RNb7LQXpuZ849FxEcS6fngV5oN/3,l:pk(qpub3xKj1RCc4e3D3yBR9aWt24uj9jyABwv6fGyyBb2GTGVzfLwcr1mYQAJ3iqqhCchippomngvwJDJJdGGjXR5v11Scn8CiBZhenQ5chjbUUmZ))",
            },
            XONLY_KEYS,
            {
                {{"51200e3c14456bfa30f9f0bed6e55f35e1e9ca17c835e9f71b25bac0dfaab38ff2cd"}},
                {{"51202bdda29337ecaf8fcd5aa395febac6f99b8a866a0e8fb3f7bde2e24b1a7df2ba"}},
            },
            OutputType::BECH32M,
            {
                {{2}, {}},
                {{3}, {}},
            }
    );
    CheckUnparsable("pkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<0;1>/<2;3>)", "pkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<0;1>/<2;3>)", "pkh(): Multiple multipath key path specifiers found");
    CheckUnparsable("pkh([deadbeef/<0;1>]qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/0)", "pkh([deadbeef/<0;1>]qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/0)", "pkh(): Key path value \'<0;1>\' specifies multipath in a section where multipath is not allowed");
    CheckUnparsable("tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/6/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/<1;2;3>/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/<3;4>/*)})", "tr(qpub3uF2JvgS5kTBamVVLbtH237KUTZNHrQFGqRoBGEMEcMGv7HbM4WGa3mEqTSMDKiM2wazUSZZqFokmm5imk4oL3KfW11b9ZQbnf5phg4HZ4B/6/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/<1;2;3>/0/*),pk(qpub3tsQZtbtE7MBE4NJEvAUqX4vqg17ejJAKRYzMmQZMAXTcYXvz8yLrbbZKEKMmxmh5ymoY7m8imCE7ugPh1amEmzYfQ9DKAF9tksSH6N6xZF/0/0/<3;4>/*)})", "tr(): Multipath subscripts have mismatched lengths");
    CheckUnparsable("tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/<6;7;8;9>/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/<1;2;3>/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/<3;4;5>/*)})", "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/<6;7;8;9>/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/<1;2;3>/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/<3;4;5>/*)})", "tr(): Multipath subscripts have mismatched lengths");
    CheckUnparsable("tr(qprv7bCASBhrxHEx5VZCuwyx9o3daYgY5xYG93cZaBGfds4164KLzGwpFzxgK6Xq32kfuNkF4UeKKbSLSYDg8hoxCPgJQ5KXZvByPm22ariW1jr/<6;7>/*,{pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/<1;2;3>/0/*),pk(qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/<3;4;5>/*)})", "tr(qpub3pBWqhEkneoFHydg1yWxWvzN8aX2VRG7WGYANZgHCCayxreVXpG4ooHAAPMP4qYu6Qe7PmNK1ziiegEs6CeLANwtyrnLEKudWu9tRfGotuY/<6;7>/*,{pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/<1;2;3>/0/*),pk(qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/<3;4;5>/*)})", "tr(): Multipath internal key mismatches multipath subscripts lengths");
    CheckUnparsable("sh(multi(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/<1;2;3>/0/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0/*,qprv7bCASBhrxHEx6f97XnJTwmCNnXmBBc84W2UqGc5RqMZpPqtnphNr3GLnX1xraKFSmPED5qDZJoZJcD3jpVRJhHp4nrtLCkT49MYuBsuStmj/0/0/<3;4>/*))", "sh(multi(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/<1;2;3>/0/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/*,qpub3pBWqhEkneoFK9DadoqUJu97LZbfb4qusFQS4zV3Ph6oGeDwNEh6b4fGNK1DGDzxopPATi7j2CZNR4vsrnh7Q5Rq4HVjMU9uhw2mrLTXLnP/0/0/<3;4>/*))", "multi(): Multipath derivation paths have mismatched lengths");
    CheckUnparsable("wpkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<0>/*)", "wpkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<0>/*)", "wpkh(): Multipath key path specifiers must have at least two items");
    CheckUnparsable("wsh(andor(pk(qprv7gSQo2p9xZ1FBZRpPkZw4szqNbAd8TcPCTdEJMikUW9Xshr4D26jZCMfAzHK6TYUAGuAskdmxUAdDb6s8mk42bnh4yF7AmneuqwsLr6HjGQ/0'/<0;1;2;3>/*),older(10000),pk(qprv7gKkTCQiNn9SsYJhcwP5gJZPFwsy5fxebYTKnGxVdca49chFn2TMFnJFncap2QA7L2WAKTdgFfNncFfEMbT5si5hQVFKrcbiNspeR1gEAoa/8/<0;1;2>/*)))", "wsh(andor(pk(qpub3uRmCYM3nvZYQ3WHVn6wS1wZvd17XvLEZgYq6k8N2qgWkWBCkZQz6zg92Fs6QSLToTvJjprmNYHAn6cdBVUrzTXdPLT91EuJuNeMEjrKocS/0'/<0;1;2;3>/*),older(10000),pk(qpub3uK6rhwcD9hk62PAixv63SW7oyiTV8gVxmNvafN7Bx732R2QKZmboacjdtRDbigCegjeoiCs3m9tde9NtHkFYtVNmKL2iqNe9QUai7xmcmZ/8/<0;1;2>/*)))", "Miniscript: Multipath derivation paths have mismatched lengths");
    CheckUnparsable("wpkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<>/*)", "wpkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<>/*)", "wpkh(): Multipath key path specifiers must have at least two items");
    CheckUnparsable("wpkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<0/*)", "wpkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<0/*)", "wpkh(): Key path value '<0' is not a valid uint32");
    CheckUnparsable("wpkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/0>/*)", "wpkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/0>/*)", "wpkh(): Key path value '0>' is not a valid uint32");
    CheckUnparsable("wpkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<0;>/*)", "wpkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<0;>/*)", "wpkh(): Key path value '' is not a valid uint32");
    CheckUnparsable("wpkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<;1>/*)", "wpkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<;1>/*)", "wpkh(): Key path value '' is not a valid uint32");
    CheckUnparsable("wpkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<0;1;>/*)", "wpkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<0;1;>/*)", "wpkh(): Key path value '' is not a valid uint32");
    CheckUnparsable("wpkh(qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/<1;1>/*)", "wpkh(qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/<1;1>/*)", "wpkh(): Duplicated key path value 1 in multipath specifier");

    // Multisig constructions
    Check("multi(1,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW,6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "multi(1,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "multi(1,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", SIGNABLE, {{"512103a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd4104a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea23552ae"}}, std::nullopt, /*op_desc_id=*/uint256{"b147e25eb4a9d3da4e86ed8e970d817563ae2cb9c71a756b11cfdeb4dc11b70c"});
    Check("sortedmulti(1,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW,6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "sortedmulti(1,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "sortedmulti(1,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", SIGNABLE, {{"512103a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd4104a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea23552ae"}}, std::nullopt, /*op_desc_id=*/uint256{"62b59d1e32a62176ef7a17538f3b80c7d1afc53e5644eb753525bdb5d556486c"});
    Check("sortedmulti(1,6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "sortedmulti(1,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "sortedmulti(1,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", SIGNABLE, {{"512103a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd4104a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea23552ae"}}, std::nullopt);
    Check("sh(multi(2,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))", "sh(multi(2,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))", "sh(multi(2,[00000000/111h/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))", DEFAULT, {{"a91445a9a622a8b0a1269944be477640eedc447bbd8487"}}, OutputType::BASE58, /*op_desc_id=*/std::nullopt, {{0x8000006FUL,222},{0}});
    Check("sortedmulti(2,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/*,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0/0/*)", "sortedmulti(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/0/*)", "sortedmulti(2,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/*,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0/0/*)", RANGE, {{"5221025d5fc65ebb8d44a5274b53bac21ff8307fec2334a32df05553459f8b1f7fe1b62102fbd47cc8034098f0e6a94c6aeee8528abf0a2153a5d8e46d325b7284c046784652ae"}, {"52210264fd4d1f5dea8ded94c61e9641309349b62f27fbffe807291f664e286bfbe6472103f4ece6dfccfa37b211eb3d0af4d0c61dba9ef698622dc17eecdf764beeb005a652ae"}, {"5221022ccabda84c30bad578b13c89eb3b9544ce149787e5b538175b1d1ba259cbb83321024d902e1a2fc7a8755ab5b694c575fce742c48d9ff192e63df5193e4c7afe1f9c52ae"}}, std::nullopt, /*op_desc_id=*/std::nullopt, {{0}, {1}, {2}, {0, 0, 0}, {0, 0, 1}, {0, 0, 2}});
    Check("wsh(multi(2,qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483647'/0,qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/1/2/*,qprv7bCASBhrxHEx6L7PVYqAUQab5THvLxXiczrVJfUsDqxEbPvEH8ETVJ7EZJnpw1kKXnVKG9iksiJPtkMnKYUGtPg4p3JgDK1d9uTkTrLPCAt/10/20/30/40/*'))", "wsh(multi(2,qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483647'/0,qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*,qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP/10/20/30/40/*'))", "wsh(multi(2,[bd16bee5/2147483647h]qpub3sTG7QvzwzhG6r45VpPF4hLC9TP4PZHEDzsW4sgVELEVEqNgefjDoxb7d5kn5HJUHxBLVM3ownPJJGCY56jSce1cnEGxqCRfepQ1FF1R2XS/0,qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*,qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP/10/20/30/40/*h))", HARDENED | RANGE | DERIVE_HARDENED, {{"0020b92623201f3bb7c3771d45b2ad1d0351ea8fbf8cfe0a0e570264e1075fa1948f"},{"002036a08bbe4923af41cf4316817c93b8d37e2f635dd25cfff06bd50df6ae7ea203"},{"0020a96e7ab4607ca6b261bfe3245ffda9c746b28d3f59e83d34820ec0e2b36c139c"}}, OutputType::BECH32, /*op_desc_id=*/std::nullopt, {{0xFFFFFFFFUL,0}, {1,2,0}, {1,2,1}, {1,2,2}, {10, 20, 30, 40, 0x80000000UL}, {10, 20, 30, 40, 0x80000001UL}, {10, 20, 30, 40, 0x80000002UL}});
    Check("tr(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW,pk(Rcq8iCtmSRneMKTEgKLLGoXpGskFnvzC7k2w1NJNsXvxYS62H8Bh))", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,pk(669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0))", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,pk(669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0))", SIGNABLE | XONLY_KEYS, {{"512017cf18db381d836d8923b1bdb246cfcd818da1a9f0e6e7907f187f0b2f937754"}}, OutputType::BECH32M, /*op_desc_id=*/uint256{"af482b44c10b737b678e1091584818372e169e2dc5219e2877fabe1b83ae467b"});
    Check("tr(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW,multi_a(1,Rcq8iCtmSRneMKTEgKLLGoXpGskFnvzC7k2w1NJNsXvxYS62H8Bh))", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,multi_a(1,669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0))", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,multi_a(1,669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0))", SIGNABLE | XONLY_KEYS, {{"5120eb5bd3894327d75093891cc3a62506df7d58ec137fcd104cdd285d67816074f3"}}, OutputType::BECH32M);
    CheckUnparsable("sh(multi(16,Rcq8iCtmSRneMKTEgKLLGoXpGskFnvzC7k2w1NJNsXvxYS62H8Bh,RZJLiEENrFhmwvVYujC2TfUpuqFxuW6z388q17ByCfpzHPkMsLVv,RaqeGqQZKnn8ucRiVy6aWav7fLPuMTR625AoAzSm9fNMfZMPA7Q4,ReDS6mbe6pPAbQSZJrnLhrbaERfhhvg41sT38og4FEtoTMRSJWuC,Rdqi2PyTRrZUmJ2ULqXgC76QDhzCkCThbfGGThz3FJ4h7KG5i3ef,RaFA6a8dkv6URF1GRCpvYVmV2i2yqiT7ypvVvyCVipAFiWKgLxV4,Rhgb8rx5SbRt791fSf6RAXPmK6ETwJmupVKGUFtY2Efd3WvTp4Ri,RcH6Cdwnt2vQa8LKHg1tkLMsCjQm6SoPiGkLkNbVsuFxsPCX3NdZ,RfpFCjVVuskHX6oGkRZ1SnweaKH6pTCx7TEv7uqE6ssFKJodW1C7,RbmFXAnpAbMvkRpxWNmLiSeBgwYqYh8nFjvA6BeHFSrMHCfPb7uo,RZudhR1c8JNcSZV4NzLz9AemyyRhXpP9cUQaWHgnvKJcryK8tYih,RcESKQPYoB7vfJkrM2zLw4tsCMbQuDYGwCwWnHEJBpKF4nMNS3Sy,Rcin5Ke8X4AAnEhGVjm7vp95difJQTR1NawVngHv7JKErj7Sdh4r,RbDVBXfF6xMV6Vf5d3LybNXAtKqohVaz7Pxyuxk7Bm5tLr26Eh8S,RcLBNHR7xGD7M3XS33oJMbnCSwkhSoUZtHscXtYFCUgRtar1x3Xm,RdzZ1zfASheZMRJjcwqq41zTZRbsZSq3GLtVkoM35wqXnQLk2pbJ))","sh(multi(16,03669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0,0260b2003c386519fc9eadf2b5cf124dd8eea4c4e68d5e154050a9346ea98ce600,0362a74e399c39ed5593852a30147f2959b56bb827dfa3e60e464b02ccf87dc5e8,0261345b53de74a4d721ef877c255429961b7e43714171ac06168d7e08c542a8b8,02da72e8b46901a65d4374fe6315538d8f368557dda3a1dcf9ea903f3afe7314c8,0318c82dd0b53fd3a932d16e0ba9e278fcc937c582d5781be626ff16e201f72286,0297ccef1ef99f9d73dec9ad37476ddb232f1238aff877af19e72ba04493361009,02e502cfd5c3f972fe9a3e2a18827820638f96b6f347e54d63deb839011fd5765d,03e687710f0e3ebe81c1037074da939d409c0025f17eb86adb9427d28f0f7ae0e9,02c04d3a5274952acdbc76987f3184b346a483d43be40874624b29e3692c1df5af,02ed06e0f418b5b43a7ec01d1d7d27290fa15f75771cb69b642a51471c29c84acd,036d46073cbb9ffee90473f3da429abc8de7f8751199da44485682a989a4bebb24,02f5d1ff7c9029a80a4e36b9a5497027ef7f3e73384a4a94fbfe7c4e9164eec8bc,02e41deffd1b7cce11cde209a781adcffdabd1b91c0ba0375857a2bfd9302419f3,02d76625f7956a7fc505ab02556c23ee72d832f1bac391bcd2d3abce5710a13d06,0399eb0a5487515802dc14544cf10b3666623762fbed2ec38a3975716e2c29c232))", "P2SH script is too large, 547 bytes is larger than 520 bytes"); // P2SH does not fit 16 compressed pubkeys in a redeemscript
    CheckUnparsable("wsh(multi(2,[aaaaaaaa][aaaaaaaa]qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483647'/0,qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/1/2/*,qprv7bCASBhrxHEx6L7PVYqAUQab5THvLxXiczrVJfUsDqxEbPvEH8ETVJ7EZJnpw1kKXnVKG9iksiJPtkMnKYUGtPg4p3JgDK1d9uTkTrLPCAt/10/20/30/40/*'))", "wsh(multi(2,[aaaaaaaa][aaaaaaaa]qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483647h/0,qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*,qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP/10/20/30/40/*h))", "Multi: Multiple ']' characters found for a single pubkey"); // Double key origin descriptor
    CheckUnparsable("wsh(multi(2,[aaaagaaa]qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483647'/0,qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/1/2/*,qprv7bCASBhrxHEx6L7PVYqAUQab5THvLxXiczrVJfUsDqxEbPvEH8ETVJ7EZJnpw1kKXnVKG9iksiJPtkMnKYUGtPg4p3JgDK1d9uTkTrLPCAt/10/20/30/40/*'))", "wsh(multi(2,[aaagaaaa]qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483647h/0,qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*,qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP/10/20/30/40/*h))", "Multi: Fingerprint 'aaagaaaa' is not hex"); // Non hex fingerprint
    CheckUnparsable("wsh(multi(2,[aaaaaaaa],qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/1/2/*,qprv7bCASBhrxHEx6L7PVYqAUQab5THvLxXiczrVJfUsDqxEbPvEH8ETVJ7EZJnpw1kKXnVKG9iksiJPtkMnKYUGtPg4p3JgDK1d9uTkTrLPCAt/10/20/30/40/*'))", "wsh(multi(2,[aaaaaaaa],qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*,qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP/10/20/30/40/*h))", "Multi: No key provided"); // No public key with origin
    CheckUnparsable("wsh(multi(2,[aaaaaaa]qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483647'/0,qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/1/2/*,qprv7bCASBhrxHEx6L7PVYqAUQab5THvLxXiczrVJfUsDqxEbPvEH8ETVJ7EZJnpw1kKXnVKG9iksiJPtkMnKYUGtPg4p3JgDK1d9uTkTrLPCAt/10/20/30/40/*'))", "wsh(multi(2,[aaaaaaa]qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483647h/0,qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*,qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP/10/20/30/40/*h))", "Multi: Fingerprint is not 4 bytes (7 characters instead of 8 characters)"); // Too short fingerprint
    CheckUnparsable("wsh(multi(2,[aaaaaaaaa]qprv7bCASBhrxHEx5wcibhuSvcJMKeSM2anYJaeDbcDhCE56tNwRTH1PvCGpuNZt7XJQmqEk1SBD9pLazH5tMxryVkzqGTNvMevMjA5ypsLY4zb/2147483647'/0,qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/1/2/*,qprv7bCASBhrxHEx6L7PVYqAUQab5THvLxXiczrVJfUsDqxEbPvEH8ETVJ7EZJnpw1kKXnVKG9iksiJPtkMnKYUGtPg4p3JgDK1d9uTkTrLPCAt/10/20/30/40/*'))", "wsh(multi(2,[aaaaaaaaa]qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/2147483647h/0,qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/1/2/*,qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP/10/20/30/40/*h))", "Multi: Fingerprint is not 4 bytes (9 characters instead of 8 characters)"); // Too long fingerprint
    CheckUnparsable("multi(a,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW,6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "multi(a,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "Multi threshold 'a' is not valid"); // Invalid threshold
    CheckUnparsable("multi(0,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW,6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "multi(0,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "Multisig threshold cannot be 0, must be at least 1"); // Threshold of 0
    CheckUnparsable("multi(3,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW,6bQrLHJ1RKgeNZwhMYcNqnihahnNhDePtGY7ht9no3QbvssuXiQ)", "multi(3,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,04a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd5b8dec5235a0fa8722476c7709c02559e3aa73aa03918ba2d492eea75abea235)", "Multisig threshold cannot be larger than the number of keys; threshold is 3 but only 2 keys specified"); // Threshold larger than number of keys
    CheckUnparsable("multi(3,Rcq8iCtmSRneMKTEgKLLGoXpGskFnvzC7k2w1NJNsXvxYS62H8Bh,RZJLiEENrFhmwvVYujC2TfUpuqFxuW6z388q17ByCfpzHPkMsLVv,RaqeGqQZKnn8ucRiVy6aWav7fLPuMTR625AoAzSm9fNMfZMPA7Q4,ReDS6mbe6pPAbQSZJrnLhrbaERfhhvg41sT38og4FEtoTMRSJWuC)", "multi(3,03669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0,0260b2003c386519fc9eadf2b5cf124dd8eea4c4e68d5e154050a9346ea98ce600,0362a74e399c39ed5593852a30147f2959b56bb827dfa3e60e464b02ccf87dc5e8,0261345b53de74a4d721ef877c255429961b7e43714171ac06168d7e08c542a8b8)", "Cannot have 4 pubkeys in bare multisig; only at most 3 pubkeys"); // Threshold larger than number of keys
    CheckUnparsable("sh(multi(16,Rcq8iCtmSRneMKTEgKLLGoXpGskFnvzC7k2w1NJNsXvxYS62H8Bh,RZJLiEENrFhmwvVYujC2TfUpuqFxuW6z388q17ByCfpzHPkMsLVv,RaqeGqQZKnn8ucRiVy6aWav7fLPuMTR625AoAzSm9fNMfZMPA7Q4,ReDS6mbe6pPAbQSZJrnLhrbaERfhhvg41sT38og4FEtoTMRSJWuC,Rdqi2PyTRrZUmJ2ULqXgC76QDhzCkCThbfGGThz3FJ4h7KG5i3ef,RaFA6a8dkv6URF1GRCpvYVmV2i2yqiT7ypvVvyCVipAFiWKgLxV4,Rhgb8rx5SbRt791fSf6RAXPmK6ETwJmupVKGUFtY2Efd3WvTp4Ri,RcH6Cdwnt2vQa8LKHg1tkLMsCjQm6SoPiGkLkNbVsuFxsPCX3NdZ,RfpFCjVVuskHX6oGkRZ1SnweaKH6pTCx7TEv7uqE6ssFKJodW1C7,RbmFXAnpAbMvkRpxWNmLiSeBgwYqYh8nFjvA6BeHFSrMHCfPb7uo,RZudhR1c8JNcSZV4NzLz9AemyyRhXpP9cUQaWHgnvKJcryK8tYih,RcESKQPYoB7vfJkrM2zLw4tsCMbQuDYGwCwWnHEJBpKF4nMNS3Sy,Rcin5Ke8X4AAnEhGVjm7vp95difJQTR1NawVngHv7JKErj7Sdh4r,RbDVBXfF6xMV6Vf5d3LybNXAtKqohVaz7Pxyuxk7Bm5tLr26Eh8S,RcLBNHR7xGD7M3XS33oJMbnCSwkhSoUZtHscXtYFCUgRtar1x3Xm,RdzZ1zfASheZMRJjcwqq41zTZRbsZSq3GLtVkoM35wqXnQLk2pbJ,RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))","sh(multi(16,03669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0,0260b2003c386519fc9eadf2b5cf124dd8eea4c4e68d5e154050a9346ea98ce600,0362a74e399c39ed5593852a30147f2959b56bb827dfa3e60e464b02ccf87dc5e8,0261345b53de74a4d721ef877c255429961b7e43714171ac06168d7e08c542a8b8,02da72e8b46901a65d4374fe6315538d8f368557dda3a1dcf9ea903f3afe7314c8,0318c82dd0b53fd3a932d16e0ba9e278fcc937c582d5781be626ff16e201f72286,0297ccef1ef99f9d73dec9ad37476ddb232f1238aff877af19e72ba04493361009,02e502cfd5c3f972fe9a3e2a18827820638f96b6f347e54d63deb839011fd5765d,03e687710f0e3ebe81c1037074da939d409c0025f17eb86adb9427d28f0f7ae0e9,02c04d3a5274952acdbc76987f3184b346a483d43be40874624b29e3692c1df5af,02ed06e0f418b5b43a7ec01d1d7d27290fa15f75771cb69b642a51471c29c84acd,036d46073cbb9ffee90473f3da429abc8de7f8751199da44485682a989a4bebb24,02f5d1ff7c9029a80a4e36b9a5497027ef7f3e73384a4a94fbfe7c4e9164eec8bc,02e41deffd1b7cce11cde209a781adcffdabd1b91c0ba0375857a2bfd9302419f3,02d76625f7956a7fc505ab02556c23ee72d832f1bac391bcd2d3abce5710a13d06,0399eb0a5487515802dc14544cf10b3666623762fbed2ec38a3975716e2c29c232,03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", "P2SH script is too large, 581 bytes is larger than 520 bytes"); // Cannot have more than 15 keys in a P2SH multisig, or we exceed maximum push size
    Check("wsh(multi(20,Rcq8iCtmSRneMKTEgKLLGoXpGskFnvzC7k2w1NJNsXvxYS62H8Bh,RZJLiEENrFhmwvVYujC2TfUpuqFxuW6z388q17ByCfpzHPkMsLVv,RaqeGqQZKnn8ucRiVy6aWav7fLPuMTR625AoAzSm9fNMfZMPA7Q4,ReDS6mbe6pPAbQSZJrnLhrbaERfhhvg41sT38og4FEtoTMRSJWuC,Rdqi2PyTRrZUmJ2ULqXgC76QDhzCkCThbfGGThz3FJ4h7KG5i3ef,RaFA6a8dkv6URF1GRCpvYVmV2i2yqiT7ypvVvyCVipAFiWKgLxV4,Rhgb8rx5SbRt791fSf6RAXPmK6ETwJmupVKGUFtY2Efd3WvTp4Ri,RcH6Cdwnt2vQa8LKHg1tkLMsCjQm6SoPiGkLkNbVsuFxsPCX3NdZ,RfpFCjVVuskHX6oGkRZ1SnweaKH6pTCx7TEv7uqE6ssFKJodW1C7,RbmFXAnpAbMvkRpxWNmLiSeBgwYqYh8nFjvA6BeHFSrMHCfPb7uo,RZudhR1c8JNcSZV4NzLz9AemyyRhXpP9cUQaWHgnvKJcryK8tYih,RcESKQPYoB7vfJkrM2zLw4tsCMbQuDYGwCwWnHEJBpKF4nMNS3Sy,Rcin5Ke8X4AAnEhGVjm7vp95difJQTR1NawVngHv7JKErj7Sdh4r,RbDVBXfF6xMV6Vf5d3LybNXAtKqohVaz7Pxyuxk7Bm5tLr26Eh8S,RcLBNHR7xGD7M3XS33oJMbnCSwkhSoUZtHscXtYFCUgRtar1x3Xm,RdzZ1zfASheZMRJjcwqq41zTZRbsZSq3GLtVkoM35wqXnQLk2pbJ,RcTcMs981XVscqYgvYhYFS8pFRvT1YTNa67gTjF1UVGDCd1sfY63,RbmrYCpFno9LLL9g5GNMSwZYLcAiQjfzqbqoXoJqoReV8yNoY3ep,ReF29TyaN7zwfQnd9YttFdKpCXL3y8eZ9UoJ5DY1MqDK9ehaeora,RbNMHmdiijSNAFpwYiEThqn2WSa2GMiRCmeHznCKyho8jqnEcbU1))","wsh(multi(20,03669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0,0260b2003c386519fc9eadf2b5cf124dd8eea4c4e68d5e154050a9346ea98ce600,0362a74e399c39ed5593852a30147f2959b56bb827dfa3e60e464b02ccf87dc5e8,0261345b53de74a4d721ef877c255429961b7e43714171ac06168d7e08c542a8b8,02da72e8b46901a65d4374fe6315538d8f368557dda3a1dcf9ea903f3afe7314c8,0318c82dd0b53fd3a932d16e0ba9e278fcc937c582d5781be626ff16e201f72286,0297ccef1ef99f9d73dec9ad37476ddb232f1238aff877af19e72ba04493361009,02e502cfd5c3f972fe9a3e2a18827820638f96b6f347e54d63deb839011fd5765d,03e687710f0e3ebe81c1037074da939d409c0025f17eb86adb9427d28f0f7ae0e9,02c04d3a5274952acdbc76987f3184b346a483d43be40874624b29e3692c1df5af,02ed06e0f418b5b43a7ec01d1d7d27290fa15f75771cb69b642a51471c29c84acd,036d46073cbb9ffee90473f3da429abc8de7f8751199da44485682a989a4bebb24,02f5d1ff7c9029a80a4e36b9a5497027ef7f3e73384a4a94fbfe7c4e9164eec8bc,02e41deffd1b7cce11cde209a781adcffdabd1b91c0ba0375857a2bfd9302419f3,02d76625f7956a7fc505ab02556c23ee72d832f1bac391bcd2d3abce5710a13d06,0399eb0a5487515802dc14544cf10b3666623762fbed2ec38a3975716e2c29c232,02bc2feaa536991d269aae46abb8f3772a5b3ad592314945e51543e7da84c4af6e,0318bf32e5217c1eb771a6d5ce1cd39395dff7ff665704f175c9a5451d95a2f2ca,02c681a6243f16208c2004bb81f5a8a67edfdd3e3711534eadeec3dcf0b010c759,0249fdd6b69768b8d84b4893f8ff84b36835c50183de20fcae8f366a45290d01fd))", "wsh(multi(20,03669b8afcec803a0d323e9a17f3ea8e68e8abe5a278020a929adbec52421adbd0,0260b2003c386519fc9eadf2b5cf124dd8eea4c4e68d5e154050a9346ea98ce600,0362a74e399c39ed5593852a30147f2959b56bb827dfa3e60e464b02ccf87dc5e8,0261345b53de74a4d721ef877c255429961b7e43714171ac06168d7e08c542a8b8,02da72e8b46901a65d4374fe6315538d8f368557dda3a1dcf9ea903f3afe7314c8,0318c82dd0b53fd3a932d16e0ba9e278fcc937c582d5781be626ff16e201f72286,0297ccef1ef99f9d73dec9ad37476ddb232f1238aff877af19e72ba04493361009,02e502cfd5c3f972fe9a3e2a18827820638f96b6f347e54d63deb839011fd5765d,03e687710f0e3ebe81c1037074da939d409c0025f17eb86adb9427d28f0f7ae0e9,02c04d3a5274952acdbc76987f3184b346a483d43be40874624b29e3692c1df5af,02ed06e0f418b5b43a7ec01d1d7d27290fa15f75771cb69b642a51471c29c84acd,036d46073cbb9ffee90473f3da429abc8de7f8751199da44485682a989a4bebb24,02f5d1ff7c9029a80a4e36b9a5497027ef7f3e73384a4a94fbfe7c4e9164eec8bc,02e41deffd1b7cce11cde209a781adcffdabd1b91c0ba0375857a2bfd9302419f3,02d76625f7956a7fc505ab02556c23ee72d832f1bac391bcd2d3abce5710a13d06,0399eb0a5487515802dc14544cf10b3666623762fbed2ec38a3975716e2c29c232,02bc2feaa536991d269aae46abb8f3772a5b3ad592314945e51543e7da84c4af6e,0318bf32e5217c1eb771a6d5ce1cd39395dff7ff665704f175c9a5451d95a2f2ca,02c681a6243f16208c2004bb81f5a8a67edfdd3e3711534eadeec3dcf0b010c759,0249fdd6b69768b8d84b4893f8ff84b36835c50183de20fcae8f366a45290d01fd))", SIGNABLE, {{"0020376bd8344b8b6ebe504ff85ef743eaa1aa9272178223bcb6887e9378efb341ac"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"2bb9d418ebdc3a75c465383985881527f3e5d6e520fb3efb152d4191b80e8412"}); // In P2WSH we can have up to 20 keys
    // Check for invalid nesting of structures
    CheckUnparsable("sh(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "sh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "A function is needed within P2SH"); // P2SH needs a script, not a key
    CheckUnparsable("sh(combo(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))", "sh(combo(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", "Can only have combo() at top level"); // Old must be top level
    CheckUnparsable("wsh(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)", "wsh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)", "A function is needed within P2WSH"); // P2WSH needs a script, not a key
    CheckUnparsable("wsh(wpkh(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW))", "wsh(wpkh(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", "Can only have wpkh() at top level"); // Cannot embed witness inside witness
    CheckUnparsable("wsh(sh(pk(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)))", "wsh(sh(pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)))", "Can only have sh() at top level"); // Cannot embed P2SH inside P2WSH
    CheckUnparsable("sh(sh(pk(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)))", "sh(sh(pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)))", "Can only have sh() at top level"); // Cannot embed P2SH inside P2SH
    CheckUnparsable("wsh(wsh(pk(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)))", "wsh(wsh(pk(03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)))", "Can only have wsh() at top level"); // Cannot embed P2WSH inside P2WSH

    // Checksums
    Check("sh(multi(2,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))#sy60zqm6", "sh(multi(2,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))#fa7mpk33", "sh(multi(2,[00000000/111h/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))#48dyuxk8", DEFAULT, {{"a91445a9a622a8b0a1269944be477640eedc447bbd8487"}}, OutputType::BASE58, /*op_desc_id=*/uint256{"e061a921fa329e252500a4c681d4912873d3bed9dc6d8777a501ff3bccea507f"}, {{0x8000006FUL,222},{0}});
    Check("sh(multi(2,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))", "sh(multi(2,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))", "sh(multi(2,[00000000/111h/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))", DEFAULT, {{"a91445a9a622a8b0a1269944be477640eedc447bbd8487"}}, OutputType::BASE58, /*op_desc_id=*/uint256{"e061a921fa329e252500a4c681d4912873d3bed9dc6d8777a501ff3bccea507f"}, {{0x8000006FUL,222},{0}});
    CheckUnparsable("sh(multi(2,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))#", "sh(multi(2,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))#", "Expected 8 character checksum, not 0 characters"); // Empty checksum
    CheckUnparsable("sh(multi(2,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))#sy60zqm6q", "sh(multi(2,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))#fa7mpk33q", "Expected 8 character checksum, not 9 characters"); // Too long checksum
    CheckUnparsable("sh(multi(2,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))#ggrsrxf", "sh(multi(2,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))#tjg09x5", "Expected 8 character checksum, not 7 characters"); // Too short checksum
    CheckUnparsable("sh(multi(3,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))#tjg09x5t", "sh(multi(3,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))#tjg09x5t", "Provided checksum 'tjg09x5t' does not match computed checksum '06smckpk'"); // Error in payload
    CheckUnparsable("sh(multi(2,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))#tjq09x4t", "sh(multi(2,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))#tjq09x4t", "Provided checksum 'tjq09x4t' does not match computed checksum 'fa7mpk33'"); // Error in checksum
    CheckUnparsable("sh(multi(2,[00000000/111'/222]qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0))##ggssrxfy", "sh(multi(2,[00000000/111'/222]qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0))##tjq09x4t", "Multiple '#' symbols"); // Error in checksum

    // Addr and raw tests
    CheckUnparsable("", "addr(asdf)", "Address is not valid"); // Invalid address
    CheckUnparsable("", "raw(asdf)", "Raw script is not hex"); // Invalid script
    CheckUnparsable("", "raw(Ü)#00000000", "Invalid characters in payload"); // Invalid chars

    Check(
        "rawtr(qprv7eTuhuPpSJ52cDD1jVHKzQhsgcBbjZGprkPamRF9Gdw4aMPQ2YMU52r5Usqgkb9FBVUZuSawaykiLnihvFN2RzAEGdnyk966rZzYCCKTA5X/86'/1'/0'/1/*)#6c7emgls",
        "rawtr(qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/86'/1'/0'/1/*)#gwsuwnrv",
        "rawtr([5a61ff8e/86h/1h/0h]qpub3x4igwSyNZDp9kbyXmUWJfW3z2e6BAyb2JfrkZ44jH4VhxnVGNnHY1PkHj7SVxTrw4iS59GmVaTsLVnzrkEtXgayt5GPoi8pxihDAZPnnzf/1/*)#apgmyv7h",
        RANGE | HARDENED | XONLY_KEYS,
        {{"51205172af752f057d543ce8e4a6f8dcf15548ec6be44041bfa93b72e191cfc8c1ee"}, {"51201b66f20b86f700c945ecb9ad9b0ad1662b73084e2bfea48bee02126350b8a5b1"}, {"512063e70f66d815218abcc2306aa930aaca07c5cde73b75127eb27b5e8c16b58a25"}},
        OutputType::BECH32M,
        /*op_desc_id=*/uint256{"ce45fc2ed0b2e9f79a11505fb63cc57ad4a2eb8102d7af6a4079159c382f4aac"},
        {{0x80000056, 0x80000001, 0x80000000, 1, 0}, {0x80000056, 0x80000001, 0x80000000, 1, 1}, {0x80000056, 0x80000001, 0x80000000, 1, 2}});

    Check(
        "rawtr(RgtGk6v4rMTyxCQR7gX9QLo47m1GkWp41HaLbSmo8DdBASNdVmxW)",
        "rawtr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)",
        "rawtr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd)",
        SIGNABLE | XONLY_KEYS,
        {{"5120a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd"}},
        OutputType::BECH32M,
        /*op_desc_id=*/uint256{"5ba3f7d83cee4795df00e0eaa5070a3e164283c5fc6e8586fd710eaa7a4168ec"});

    CheckUnparsable(
        "",
        "rawtr(qpub68FQ9imX6mCWacw6eNRjaa8q8ynnHmUd5i7MVR51ZMPP5JycyfVHSLQVFPHMYiTybWJnSBL2tCBpy6aJTR2DYrshWYfwAxs8SosGXd66d8/*, qpub3sY5hNhwg3qZqYosLU96KqRDeyb4JzJN5EMVK71TPoCwvgpP6zzMRw6Ej9HY1NSD2y8NtNDX1VjG2nKhDiUYMQYU456TaMVcPAuQHKsGKte/*)",
        "rawtr(): only one key expected.");

    // A 2of4 but using a direct push rather than OP_2
    CScript nonminimalmultisig;
    CKey keys[4];
    nonminimalmultisig << std::vector<unsigned char>{2};
    for (int i = 0; i < 4; i++) {
        keys[i].MakeNewKey(true);
        nonminimalmultisig << ToByteVector(keys[i].GetPubKey());
    }
    nonminimalmultisig << 4 << OP_CHECKMULTISIG;
    CheckInferRaw(nonminimalmultisig);

    // A 2of4 but using a direct push rather than OP_4
    nonminimalmultisig.clear();
    nonminimalmultisig << 2;
    for (int i = 0; i < 4; i++) {
        keys[i].MakeNewKey(true);
        nonminimalmultisig << ToByteVector(keys[i].GetPubKey());
    }
    nonminimalmultisig << std::vector<unsigned char>{4} << OP_CHECKMULTISIG;
    CheckInferRaw(nonminimalmultisig);

    // Miniscript tests

    // Invalid checksum
    CheckUnparsable("wsh(and_v(vc:andor(pk(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t),pk_k(RaBEvMRTNn4qi9stUjS2EgyA28qteYxCwsgbVDVorvuZcKT1ca6r),and_v(v:older(1),pk_k(RgpzUMciB4EW49wLv4nkA4Cr6R9fnXsQXokpuNvWtuwh6besz5wm))),after(10)))#abcdef12", "wsh(and_v(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(02aa27e5eb2c185e87cd1dbc3e0efc9cb1175235e0259df1713424941c3cb40402))),after(10)))#abcdef12", "Provided checksum 'abcdef12' does not match computed checksum 'tyzp6a7p'");
    // Only p2wsh or tr contexts are valid
    CheckUnparsable("sh(and_v(vc:andor(pk(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t),pk_k(RaBEvMRTNn4qi9stUjS2EgyA28qteYxCwsgbVDVorvuZcKT1ca6r),and_v(v:older(1),pk_k(RgpzUMciB4EW49wLv4nkA4Cr6R9fnXsQXokpuNvWtuwh6besz5wm))),after(10)))", "sh(and_v(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(02aa27e5eb2c185e87cd1dbc3e0efc9cb1175235e0259df1713424941c3cb40402))),after(10)))", "Miniscript expressions can only be used in wsh or tr.");
    CheckUnparsable("tr(and_v(vc:andor(pk(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t),pk_k(RaBEvMRTNn4qi9stUjS2EgyA28qteYxCwsgbVDVorvuZcKT1ca6r),and_v(v:older(1),pk_k(RgpzUMciB4EW49wLv4nkA4Cr6R9fnXsQXokpuNvWtuwh6besz5wm))),after(10)))", "tr(and_v(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(02aa27e5eb2c185e87cd1dbc3e0efc9cb1175235e0259df1713424941c3cb40402))),after(10)))", "tr(): key 'and_v(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(02aa27e5eb2c185e87cd1dbc3e0efc9cb1175235e0259df1713424941c3cb40402))),after(10))' is not valid");
    CheckUnparsable("raw(and_v(vc:andor(pk(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t),pk_k(RaBEvMRTNn4qi9stUjS2EgyA28qteYxCwsgbVDVorvuZcKT1ca6r),and_v(v:older(1),pk_k(RgpzUMciB4EW49wLv4nkA4Cr6R9fnXsQXokpuNvWtuwh6besz5wm))),after(10)))", "sh(and_v(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(02aa27e5eb2c185e87cd1dbc3e0efc9cb1175235e0259df1713424941c3cb40402))),after(10)))", "Miniscript expressions can only be used in wsh or tr.");
    CheckUnparsable("", "tr(034D2224bbbbbbbbbbcbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb40,{{{{{{{{{{{{{{{{{{{{{{multi(1,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/967808'/9,qprv7jbyHVLdbEW3AeKvx5F5nmAAKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVzmPQMA/968/2/5/8/5/2/5/58/58/2/5/5/5/58/588/2/6/8/5/2/8/2/5/8/2/58/2/5/8/5/2/8/5/8/3/4/5/58/55/2/5/58/58/2/5/5/5/8/5/2/8/5/85/2/8/2/5/8/5/2/5/58/58/2/5/58/58/588/2/58/2/8/5/8/5/4/5/585/2/5/58/58/2/5/5/58/588/2/58/2/5/8/5/2/8/2/5/8/5/5/58/588/2/6/8/5/2/8/2/5/8/5/2/5/58/58/2/5/58/58/2/0/8/5/2/8/5/8/5/4/5/58/588/2/6/8/5/2/8/2/5/8/5/2/5/58/58/2/5/58/58/588/2/58/2/5/8/5/8/24/5/58/52/5/8/5/2/8/24/5/58/588/246/8/5/2/8/2/5/8/5/2/5/58/58/2/5/5/5/58/588/2/6/8/5/2/8/2/5/8/2/58/2/5/8/5/2/8/5/8/5/4/5/58/55/58/2/5/8/55/2/5/8/58/555/58/2/5/8/4//2/5/58/5w/2/5/8/5/2/4/5/58/5558'/2/5/58/58/2/5/5/58/588/2/58/2/5/8/5/2/8/2/5/8/5/5/8/58/2/5/58/58/2/5/8/9/588/2/58/2/5/8/5/2/8/5/8/5/4/5/58/588/2/6/8/5/2/8/2/5/8/5/2/5/58/58/2/5/5/58/588/2/58/2/5/8/5/2/82/5/8/5/5/58/52/6/8/5/2/8/{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{}{{{{{{{{{DDD2/5/8/5/2/5/58/58/2/5/58/58/588/2/58/2/8/5/8/5/4/5/58/588/2/6/8/5/2/8/2/5/8588/246/8/5/2DLDDDDDDDbbD3DDDD/8/2/5/8/5/2/5/58/58/2/5/5/5/58/588/2/6/8/5/2/8/2/5/8/2/58/2/5/8/5/2/8/5/8/3/4/5/58/55/2/5/58/58/2/5/5/5/8/5/2/8/5/85/2/8/2/5/8D)/5/2/5/58/58/2/5/58/58/58/588/2/58/2/5/8/5/25/58/58/2/5/58/58/2/5/8/9/588/2/58/2/6780,qprvA1RpRA33e1JQ7ifknakTFpgNXPmW2YvmhqLQYMmrj4xJXXWYpDPS3xz7iAxn8L39njGVyuoseXzU6rcxFLJ8HFsTjSyQbLYnMpCqE2VbFW/8/5/2/5/58678008')", "'multi(1,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P/967808'/9,qprv7jbyHVLdbEW3AeKvx5F5nmAAKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVzmPQMA/968/2/5/8/5/2/5/58/58/2/5/5/5/58/588/2/6/8/5/2/8/2/5/8/2/58/2/5/8/5/2/8/5/8/3/4/5/58/55/2/5/58/58/2/5/5/5/8/5/2/8/5/85/2/8/2/5/8/5/2/5/58/58/2/5/58/58/588/2/58/2/8/5/8/5/4/5/585/2/5/58/58/2/5/5/58/588/2/58/2/5/8/5/2/8/2/5/8/5/5/58/588/2/6/8/5/2/8/2/5/8/5/2/5/58/58/2/5/58/58/2/0/8/5/2/8/5/8/5/4/5/58/588/2/6/8/5/2/8/2/5/8/5/2/5/58/58/2/5/58/58/588/2/58/2/5/8/5/8/24/5/58/52/5/8/5/2/8/24/5/58/588/246/8/5/2/8/2/5/8/5/2/5/58/58/2/5/5/5/58/588/2/6/8/5/2/8/2/5/8/2/58/2/5/8/5/2/8/5/8/5/4/5/58/55/58/2/5/8/55/2/5/8/58/555/58/2/5/8/4//2/5/58/5w/2/5/8/5/2/4/5/58/5558'/2/5/58/58/2/5/5/58/588/2/58/2/5/8/5/2/8/2/5/8/5/5/8/58/2/5/58/58/2/5/8/9/588/2/58/2/5/8/5/2/8/5/8/5/4/5/58/588/2/6/8/5/2/8/2/5/8/5/2/5/58/58/2/5/5/58/588/2/58/2/5/8/5/2/82/5/8/5/5/58/52/6/8/5/2/8/{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{}{{{{{{{{{DDD2/5/8/5/2/5/58/58/2/5/58/58/588/2/58/2/8/5/8/5/4/5/58/588/2/6/8/5/2/8/2/5/8588/246/8/5/2DLDDDDDDDbbD3DDDD/8/2/5/8/5/2/5/58/58/2/5/5/5/58/588/2/6/8/5/2/8/2/5/8/2/58/2/5/8/5/2/8/5/8/3/4/5/58/55/2/5/58/58/2/5/5/5/8/5/2/8/5/85/2/8/2/5/8D)/5/2/5/58/58/2/5/58/58/58/588/2/58/2/5/8/5/25/58/58/2/5/58/58/2/5/8/9/588/2/58/2/6780,qprvA1RpRA33e1JQ7ifknakTFpgNXPmW2YvmhqLQYMmrj4xJXXWYpDPS3xz7iAxn8L39njGVyuoseXzU6rcxFLJ8HFsTjSyQbLYnMpCqE2VbFW/8/5/2/5/58678008'' is not a valid descriptor function");
    // No uncompressed keys allowed
    CheckUnparsable("", "wsh(and_v(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(049228de6902abb4f541791f6d7f925b10e2078ccb1298856e5ea5cc5fd667f930eac37a00cc07f9a91ef3c2d17bf7a17db04552ff90ac312a5b8b4caca6c97aa4))),after(10)))", "Uncompressed keys are not allowed");
    // No hybrid keys allowed
    CheckUnparsable("", "wsh(and_v(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(069228de6902abb4f541791f6d7f925b10e2078ccb1298856e5ea5cc5fd667f930eac37a00cc07f9a91ef3c2d17bf7a17db04552ff90ac312a5b8b4caca6c97aa4))),after(10)))", "Hybrid public keys are not allowed");
    // Insane at top level
    CheckUnparsable("wsh(and_b(vc:andor(pk(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t),pk_k(RaBEvMRTNn4qi9stUjS2EgyA28qteYxCwsgbVDVorvuZcKT1ca6r),and_v(v:older(1),pk_k(RgpzUMciB4EW49wLv4nkA4Cr6R9fnXsQXokpuNvWtuwh6besz5wm))),after(10)))", "wsh(and_b(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(02aa27e5eb2c185e87cd1dbc3e0efc9cb1175235e0259df1713424941c3cb40402))),after(10)))", "and_b(vc:andor(pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(02aa27e5eb2c185e87cd1dbc3e0efc9cb1175235e0259df1713424941c3cb40402))),after(10)) is invalid");
    // Invalid sub
    CheckUnparsable("wsh(and_v(vc:andor(v:pk_k(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t),pk_k(RaBEvMRTNn4qi9stUjS2EgyA28qteYxCwsgbVDVorvuZcKT1ca6r),and_v(v:older(1),pk_k(RgpzUMciB4EW49wLv4nkA4Cr6R9fnXsQXokpuNvWtuwh6besz5wm))),after(10)))", "wsh(and_v(vc:andor(v:pk_k(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),pk_k(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0),and_v(v:older(1),pk_k(02aa27e5eb2c185e87cd1dbc3e0efc9cb1175235e0259df1713424941c3cb40402))),after(10)))", "v:pk_k(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204) is invalid");
    // Insane subs
    CheckUnparsable("wsh(or_i(older(1),pk(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t)))", "wsh(or_i(older(1),pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204)))", "or_i(older(1),pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204)) is not sane: witnesses without signature exist");
    CheckUnparsable("wsh(or_b(sha256(cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),s:pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204)))", "wsh(or_b(sha256(cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),s:pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204)))", "or_b(sha256(cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),s:pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204)) is not sane: malleable witnesses exist");
    CheckUnparsable("wsh(and_b(and_b(older(1),a:older(100000000)),s:pk(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t)))", "wsh(and_b(and_b(older(1),a:older(100000000)),s:pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204)))", "and_b(older(1),a:older(100000000)) is not sane: contains mixes of timelocks expressed in blocks and seconds");
    CheckUnparsable("wsh(and_b(or_b(pkh(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t),s:pk(RaBEvMRTNn4qi9stUjS2EgyA28qteYxCwsgbVDVorvuZcKT1ca6r)),s:pk(RgiJjNspd8C3nfNSi2im3RFrALCDXw5ZzPVbjc8cEHGEAWSNWZ5t)))", "wsh(and_b(or_b(pkh(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),s:pk(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0)),s:pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204)))", "and_b(or_b(pkh(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204),s:pk(032707170c71d8f75e4ca4e3fce870b9409dcaf12b051d3bcadff74747fa7619c0)),s:pk(03cdabb7f2dce7bfbd8a0b9570c6fd1e712e5d64045e9d6b517b3d5072251dc204)) is not sane: contains duplicate public keys");
    // Valid with extended keys.
    Check("wsh(and_v(v:ripemd160(095ff41131e5946f3c85f79e44adbcf8e27e080e),multi(1,qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P,qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0)))", "wsh(and_v(v:ripemd160(095ff41131e5946f3c85f79e44adbcf8e27e080e),multi(1,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0)))", "wsh(and_v(v:ripemd160(095ff41131e5946f3c85f79e44adbcf8e27e080e),multi(1,qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ,qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0)))", DEFAULT, {{"0020acf425291b98a1d7e0d4690139442abc289175be32ef1f75945e339924246d73"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"17ce039c6baafc4c2772a7483c0971fdc0a1e0686420f22340308d6d6846e4fa"}, {{},{0}});
    // An exotic multisig, we can sign for both branches
    Check("wsh(thresh(1,pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P),a:pkh(qprv7dZNB9XzdecJGYAHMEjkBrK3teNpwJMiRQeip8w6QE5G7eJ9gv2DwvJ4h8eQQVrfJgYubJfBhyhkzULMAcbA2zhiDsrcJFY98thzSAGkE6i/0)))", "wsh(thresh(1,pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ),a:pkh(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0)))", "wsh(thresh(1,pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ),a:pkh(qpub3rYiaf4tU2AbV2EkTGGkYzFnSgDKLm5ZndaKcXLhxZcEzSdJETLUVicYYPtbi6hjjKskGj2p9iBJUprQWvChdvcBG25UVjxXWHcCy32bReF/0)))", SIGNABLE, {{"00204a4528fbc0947e02e921b54bd476fc8cc2ebb5c6ae2ccf10ed29fe2937fb6892"}}, OutputType::BECH32, /*op_desc_id=*/std::nullopt, {{},{0}});
    // We can sign for a script requiring the two kinds of timelock.
    // But if we don't set a sequence high enough, we'll fail.
    // And same for the nLockTime.
    // But if both are set to (at least) the required value, we'll succeed.
    // We can't sign for a script requiring a ripemd160 preimage without providing it.
    Check("wsh(and_v(v:ripemd160(d6b01bd7cf607eda8b3d7bc94e9d8eaa64dc7563),pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)))", "wsh(and_v(v:ripemd160(d6b01bd7cf607eda8b3d7bc94e9d8eaa64dc7563),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", "wsh(and_v(v:ripemd160(d6b01bd7cf607eda8b3d7bc94e9d8eaa64dc7563),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", SIGNABLE_FAILS, {{"0020aad2f05900b72e5c31e2288334399ea3cc3fb05bd4cc6968868510555e6b0d6a"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"06228087017617d685fed9fe179c914aa02f183bf1675b22b130d4ef6bcc341c"}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, {});
    // But if we provide it, we can.
    Check("wsh(and_v(v:ripemd160(d6b01bd7cf607eda8b3d7bc94e9d8eaa64dc7563),pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)))", "wsh(and_v(v:ripemd160(d6b01bd7cf607eda8b3d7bc94e9d8eaa64dc7563),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", "wsh(and_v(v:ripemd160(d6b01bd7cf607eda8b3d7bc94e9d8eaa64dc7563),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", SIGNABLE, {{"0020aad2f05900b72e5c31e2288334399ea3cc3fb05bd4cc6968868510555e6b0d6a"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"06228087017617d685fed9fe179c914aa02f183bf1675b22b130d4ef6bcc341c"}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, {{"d6b01bd7cf607eda8b3d7bc94e9d8eaa64dc7563"_hex_v_u8, "319aa1b4a8a7a6d7ea636cef44b9a38e92e7c3a1b1a3b72a3b1c40386936662e"_hex_v_u8}});
    // Same for sha256
    Check("wsh(and_v(v:sha256(549ea0a420740a37c8759cc763ad3c05877b7ca107c5531a9c8bada408a417e1),pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)))", "wsh(and_v(v:sha256(549ea0a420740a37c8759cc763ad3c05877b7ca107c5531a9c8bada408a417e1),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", "wsh(and_v(v:sha256(549ea0a420740a37c8759cc763ad3c05877b7ca107c5531a9c8bada408a417e1),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", SIGNABLE_FAILS, {{"002024f041377d4c69bd899fd716674cf0db35ace608faba2cc697956657aa74866a"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"b5996d5b5ec73723233a0c694e2f8ed6a584be0a6fcda0abad58017b107f6590"}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, {});
    Check("wsh(and_v(v:sha256(549ea0a420740a37c8759cc763ad3c05877b7ca107c5531a9c8bada408a417e1),pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)))", "wsh(and_v(v:sha256(549ea0a420740a37c8759cc763ad3c05877b7ca107c5531a9c8bada408a417e1),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", "wsh(and_v(v:sha256(549ea0a420740a37c8759cc763ad3c05877b7ca107c5531a9c8bada408a417e1),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", SIGNABLE, {{"002024f041377d4c69bd899fd716674cf0db35ace608faba2cc697956657aa74866a"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"b5996d5b5ec73723233a0c694e2f8ed6a584be0a6fcda0abad58017b107f6590"}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, {{"549ea0a420740a37c8759cc763ad3c05877b7ca107c5531a9c8bada408a417e1"_hex_v_u8, "319aa1b4a8a7a6d7ea636cef44b9a38e92e7c3a1b1a3b72a3b1c40386936662e"_hex_v_u8}});
    // Same for hash160
    Check("wsh(and_v(v:hash160(6b42b0c8ebf37c09f53c57a924e791f6d684125c),pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)))", "wsh(and_v(v:hash160(6b42b0c8ebf37c09f53c57a924e791f6d684125c),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", "wsh(and_v(v:hash160(6b42b0c8ebf37c09f53c57a924e791f6d684125c),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", SIGNABLE_FAILS, {{"00202b32261502317ae1178b9eaf0cde9f7431ee16de5caeab26f04c6f558bfd448a"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"5c9840f05ac1712705a111f008441f55ea6e10796590173d1c7c268f150e6c49"}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, {});
    Check("wsh(and_v(v:hash160(6b42b0c8ebf37c09f53c57a924e791f6d684125c),pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)))", "wsh(and_v(v:hash160(6b42b0c8ebf37c09f53c57a924e791f6d684125c),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", "wsh(and_v(v:hash160(6b42b0c8ebf37c09f53c57a924e791f6d684125c),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", SIGNABLE, {{"00202b32261502317ae1178b9eaf0cde9f7431ee16de5caeab26f04c6f558bfd448a"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"5c9840f05ac1712705a111f008441f55ea6e10796590173d1c7c268f150e6c49"}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, {{"6b42b0c8ebf37c09f53c57a924e791f6d684125c"_hex_v_u8, "319aa1b4a8a7a6d7ea636cef44b9a38e92e7c3a1b1a3b72a3b1c40386936662e"_hex_v_u8}});
    // Same for hash256
    Check("wsh(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)))", "wsh(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", "wsh(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", SIGNABLE_FAILS, {{"0020ccfb6311e42007921cf5d20ae916b6f5e9b9dbb81e6e7140ac1fa38385201d50"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"2968c3d5a3cd39cea13e5b4d1f009ba5bba03d084772bcf2d38b768cb9fbd391"}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, {});
    Check("wsh(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),pk(qprv7jbyHVLdbEW3AeKvx5F5oD1rKckNSCzHJ3gs4bWf52K99pox3cEVhXP9ZjFg8yThwFSrjadxRuWcurL1DTQ2hc6Ff56SVxpNTjmKVy8au5P)))", "wsh(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", "wsh(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),pk(qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ)))", SIGNABLE, {{"0020ccfb6311e42007921cf5d20ae916b6f5e9b9dbb81e6e7140ac1fa38385201d50"}}, OutputType::BECH32, /*op_desc_id=*/uint256{"2968c3d5a3cd39cea13e5b4d1f009ba5bba03d084772bcf2d38b768cb9fbd391"}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/CTxIn::SEQUENCE_FINAL, {{"b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6"_hex_v_u8, "319aa1b4a8a7a6d7ea636cef44b9a38e92e7c3a1b1a3b72a3b1c40386936662e"_hex_v_u8}});
    // Can have a Miniscript expression under tr() if it's alone.
    Check("tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,thresh(2,pk(RdQH5GKforWJ4SSQsejwoUExyFoMHNCf7YzJ62VqFS4sxz3xYSve),s:pk(Rc5fvKfDwD5c6s4hEkAJ2PnrhtadM87L6RAKQw6MWYkvsLYfVHxT),adv:older(42)))", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,thresh(2,pk(30a6069f344fb784a2b4c99540a91ee727c91e3a25ef6aae867d9c65b5f23529),s:pk(9918d400c1b8c3c478340a40117ced4054b6b58f48cdb3c89b836bdfee1f5766),adv:older(42)))", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,thresh(2,pk(30a6069f344fb784a2b4c99540a91ee727c91e3a25ef6aae867d9c65b5f23529),s:pk(9918d400c1b8c3c478340a40117ced4054b6b58f48cdb3c89b836bdfee1f5766),adv:older(42)))", MISSING_PRIVKEYS | XONLY_KEYS | SIGNABLE, {{"512033982eebe204dc66508e4b19cfc31b5ffc6e1bfcbf6e5597dfc2521a52270795"}}, OutputType::BECH32M);
    // Can have a pkh() expression alone as tr() script path (because pkh() is valid Miniscript).
    Check("tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,pkh(RdQH5GKforWJ4SSQsejwoUExyFoMHNCf7YzJ62VqFS4sxz3xYSve))", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,pkh(30a6069f344fb784a2b4c99540a91ee727c91e3a25ef6aae867d9c65b5f23529))", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,pkh(30a6069f344fb784a2b4c99540a91ee727c91e3a25ef6aae867d9c65b5f23529))", MISSING_PRIVKEYS | XONLY_KEYS | SIGNABLE, {{"51201e9875f690f5847404e4c5951e2f029887df0525691ee11a682afd37b608aad4"}}, OutputType::BECH32M);
    // Can have a Miniscript expression under tr() if it's part of a tree.
    Check("tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{{pkh(RbnS7u7FnvvU68wBNMe9AmLsoymz6Kpboe4jDJjXKw9r6vBLeUvk),pk(RfGkhzhuuXH45tXFc1Uk1dLgScFAUtMBJpCX5SUntDdvwSSUdp65)},thresh(1,pk(RdQH5GKforWJ4SSQsejwoUExyFoMHNCf7YzJ62VqFS4sxz3xYSve),s:pk(Rc5fvKfDwD5c6s4hEkAJ2PnrhtadM87L6RAKQw6MWYkvsLYfVHxT))})", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{{pkh(1c9bc926084382e76da33b5a52d17b1fa153c072aae5fb5228ecc2ccf89d79d5),pk(0dd6b52b192ab195558d22dd8437a9ec4519ee5ded496c0d55bc9b1a8b0e8c2b)},thresh(1,pk(30a6069f344fb784a2b4c99540a91ee727c91e3a25ef6aae867d9c65b5f23529),s:pk(9918d400c1b8c3c478340a40117ced4054b6b58f48cdb3c89b836bdfee1f5766))})", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{{pkh(1c9bc926084382e76da33b5a52d17b1fa153c072aae5fb5228ecc2ccf89d79d5),pk(0dd6b52b192ab195558d22dd8437a9ec4519ee5ded496c0d55bc9b1a8b0e8c2b)},thresh(1,pk(30a6069f344fb784a2b4c99540a91ee727c91e3a25ef6aae867d9c65b5f23529),s:pk(9918d400c1b8c3c478340a40117ced4054b6b58f48cdb3c89b836bdfee1f5766))})", MISSING_PRIVKEYS | XONLY_KEYS, {{"5120d8ea39b29de2b550b68bd2ada8b075c888c2b2df3290c7a35856482747848934"}}, OutputType::BECH32M);
    // Can have two Miniscripts in a Taproot with mixed private and public keys, and mixed ranged extended keys and raw keys.
    Check("tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(v:pk(qpub3tSkYxdC7Vr7ZVvYUmXRbFmQY7uAbvcWyACkMb32HARRzJiKAnFWXC3CQJgTH3JaPyMLemcb4EiCUMjqqJLFGgcQM12yrUML7SgTn3ryvR2/*),pk(02daf6e3477fc3906a1997820ed2940c8f5fa0942946d0368f981b001fdd85afcb)),and_v(v:pk(qprv7fNWzDmRKJN6vgqSQDEXv2pACxkSZYyYryyxs1LM4rXh2ATkxVjGfTZUZjxf4mBBxEHwxqzajr5X9LjQimuoofJZaQLVspe9zAWu8HDncug/*),pk(03272c0c1ae2c07528283b91ca57b45d2cc84e7960e1f17f58815372285f35e99a))})", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(v:pk(qpub3tSkYxdC7Vr7ZVvYUmXRbFmQY7uAbvcWyACkMb32HARRzJiKAnFWXC3CQJgTH3JaPyMLemcb4EiCUMjqqJLFGgcQM12yrUML7SgTn3ryvR2/*),pk(02daf6e3477fc3906a1997820ed2940c8f5fa0942946d0368f981b001fdd85afcb)),and_v(v:pk(qpub3tMsPjJK9fvQ9AuuWEmYHAktkzavy1hQECuZfPjxdC4ftxnuW33XDFsxR1jZ2QoLR15L8WWk1XAfPYLbPmKE1jmdcTqBExm9Yh5GcT7f6kW/*),pk(03272c0c1ae2c07528283b91ca57b45d2cc84e7960e1f17f58815372285f35e99a))})", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(v:pk(qpub3tSkYxdC7Vr7ZVvYUmXRbFmQY7uAbvcWyACkMb32HARRzJiKAnFWXC3CQJgTH3JaPyMLemcb4EiCUMjqqJLFGgcQM12yrUML7SgTn3ryvR2/*),pk(02daf6e3477fc3906a1997820ed2940c8f5fa0942946d0368f981b001fdd85afcb)),and_v(v:pk(qpub3tMsPjJK9fvQ9AuuWEmYHAktkzavy1hQECuZfPjxdC4ftxnuW33XDFsxR1jZ2QoLR15L8WWk1XAfPYLbPmKE1jmdcTqBExm9Yh5GcT7f6kW/*),pk(03272c0c1ae2c07528283b91ca57b45d2cc84e7960e1f17f58815372285f35e99a))})", MISSING_PRIVKEYS | XONLY_KEYS | RANGE | MIXED_PUBKEYS, {{"5120793185cd1a9a0bb710fa57df3845ac4ddf7df63b74beadce2573cbb0b508b3a4"}}, OutputType::BECH32M, /*op_desc_id=*/{}, {{}, {0}});
    // Can sign for a Miniscript expression containing a hash challenge inside a Taproot tree. (Fails without the
    // preimages and the sequence, passes with.)
    Check("tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),v:pk(RbnS7u7FnvvU68wBNMe9AmLsoymz6Kpboe4jDJjXKw9r6vBLeUvk)),older(42)),multi_a(2,adf586a32ad4b0674a86022b000348b681b4c97a811f67eefe4a6e066e55080c,RcvKi7QCUNJ7wEAVT47FF7EGkromqqxtg3UPgjafrgD1EY7vRhsz)})", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),v:pk(1c9bc926084382e76da33b5a52d17b1fa153c072aae5fb5228ecc2ccf89d79d5)),older(42)),multi_a(2,adf586a32ad4b0674a86022b000348b681b4c97a811f67eefe4a6e066e55080c,14fa4ad085cdee1e2fc73d491b36a96c192382b1d9a21108eb3533f630364f9f)})", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),v:pk(1c9bc926084382e76da33b5a52d17b1fa153c072aae5fb5228ecc2ccf89d79d5)),older(42)),multi_a(2,adf586a32ad4b0674a86022b000348b681b4c97a811f67eefe4a6e066e55080c,14fa4ad085cdee1e2fc73d491b36a96c192382b1d9a21108eb3533f630364f9f)})", MISSING_PRIVKEYS | XONLY_KEYS | SIGNABLE | SIGNABLE_FAILS, {{"512018c4b3ef1caface1d80be732c9847adce0cd7de4e3918bf9d32e0468cca8d1eb"}}, OutputType::BECH32M);
    Check("tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),v:pk(RbnS7u7FnvvU68wBNMe9AmLsoymz6Kpboe4jDJjXKw9r6vBLeUvk)),older(42)),multi_a(2,adf586a32ad4b0674a86022b000348b681b4c97a811f67eefe4a6e066e55080c,RcvKi7QCUNJ7wEAVT47FF7EGkromqqxtg3UPgjafrgD1EY7vRhsz)})", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),v:pk(1c9bc926084382e76da33b5a52d17b1fa153c072aae5fb5228ecc2ccf89d79d5)),older(42)),multi_a(2,adf586a32ad4b0674a86022b000348b681b4c97a811f67eefe4a6e066e55080c,14fa4ad085cdee1e2fc73d491b36a96c192382b1d9a21108eb3533f630364f9f)})", "tr(a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd,{and_v(and_v(v:hash256(b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6),v:pk(1c9bc926084382e76da33b5a52d17b1fa153c072aae5fb5228ecc2ccf89d79d5)),older(42)),multi_a(2,adf586a32ad4b0674a86022b000348b681b4c97a811f67eefe4a6e066e55080c,14fa4ad085cdee1e2fc73d491b36a96c192382b1d9a21108eb3533f630364f9f)})", MISSING_PRIVKEYS | XONLY_KEYS | SIGNABLE, {{"512018c4b3ef1caface1d80be732c9847adce0cd7de4e3918bf9d32e0468cca8d1eb"}}, OutputType::BECH32M, /*op_desc_id=*/{}, {{}}, /*spender_nlocktime=*/0, /*spender_nsequence=*/42, /*preimages=*/{{"b6ccde09697a397d2d4c0c0f30ad6111f6e9abfc16e54e91f4fd31d8d4e065c6"_hex_v_u8, "319aa1b4a8a7a6d7ea636cef44b9a38e92e7c3a1b1a3b72a3b1c40386936662e"_hex_v_u8}});

    // Basic sh(pkh()) with key origin
    CheckInferDescriptor("a9141a31ad23bf49c247dd531a623c2ef57da3c400c587", "sh(pkh([deadbeef/0h/0h/0]03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd))", {"76a9149a1c78a507689f6f54b847ad1cef1e614ee23f1e88ac"}, {{"03a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd", "deadbeef/0h/0h/0"}});
    // p2pk script with hybrid key must infer as raw()
    CheckInferDescriptor("41069228de6902abb4f541791f6d7f925b10e2078ccb1298856e5ea5cc5fd667f930eac37a00cc07f9a91ef3c2d17bf7a17db04552ff90ac312a5b8b4caca6c97aa4ac", "raw(41069228de6902abb4f541791f6d7f925b10e2078ccb1298856e5ea5cc5fd667f930eac37a00cc07f9a91ef3c2d17bf7a17db04552ff90ac312a5b8b4caca6c97aa4ac)", {}, {{"069228de6902abb4f541791f6d7f925b10e2078ccb1298856e5ea5cc5fd667f930eac37a00cc07f9a91ef3c2d17bf7a17db04552ff90ac312a5b8b4caca6c97aa4", ""}});
    // p2pkh script with hybrid key must infer as addr()
    CheckInferDescriptor("76a91445ff7c2327866472639d507334a9a00119dfd32688ac", "addr(QSz6nwMoRVHS4S3yqWtWWFnQ9zCPsLXy3K)", {}, {{"069228de6902abb4f541791f6d7f925b10e2078ccb1298856e5ea5cc5fd667f930eac37a00cc07f9a91ef3c2d17bf7a17db04552ff90ac312a5b8b4caca6c97aa4", ""}});
    // p2wpkh script with uncompressed key must infer as addr()
    CheckInferDescriptor("001422e363a523947a110d9a9eb114820de183aca313", "addr(hg1qyt3k8ffrj3apzrv6n6c3fqsduxp6egcnhj5ydv)", {}, {{"049228de6902abb4f541791f6d7f925b10e2078ccb1298856e5ea5cc5fd667f930eac37a00cc07f9a91ef3c2d17bf7a17db04552ff90ac312a5b8b4caca6c97aa4", ""}});
    // Infer pkh() from p2pkh with uncompressed key
    CheckInferDescriptor("76a914a31725c74421fadc50d35520ab8751ed120af80588ac", "pkh(04c56fe4a92d401bcbf1b3dfbe4ac3dac5602ca155a3681497f02c1b9a733b92d704e2da6ec4162e4846af9236ef4171069ac8b7f8234a8405b6cadd96f34f5a31)", {}, {{"04c56fe4a92d401bcbf1b3dfbe4ac3dac5602ca155a3681497f02c1b9a733b92d704e2da6ec4162e4846af9236ef4171069ac8b7f8234a8405b6cadd96f34f5a31", ""}});
    // Infer pk() from p2pk with uncompressed key
    CheckInferDescriptor("4104032540df1d3c7070a8ab3a9cdd304dfc7fd1e6541369c53c4c3310b2537d91059afc8b8e7673eb812a32978dabb78c40f2e423f7757dca61d11838c7aeeb5220ac", "pk(04032540df1d3c7070a8ab3a9cdd304dfc7fd1e6541369c53c4c3310b2537d91059afc8b8e7673eb812a32978dabb78c40f2e423f7757dca61d11838c7aeeb5220)", {}, {{"04032540df1d3c7070a8ab3a9cdd304dfc7fd1e6541369c53c4c3310b2537d91059afc8b8e7673eb812a32978dabb78c40f2e423f7757dca61d11838c7aeeb5220", ""}});
}


// Upstream's TRDescriptor::MaxSatisfactionWeight carried a FIXME: it assumed a
// key-path spend and returned 1 + 65 for every tr(), "which can lead to very large
// underestimations". That was tolerable while the number only informed coin
// selection. It is not tolerable now: the Quicksilver per-tx proof-of-work is priced
// by serialized bytes, and the sender grinds against this estimate BEFORE its
// signatures exist. An underestimate is an under-ground proof, and an under-ground
// proof is a transaction the network rejects -- with no way to recover in the
// offline signing flow, where the signing vault never sees the broadcast
// transaction. So the estimate must be an upper bound over EVERY spending path.
BOOST_AUTO_TEST_CASE(tr_max_satisfaction_covers_script_paths)
{
    const std::string XONLY{"a34b99f22c790c4e36b2b3c2c35a36db06226e41c692fc82b8b56ac1c540c5bd"};
    const auto parse = [](const std::string& desc) {
        FlatSigningProvider keys;
        std::string error;
        auto descs = Parse(desc, keys, error);
        BOOST_REQUIRE_MESSAGE(!descs.empty(), desc + ": " + error);
        return std::move(descs[0]);
    };

    // Key path only: one signature, pushed. Unchanged from before.
    const auto keypath = parse("tr(" + XONLY + ")");
    BOOST_CHECK_EQUAL(*keypath->MaxSatisfactionWeight(true), 1 + 65);
    BOOST_CHECK_EQUAL(*keypath->MaxSatisfactionElems(), 1);

    // One leaf at depth 0. The script path costs
    //   signature        1 + 65 = 66
    //   leaf script      1 + (1 + 32 + 1) = 35   [<xonly> OP_CHECKSIG]
    //   control block    1 + (33 + 32*0) = 34
    // = 135, which is what a sender must be charged for, not 66.
    const auto one_leaf = parse("tr(" + XONLY + ",pk(" + XONLY + "))");
    BOOST_CHECK_EQUAL(*one_leaf->MaxSatisfactionWeight(true), 66 + 35 + 34);
    BOOST_CHECK_EQUAL(*one_leaf->MaxSatisfactionElems(), 1 + 2);

    // Two leaves at depth 1: the control block grows by one 32-byte hash per level,
    // so depth is part of the bound and cannot be assumed away.
    const auto two_leaves = parse("tr(" + XONLY + ",{pk(" + XONLY + "),pk(" + XONLY + ")})");
    BOOST_CHECK_EQUAL(*two_leaves->MaxSatisfactionWeight(true), 66 + 35 + (1 + 33 + 32));
    BOOST_CHECK_EQUAL(*two_leaves->MaxSatisfactionElems(), 1 + 2);

    // The bound must be a MAXIMUM over paths, not the first or the last leaf: a big
    // leaf alongside a small one must produce the big leaf's cost.
    const auto mixed = parse("tr(" + XONLY + ",{pk(" + XONLY + "),multi_a(1," + XONLY + "," + XONLY + ")})");
    BOOST_CHECK_GT(*mixed->MaxSatisfactionWeight(true), *two_leaves->MaxSatisfactionWeight(true));
}

BOOST_AUTO_TEST_SUITE_END()
