#!/usr/bin/env python3
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Check that removed dead surfaces do not return:
#   * BIP70 / IP-payment GUI objects
#   * dead isLegacy / legacy_vault abstractions
#   * fee-era fuzz hooks, effective-value wrappers, and fee-era prose
#   * prelaunch compatibility aliases, dictionary-form outputs, camelCase fund options
#   * pre-Quicksilver database formats and chain-specific duplicate-tx exceptions
#   * stale JSON-RPC, qpub, derivation-path, and lab-host public surfaces
#   * the incomplete Erlay/BIP-330 txreconciliation surface and its minisketch dependency
#   * sandbox misnamed as Regtest
#   * public Quicksilver-facing xpub/xprv wording
#   * registered-but-unused RPC / config / vault-flag leftovers
#   * always-NORMAL ChainstateRole / GetRole and the dual-chainstate blockfile type
#   * caller-less vault, node, script, agent and GUI queries
#   * watch-only transaction-list eye icons
#   * the unused CONNECTIONS_NONE peer-count flag
#   * the empty, unloaded Qt application-catalog pipeline
#   * wrapped-witness descriptor, helper, and transaction-construction residue
#   * caller-less signing, vault-db, agent, RBF, Qt-migration and IPC leftovers
#   * provenance links to unpublished planning documents
#   * retired binary names, product identities, desktop IDs, and thread prefixes
#   * fee-contrast product copy, non-descriptor vault errors, camelCase listunspent
#     options, bitcoin-era networkhashps naming, fee-era package/priority test names
#     (including TestPackageSelection), leftover "descriptor vault" type contrast,
#     bitcoin-era JSON scriptPubKey/scriptSig/redeemScript/witnessScript keys,
#     bitcoin-era getblockstats sw* keys and decodescript "segwit" object,
#     caller-less blocking vault/agent wrappers, native-segwit product copy,
#     global-xpub parse errors, BIP-125 replacement copy, GetDestinationForKey
#     helpers, remaining AgentClient test-only wrappers,
#     pre-segwit GBT/upgrade-test residue, CAddress V1 disk, coin-control and
#     BIP70 merchant copy, bitcoin-era linearize height / bootstrap.dat, and
#     leftover "agent wallet" product copy
#
# Negative feeless assertions stay valid. Internal BIP32/PSQT field names are
# classified separately.
#
# Every rule is matched per line and again over the file joined into one
# whitespace-normalised string, so a banned phrase that wraps across a line
# break cannot hide from it. See test/lint/lint_wrapped_prose.py.
#
# Run `--self-test` to exercise the matcher without touching the tree.

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.append(str(Path(__file__).parent))
from lint_wrapped_prose import WrappedText, report_self_test  # noqa: E402


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    path_scope: "re.Pattern[str] | None" = None


# There is deliberately no scope allowlist here.
#
# A list of directories to scan is half of this lint's assertion, and it is the
# half that fails silently: a green run over the wrong file set is
# indistinguishable from a green run over a clean tree. The list used to name
# eleven directories, which left ci/, depends/, .github/ and thirteen root
# files -- INSTALL.md, CONTRIBUTING.md, SECURITY.md, vcpkg.json among them --
# unread by all 150 rules below. The Qt catalogue purge duly left an unread
# linguist_tools assignment in depends/packages/qt.mk and every run stayed
# green. Scanning every tracked file instead costs nothing measurable and
# cannot go stale when a new top-level directory appears.
#
# Only genuinely vendored trees are skipped, below, where upstream's own names
# legitimately live.

SKIP_PREFIXES = [
    Path("src/crc32c"),
    Path("src/crypto/ctaes"),
    Path("src/leveldb"),
    Path("src/secp256k1"),
    Path("src/univalue"),
    Path("docs/superpowers"),
]

SKIP_FILES = {
    Path("test/lint/lint-quicksilver-dead-surface-residue.py"),
    Path("test/lint/lint-quicksilver-fee-residue.py"),
    Path("test/lint/lint-stale-chain-fee-residue.py"),
    # Its fixtures reproduce the wrapped "agent wallets" this lint could not
    # see, verbatim, because a matcher tested against a paraphrase proves
    # nothing about the phrase it was written to catch.
    Path("test/lint/lint_wrapped_prose.py"),
    Path("test/lint/README.md"),
}

SRC = re.compile(r"^src/")
QT = re.compile(r"^src/qt/")
DOC = re.compile(r"^doc/")
FUNCTIONAL = re.compile(r"^test/functional/")
NETBASE_OR_NET_TESTS = re.compile(r"^(?:src/netbase\.cpp|src/test/netbase_tests\.cpp)$")
PUBLIC_QPUB_SCOPE = re.compile(r"^(?:doc/multisig-tutorial\.md|test/functional/vault_gethdkeys\.py)$")
RPC_QPUB_SCOPE = re.compile(r"^(?:src/rpc/rawtransaction\.cpp|test/functional/rpc_psqt\.py)$")
SLIP44_EXAMPLE_SCOPE = re.compile(r"^(?:doc/(?:descriptors|external-signer)\.md|src/rpc/output_script\.cpp|src/vault/test/vaultload_tests\.cpp|test/functional/rpc_getdescriptorinfo\.py)$")
DEAD_INTERFACE_SCOPE = re.compile(r"^src/(?:interfaces/(?:chain|node|mining|vault)\.h|(?:node|vault)/interfaces\.cpp)$")
VAULT_DB_SCOPE = re.compile(r"^src/vault/(?:interfaces\.cpp|vaultdb\.(?:h|cpp))$")
RELAYPOOL_ENTRY_SCOPE = re.compile(r"^src/kernel/relaypool_entry\.h$")
TEST_UTIL_SCOPE = re.compile(r"^src/test/util/(?:mining|net|setup_common)\.(?:h|cpp)$")
VAULT_SPEND_SCOPE = re.compile(r"^src/vault/spend\.(?:h|cpp)$")
VAULT_SPEND_RPC_SCOPE = re.compile(r"^src/vault/rpc/spend\.cpp$")
VAULT_DB_REF_SCOPE = re.compile(r"^src/vault/(?:db\.h|sqlite\.(?:h|cpp)|test/util\.h)$")
AGENT_CLIENT_SCOPE = re.compile(r"^src/agent/agentclient\.h$")
QT_OPTIONS_SCOPE = re.compile(r"^src/qt/optionsmodel\.(?:h|cpp)$")
BASH_COMPLETION_SCOPE = re.compile(r"^contrib/completions/bash/quicksilver-cli\.bash$")
JSON_SCRIPT_KEY_SCOPE = re.compile(r"^src/(?!test/data/)")
PRELAUNCH_FORMAT_SCOPE = re.compile(r"^src/(?:chain\.h|primitives/block\.h|undo\.h|outputtype\.h|qt/(?:optionsmodel|quicksilverunits)\.(?:h|cpp))$")
P2P_VERSION_SCOPE = re.compile(r"^src/(?:node/protocol_version\.h|net_processing\.cpp)$")
RPC_PROTOCOL_SCOPE = re.compile(r"^src/(?:rpc/request\.(?:h|cpp)|httprpc\.cpp)$")
FUNCTIONAL_FRAMEWORK_SCOPE = re.compile(r"^test/functional/test_framework/")
COINSTATS_SCOPE = re.compile(r"^src/(?:kernel/coinstats\.(?:h|cpp)|rpc/blockchain\.cpp)$")

RULES = [
    Rule("unpublished planning-document provenance", re.compile(r"docs/superpowers/(?:plans|specs)/")),
    Rule("dead Qt application-catalog pipeline", re.compile(r"quicksilver_(?:en|locale)|quicksilverstrings|extract_strings_qt|Qt5::(?:lupdate|lconvert)|qt5_add_translation|\bLinguistTools\b")),
    Rule("dead BIP70 payment-request field", re.compile(r"\b(?:sPaymentRequest|authenticatedMerchant|GetPaymentRequestMerchant)\b"), QT),
    Rule("dead BIP70 payment-request object", re.compile(r'\b(?:x509\+sha256|x509\+sha1)\b|r\.first == "PaymentRequest"|first == "PaymentRequest"')),
    Rule("dead IP-payment / OP_EVAL residue", re.compile(r"Sent to IP|Received by IP connection|\bOP_EVAL\b"), QT),
    Rule("BIP21 payment server mislabeled deprecated", re.compile(r"\(Deprecated\).{0,120}payment URI|paymentserver.{0,120}\(Deprecated\)", re.IGNORECASE), re.compile(r"^src/qt/README\.md$")),
    Rule("dead isLegacy abstraction", re.compile(r"\bisLegacy\s*\("), SRC),
    Rule("dead legacy_vault abstraction", re.compile(r"\blegacy_vault\b"), SRC),
    Rule("dead RollingFeeUpdate hook", re.compile(r"\bRollingFeeUpdate\s*\(")),
    Rule("dead amount_fee residue", re.compile(r"\bamount_fee\b")),
    Rule(
        "fee-era prose",
        re.compile(
            r"estimate size for fee|very high fees|transaction \+ fees|"
            r"for fee calculation|Feerate of first|low feerate and found|"
            r"dummy signatures for fee",
            re.IGNORECASE,
        ),
    ),
    Rule("tor-as-onion compatibility alias", re.compile(r'net == "tor"|ParseNetwork\("tor"\),\s*NET_ONION|ParseNetwork\("TOR"\),\s*NET_ONION'), NETBASE_OR_NET_TESTS),
    Rule("allowignoredconf compatibility flag", re.compile(r"\ballowignoredconf\b")),
    Rule("dictionary-form RPC compatibility", re.compile(r"For compatibility reasons, a dictionary")),
    Rule("dictionary-form outputs implementation", re.compile(r"\boutputs_is_obj\b")),
    Rule("camelCase fund option", re.compile(r'(?:\{\s*"(?:changeAddress|changePosition|lockUnspents|minimumAmount|maximumAmount|maximumCount|minimumSumAmount)"|, "(?:changeAddress|changePosition|lockUnspents|minimumAmount|maximumAmount|maximumCount|minimumSumAmount)"\})'), SRC),
    Rule("fee-era effective value", re.compile(r"\b(?:GetEffectiveValue|HasEffectiveValue|GetSelectedEffectiveValue)\b")),
    Rule("dead confirm-target", re.compile(r"\bm_confirm_target\b")),
    Rule("historical duplicate-tx exception", re.compile(r"\b(?:BIP30|IsBIP30Repeat|IsBIP30Unspendable|BIP34_IMPLIES_BIP30_LIMIT)\b|bad-txns-BIP30")),
    Rule("pre-Quicksilver addrman format", re.compile(r"\b(?:V0_HISTORICAL|V1_DETERMINISTIC|V2_ASMAP|V3_BIP155)\b")),
    Rule("pre-Quicksilver key metadata version", re.compile(r"\b(?:VERSION_BASIC|VERSION_WITH_HDDATA|VERSION_WITH_KEY_ORIGIN)\b")),
    Rule("pre-Quicksilver agent vault record", re.compile(r"\bLegacyAgentWalletRecordV[12]\b")),
    Rule("inherited vault feature stamp", re.compile(r"\b(?:FEATURE_LATEST|169900)\b")),
    Rule("old coins database probe", re.compile(r"\bDB_COINS\b|\.NeedsUpgrade\s*\("), SRC),
    Rule("boolean verbosity alias", re.compile(r"verbosity\|verbose|\ballow_bool\b"), SRC),
    Rule("legacy decodepsqt result name", re.compile(r"\bglobal_xpubs\b"), RPC_QPUB_SCOPE),
    Rule("JSON-RPC 1.0 alias", re.compile(r'jsonrpc_version\.get_str\(\)\s*==\s*"1\.0"'), re.compile(r"^src/rpc/request\.cpp$")),
    Rule("legacy JSON-RPC server mode", re.compile(r"\b(?:V1_LEGACY|m_json_version|legacy_version)\b"), RPC_PROTOCOL_SCOPE),
    Rule("prelaunch serialized-format tombstone", re.compile(r"\b(?:DUMMY_VERSION|nVersionDummy|RETIRED_WRAPPED_WITNESS)\b|burnt milli|retired and must not be reused"), PRELAUNCH_FORMAT_SCOPE),
    Rule("historical P2P feature gate", re.compile(r"\b(?:BIP0031_VERSION|SENDHEADERS_VERSION|SHORT_IDS_BLOCKS_VERSION|INVALID_CB_NO_BAN_VERSION|WTXID_RELAY_VERSION)\b"), P2P_VERSION_SCOPE),
    Rule("previous-release functional harness", re.compile(r"\bversion_is_at_least\b|\b(?:190000|219900|239000|260000|299900)\b|compatibility with older clients"), FUNCTIONAL_FRAMEWORK_SCOPE),
    Rule("legacy UTXO hash implementation", re.compile(r"\b(?:HASH_SERIALIZED|hashSerialized)\b|hash_serialized_3"), COINSTATS_SCOPE),
    Rule("inherited release migration", re.compile(r"prior to 25\.0|requires quicksilver-daemon server to be running v22\.0|pre-v28 versions|CSIDL_APPDATA")),
    Rule("upstream release RPC wording", re.compile(r"v27\.0 and prior releases"), DOC),
    Rule("SLIP-44 coin type 0 in derivation example", re.compile(r"/(?:44|49|84)(?:'|h)/0(?:'|h)/0(?:'|h)"), SLIP44_EXAMPLE_SCOPE),
    Rule("createmultisig base58 default", re.compile(r'RPCArg::Default\{"base58"\}|output_type = OutputType::BASE58'), re.compile(r"^src/rpc/output_script\.cpp$")),
    Rule("lab host name", re.compile(r"\bbox[0-9]+\b|\bworker-[0-9]+\b|\[\[rig-gpu-access\]\]")),
    Rule("undocumented rpcuser deprecation", re.compile(r"rpcuser and rpcpassword will soon be deprecated")),
    Rule("settings compatibility quirk", re.compile(r"Weird behavior preserved for backwards compatibility")),
    Rule("legacy Qt settings migration", re.compile(r"Migrate and delete legacy GUI settings")),
    Rule("Regtest capitalization", re.compile(r"\bRegtest\b")),
    Rule("public xpub/xprv wording", re.compile(r"\bxpub|\bxprv"), PUBLIC_QPUB_SCOPE),
    Rule("legacy address/spend wording", re.compile(r"\blegacy (?:addresses|address|spends)\b", re.IGNORECASE), re.compile(r"^(?:test/functional|doc)/")),
    Rule("dead from-map value", re.compile(r'value_map\["from"\]|mapValue\["from"\]'), QT),
    Rule("legacy vault comment", re.compile(r"unlike legacy vaults|Compatibility with old vaults", re.IGNORECASE)),
    Rule("removed Erlay/txreconciliation surface", re.compile(r"txreconciliation|TxReconciliation|TXRECONCILIATION|sendtxrcncl|SENDTXRCNCL|\bErlay\b|BIP[ -]?330", re.IGNORECASE)),
    Rule("removed minisketch dependency", re.compile(r"minisketch", re.IGNORECASE)),
    Rule("hidden generate RPC tombstone", re.compile(r'static RPCHelpMan generate\(\)|RPCHelpMan\{"generate"|\{"hidden", &generate\}')),
    Rule("watchonly listtransactions completion", re.compile(r"listtransactions\|setban")),
    Rule("unused key-origin vault flag", re.compile(r"\bVAULT_FLAG_KEY_ORIGIN_METADATA\b|\bkey_origin_metadata\b")),
    Rule("hdseedid RPC field", re.compile(r'"hdseedid"'), re.compile(r"^src/vault/rpc/")),
    Rule("empty-table -checkpoints flag", re.compile(r"-checkpoints\b")),
    Rule("checkpoint walker and empty maps", re.compile(r"\bDEFAULT_CHECKPOINTS_ENABLED\b|\bcheckpoints_enabled\b|\bGetLastCheckpoint\b|\bBLOCK_CHECKPOINT\b|\bCCheckpointData\b|\bMapCheckpoints\b|\bcheckpointData\b|\bmapCheckpoints\b|\.Checkpoints\(\)|bad-fork-prior-to-checkpoint")),
    Rule("empty-table -forcednsseed flag", re.compile(r"-forcednsseed\b")),
    Rule("unsupported-types reject set", re.compile(r"\bUNSUPPORTED_TYPES\b|\bLoadUnsupportedVaultRecords\b|\bUNSUPPORTED_RECORD\b")),
    Rule("CMasterKey dummy derivation slots", re.compile(r"\bnDerivationMethod\b|\bvchOtherDerivationParameters\b")),
    Rule("unused CKeyMetadata hd_seed_id", re.compile(r"\bhd_seed_id\b")),
    Rule("constant getvaultinfo format field", re.compile(r'pushKV\("format"|the database format \(sqlite\)'), re.compile(r"^src/vault/rpc/vault\.cpp$")),
    Rule("constant getvaultinfo descriptors field", re.compile(r'obj\.pushKV\("descriptors"|whether this vault uses descriptors'), re.compile(r"^src/vault/rpc/vault\.cpp$")),
    Rule("GBT request capabilities help", re.compile(r"client side supported feature"), re.compile(r"^src/rpc/mining\.cpp$")),
    Rule("GBT constant coinbaseaux/vbrequired", re.compile(r"\bcoinbaseaux\b|\bvbrequired\b"), SRC),
    Rule("GBT hashing-miner noncerange", re.compile(r"\bnoncerange\b"), SRC),
    Rule(
        "dropped send* verbose wrapper",
        re.compile(r'\{"verbose", RPCArg::Type::BOOL|if verbose is (?:not )?set'),
        VAULT_SPEND_RPC_SCOPE,
    ),
    Rule("never-run script_assets_test", re.compile(r"script_assets_test_minimizer|\bscript_assets_test\b|\bDIR_UNIT_TEST_DATA\b")),
    Rule(
        "dropped assumeUTXO identifier",
        re.compile(r"assumeutxo", re.IGNORECASE),
    ),
    Rule(
        "dropped UTXO-snapshot bootstrap surface",
        re.compile(
            r"\b(?:loadtxoutset|dumptxoutset|getchainstates|GetAvailableSnapshotHeights|"
            r"ActivateSnapshot|PopulateAndValidateSnapshot|MaybeCompleteSnapshotValidation|"
            r"m_snapshot_chainstate|DetectSnapshotChainstate|ActivateExistingSnapshot|"
            r"ValidatedSnapshotCleanup|DeleteSnapshotChainstate|SNAPSHOT_MAGIC_BYTES|"
            r"SnapshotMetadata|SNAPSHOT_CHAINSTATE_SUFFIX|CreateUTXOSnapshot|"
            r"TryDownloadingHistoricalBlocks|hasAssumedValidChain|IsSnapshotActive|"
            r"BackgroundSyncInProgress|EmplaceCoinInternalDANGER|chainstate_snapshot|"
            r"utxo_to_sqlite)\b|BlockfileType::ASSUMED"
        ),
    ),
    Rule(
        "dropped chainstate-role surface",
        re.compile(r"\bChainstateRole\b|\bGetRole\s*\("),
    ),
    Rule(
        "dropped dual-blockfile type",
        re.compile(r"\bBlockfileType(?:ForHeight)?\b"),
    ),
    Rule(
        "removed implicit-whitelist compatibility flags",
        re.compile(r"\b(?:whitelistrelay|whitelistforcerelay|DEFAULT_WHITELISTRELAY|DEFAULT_WHITELISTFORCERELAY|whitelist_(?:relay|forcerelay))\b"),
    ),
    Rule("ignored BIP22 submitblock argument", re.compile(r'"dummy"[^\n]*BIP22|allow 2 arguments for compliance with BIP22', re.IGNORECASE)),
    Rule("obsolete RPC transaction-error alias", re.compile(r"\bRPC_TRANSACTION_(?:ERROR|REJECTED)\b")),
    Rule("descriptor-cache upgrade path", re.compile(r"\b(?:VAULT_FLAG_LAST_HARDENED_XPUB_CACHED|UpgradeDescriptorCache)\b")),
    Rule("unordered vault-transaction upgrade", re.compile(r"\b(?:ReorderTransactions|any_unordered)\b")),
    Rule("vault-load auto-repair", re.compile(r"reason=retired-address-type|extraneous-encryption")),
    Rule("dead encrypted-key presence query", re.compile(r"\bHaveCryptedKeys\b")),
    Rule("unused BlockInfo undo payload", re.compile(r"\bundo_data\b"), re.compile(r"^src/interfaces/chain\.h$")),
    Rule("stale future-commit note", re.compile(r"will be removed in upcoming commit", re.IGNORECASE)),
    Rule("fee-era reconsiderable-parent wording", re.compile(r"low-feerate parent of orphan", re.IGNORECASE)),
    Rule(
        "dead vault key, record and load surfaces",
        re.compile(
            r"\b(?:GetAffectedKeys|AllInputsMine|CopyFrom|EraseMasterKey|LOAD_FAIL|"
            r"BIP32_HARDENED_KEY_LIMIT)\b"
        ),
    ),
    Rule("fee-era currency atom", re.compile(r"\bCURRENCY_ATOM\b")),
    Rule(
        "dead node, script and agent surfaces",
        re.compile(
            r"\b(?:ResetChainstates|SetConfigFilePath|GetCScripts|AcceptHeaders|"
            r"ExplicitCopyTag|ExplicitCopy|RPC_VAULT_ALREADY_UNLOCKED)\b"
        ),
    ),
    Rule(
        "dead GUI query surfaces",
        re.compile(r"\b(?:getTransactionSize|isReleaseVersion|getGraphRange|MC_ERROR|MC_DEBUG)\b"),
    ),
    Rule("dead combobox role setter", re.compile(r"\bsetRole\s*\("), QT),
    Rule("watch-only eye icons", re.compile(r"\beye_(?:plus|minus)\b")),
    Rule("dead GPU CSV verdict helper", re.compile(r"\bGpuCsvOk\b")),
    Rule("unused peer-count flag", re.compile(r"\bCONNECTIONS_NONE\b"), QT),
    Rule(
        "wrapped-witness construction residue",
        re.compile(
            r"\bsh\((?:wpkh|wsh)\(|\bp2sh[_-]?p2w(?:pkh|sh)\b|"
            r"\bp2sh-segwit\b|\bNESTED_P2WPKH\b|:WS(?:\"|')",
            re.IGNORECASE,
        ),
    ),
    Rule(
        "dead internal interface methods",
        re.compile(
            r"\b(?:initLogging|getLastBlockTime|hasDescendantsInRelayPool|getSetting|"
            r"deleteRwSettings|overwriteRwSetting|getVaultDir|isEncrypted|getBlockHeader|getCoinbaseTx|"
            r"getWitnessCommitmentIndex|getCoinbaseMerklePath)\b"
        ),
        DEAD_INTERFACE_SCOPE,
    ),
    Rule("dead vault encryption database probe", re.compile(r"\bIsEncrypted\b"), VAULT_DB_SCOPE),
    Rule(
        "dead relaypool notification payload",
        re.compile(
            r"\b(?:NewRelayPoolTransactionInfo|RemovedRelayPoolTransactionInfo|"
            r"RelayPoolTransactionsRemovedForBlock|m_virtual_transaction_size|txHeight|"
            r"m_relaypool_limit_bypassed|m_submitted_in_package|m_has_no_relaypool_parents|"
            r"HasNoInputsOf)\b"
        ),
    ),
    Rule("dead relaypool transaction-info wrapper", re.compile(r"\bTransactionInfo\b"), RELAYPOOL_ENTRY_SCOPE),
    Rule("dead Qt state and signal leftovers", re.compile(r"\b(?:cachedNodeids|proxyIpChecks)\b"), QT),
    Rule("dead test utility surfaces", re.compile(r"\b(?:CreateBlockChain|DynSock|m_block_tree_db_in_memory)\b"), TEST_UTIL_SCOPE),
    Rule("dead block-status reservations", re.compile(r"\b(?:BLOCK_VALID_RESERVED|BLOCK_STATUS_RESERVED)\b")),
    Rule(
        "dead internal sweep surfaces",
        re.compile(
            r"\b(?:DrainAllOutboundMessages|AddLocalServices|RemoveLocalServices|"
            r"HasCoinsViews|RewriteDB|IdIsXpub|m_vault_filenames|RPC_OUT_OF_MEMORY|"
            r"SCRIPT_ERR_LAST)\b"
        ),
    ),
    Rule(
        "prelaunch source tombstones",
        re.compile(
            r"\b(?:ALLOW_BOOL|ALLOW_INT|ALLOW_STRING|ALLOW_LIST|DB_TXINDEX_BLOCK)\b"
        ),
        SRC,
    ),
    Rule(
        "misnamed UTXO-set RPC exception",
        re.compile(r"\bRPC_TRANSACTION_ALREADY_IN_UTXO_SET\b"),
    ),
    Rule(
        "dead vault coin-selection parameters",
        re.compile(
            r"CalculateMaximumSignedInputSize\([^)]*\bCOutPoint\b|"
            r"\bFetchSelectedInputs\([^)]*\bCoinSelectionParams\b|"
            r"\b(?:AttemptSelection|ChooseSelectionResult)\(interfaces::Chain&"
        ),
        VAULT_SPEND_SCOPE,
    ),
    Rule("dead HaveCScript query", re.compile(r"\bHaveCScript\b")),
    Rule("dead SetupGeneration", re.compile(r"\bSetupGeneration\b")),
    Rule("dead SendTransactionToAll", re.compile(r"\bSendTransactionToAll\b")),
    Rule(
        "dead AgentClient wrappers",
        re.compile(
            r"const AgentClientOptions& Options\(\)|AgentPeerSet& Peers\(\)|"
            r"RemovePeer\(AgentPeerId peer_id\) \{ return m_peers|"
            r"std::vector<AgentPeerSetAction> AnnounceTransactionToAll|"
            r"bool HasHeaderStorePath\(\)|"
            r"const std::optional<fs::path>& HeaderStorePath\(\)|"
            r"bool loaded_from_store\(\)|"
            r"bool started_fresh\(\)|"
            r"bool HasAnyOutboundMessages\(\)|"
            r"size_t TotalOutboundMessageCount\(\)|"
            r"std::optional<AgentPeerOutboundMessage> PopNextOutboundMessage\(\)|"
            r"size_t MaxTxInventory\(\) const \{ return m_peers"
        ),
        AGENT_CLIENT_SCOPE,
    ),
    Rule("dead vault database refcount", re.compile(r"\b(?:m_refcount|RemoveRef)\b|virtual void AddRef\(\)|void AddRef\(\) override"), VAULT_DB_REF_SCOPE),
    Rule("BDB rewrite skip-key", re.compile(r"\bpszSkip\b|Rewrite\(const char\*"), re.compile(r"^src/vault/")),
    Rule("dead vault RBF replacement surface", re.compile(r"\b(?:MarkReplaced|replaced_by_txid|replaces_txid)\b")),
    Rule("boolean verbosity completion", re.compile(r"getblock\|getblockheader\|.{0,200}getrawtransaction|getblock\|getrawtransaction"), BASH_COMPLETION_SCOPE),
    Rule("legacy Qt font-migration key", re.compile(r"\bUseEmbeddedMonospacedFont\b")),
    Rule("empty Qt settings-version stamp", re.compile(r"\b(?:nSettingsVersion|checkAndMigrate)\b"), QT_OPTIONS_SCOPE),
    Rule("removed multiprocess IPC test harness", re.compile(r"\b(?:IpcPipeTest|IpcSocketPairTest|IpcSocketTest|ipc_test\.h)\b")),
    Rule("removed macdeploy helper", re.compile(r"\bmacdeployqtplus\b")),
    Rule("removed deterministic-coverage helper", re.compile(r"\btest_deterministic_coverage\b")),
    Rule("removed multiprocess node binary name", re.compile(r"\bquicksilver-node\b")),
    Rule("fee-contrast product copy", re.compile(r"instead of a fee|without a fee", re.IGNORECASE)),
    Rule("non-descriptor vault product error", re.compile(r"non-descriptor vault", re.IGNORECASE)),
    Rule("only-descriptor-vaults product error", re.compile(r"Only descriptor vaults can")),
    Rule("sqlite required for descriptor vault copy", re.compile(r"required for descriptor vault")),
    Rule("descriptor_vault public flag name", re.compile(r'\{\s*"descriptor_vault"'), re.compile(r"^src/vault/vault\.h$")),
    Rule("bitcoin-era networkhashps RPC", re.compile(r"\b(?:getnetworkhashps|networkhashps|GetNetworkHashPS)\b")),
    Rule("fee-era CPFP package test name", re.compile(r"\b(?:package_no_cpfp_tests|package_cpfp_tests)\b")),
    Rule("fee-era prioritised mining test name", re.compile(r"\bTestPrioritisedMining\b")),
    Rule("fee-era package-selection test name", re.compile(r"\bTestPackageSelection\b")),
    Rule("descriptor vault product copy", re.compile(r"\bdescriptor vaults?\b", re.IGNORECASE)),
    Rule("bitcoin-era JSON scriptPubKey key", re.compile(r'(?<!exists\()(?:"|\\")scriptPubKey(?:"|\\")'), JSON_SCRIPT_KEY_SCOPE),
    Rule("bitcoin-era JSON scriptSig key", re.compile(r'(?<!exists\()(?:"|\\")scriptSig(?:"|\\")'), JSON_SCRIPT_KEY_SCOPE),
    Rule("bitcoin-era JSON redeemScript key", re.compile(r'(?<!exists\()(?:"|\\")redeemScript(?:"|\\")'), JSON_SCRIPT_KEY_SCOPE),
    Rule("bitcoin-era JSON witnessScript key", re.compile(r'(?<!exists\()(?:"|\\")witnessScript(?:"|\\")'), JSON_SCRIPT_KEY_SCOPE),
    Rule(
        "dead blocking vault interface methods",
        re.compile(r"\b(?:getCoins|getVaultTxDetails|listCoins|getBalance)\s*\("),
        DEAD_INTERFACE_SCOPE,
    ),
    Rule(
        "dead CoinsResult TypesCount",
        re.compile(r"size_t TypesCount\(\) const \{ return coins\.size\(\); \}"),
        re.compile(r"^src/vault/spend\.h$"),
    ),
    Rule("native-segwit product copy", re.compile(r"native segwit", re.IGNORECASE)),
    Rule("segwit-output product error", re.compile(r"not useable for SegWit outputs")),
    Rule("global xpub parse error", re.compile(r"global xpub", re.IGNORECASE)),
    Rule("BIP-125 replacement product copy", re.compile(r"BIP-125-replaced")),
    Rule("datacarrier scriptPubKey help", re.compile(r"data-carrying raw scriptPubKey")),
    Rule("Send-tab product copy", re.compile(r"`Send` tab")),
    Rule("dead GetDestinationForKey helpers", re.compile(r"\b(?:GetDestinationForKey|GetAllDestinationsForKey)\b"), SRC),
    Rule("pre-segwit GBT branch", re.compile(r"\bfPreSegWit\b")),
    Rule("pre-segwit node-upgrade test", re.compile(r"feature_presegwit_node_upgrade")),
    Rule("dead thin-space UTF-8 macros", re.compile(r"\b(?:REAL_)?THIN_SP_UTF8\b"), SRC),
    Rule("dead message-box icon mask", re.compile(r"\bICON_MASK\b"), SRC),
    Rule("CAddress V1 disk format", re.compile(r"\bV1_DISK\b")),
    Rule("CAddress V1 disk version-0 path", re.compile(r"stored_format_version == 0")),
    Rule(
        "coin-control product copy",
        re.compile(r"coin control features|Enable coin &(?:amp;)?control"),
        re.compile(r"^src/qt/forms/optionsdialog\.ui$"),
    ),
    Rule(
        "malware steal coins copy",
        re.compile(r"malware can steal your coins", re.IGNORECASE),
        re.compile(r"^src/qt/forms/optionsdialog\.ui$"),
    ),
    Rule(
        "delegated coins-are-yours copy",
        re.compile(r"These coins are still yours"),
        re.compile(r"^src/qt/forms/overviewpage\.ui$"),
    ),
    Rule(
        "BIP70 merchant-instruction copy",
        re.compile(r"merchant instructions to switch vaults"),
        re.compile(r"^src/qt/paymentserver\.cpp$"),
    ),
    Rule("bitcoin-era linearize height", re.compile(r"max_height(?:=|'\] = )313000")),
    Rule(
        "bitcoin-era JSON getblockstats sw* keys",
        re.compile(r'(?<!stat == )["\'](?:swtotal_size|swtotal_weight|swtxs)["\']'),
        re.compile(r"^(src/rpc/blockchain\.cpp|test/functional/data/rpc_getblockstats\.json)$"),
    ),
    Rule(
        "bitcoin-era JSON decodescript segwit key",
        re.compile(r'(?:pushKV\(|Type::OBJ, )\s*"segwit"|[\[\'"]segwit[\'"](?:\s*\]|\s*:)|assert [\'"]segwit[\'"] not in|"segwit":'),
        re.compile(r"^(src/rpc/rawtransaction\.cpp|test/functional/rpc_decodescript\.py|test/functional/data/rpc_decodescript\.json)$"),
    ),
    Rule("agent-wallet product copy", re.compile(r"agent wallets?", re.IGNORECASE)),
    # The agent's funded container is an allotment. It was called a wallet, then
    # briefly a vault -- which collided with the steward's own vault, the exact
    # confusion the rename fixed. Both spellings stay banned in product copy.
    Rule("agent-vault product copy", re.compile(r"agent vaults?", re.IGNORECASE)),
    Rule("bootstrap.dat linearize example", re.compile(r"bootstrap\.dat"), re.compile(r"^contrib/linearize/")),
    Rule("retired quicksilver-" "util binary", re.compile(r"quicksilver-" r"util|quicksilver" r"util|QUICKSILVER" r"UTIL")),
    # Case-sensitive per spelling, so the retired name is caught as a prefix of
    # an identifier (a QUICKSILVER-D-underscore variable, an Init class) while
    # quicksilver-daemon's own spellings, where "d" is followed by "aemon", are not.
    Rule("retired quicksilver" "d binary", re.compile(r"\bquicksilver" r"d(?![a-z])|\bQuicksilver" r"d(?![a-z])|\bQUICKSILVER" r"D(?![A-Z])")),
    Rule("retired quicksilver-" "qt product name", re.compile(r"(?<!test_)quicksilver-" r"qt|Quicksilver-" r"Qt|Quicksilver " r"Qt|Quicksilver" r"Qt|quicksilver" r"qt")),
    Rule("retired quicksilver_" "qt desktop identity", re.compile(r"quicksilver_" r"qt\.(?:desktop|metainfo)|hg\.quicksilver_" r"qt")),
    Rule("retired b- thread prefix", re.compile(r'["\'`]b' r'-')),
]

ALLOWED = {
    Path("src/test/descriptor_tests.cpp"): [
        re.compile(r"CheckUnparsable\(\"sh\((?:wpkh|wsh)\("),
    ],
    Path("test/functional/rpc_getdescriptorinfo.py"): [
        re.compile(r"assert_raises_rpc_error.*sh\((?:wpkh|wsh)\("),
    ],
    Path("test/functional/rpc_scantxoutset.py"): [
        re.compile(r"assert_raises_rpc_error.*sh\(wpkh\("),
    ],
    Path("test/util/data/quicksilver-tx-test.json"): [
        re.compile(r":WS\""),
    ],
    # Dated evidence of the 2026-09-23 Jammy build; these names describe the
    # artifacts that were actually produced and are not current procedure.
    Path("contrib/release/README.md"): [
        re.compile(r"quicksilver-" r"qt-dbgsym_0\.1\.1-1_amd64\.ddeb"),
        re.compile(r"quicksilver-" r"qt_0\.1\.1-1_amd64\.deb"),
        re.compile(r"made `quicksilver-" r"qt` define"),
        re.compile(r"installed `quicksilver-" r"qt` and"),
    ],
    Path("doc/design/network-naming.md"): [
        re.compile(r"`regtest`"),
        re.compile(r"\bregtest\b"),
    ],
    Path("src/qt/test/optiontests.cpp"): [
        re.compile(r"legacyDisplayUnitSettingIsIgnored"),
        re.compile(r"legacy migration path"),
    ],
    Path("test/functional/rpc_psqt.py"): [
        re.compile(r'assert "global_xpubs" not in'),
    ],
    # Negative assertions and the comment explaining the removal, not the surface itself.
    Path("src/test/net_tests.cpp"): [
        re.compile(r'ALL_NET_MESSAGE_TYPES, "sendtxrcncl"'),
        re.compile(r"all_net_message_types_exclude_sendtxrcncl"),
        re.compile(r"BIP-330 was only ever a handshake upstream"),
    ],
    Path("test/functional/feature_config_args.py"): [
        re.compile(r"Invalid parameter -checkpoints=0"),
        re.compile(r"extra_args=\['-checkpoints=0'\]"),
        re.compile(r"Invalid parameter -forcednsseed=1"),
        re.compile(r"extra_args=\['-forcednsseed=1'\]"),
        re.compile(r"Invalid parameter -whitelistrelay"),
        re.compile(r"\['-whitelistrelay'\]"),
        re.compile(r"Invalid parameter -whitelistforcerelay"),
        re.compile(r"\['-whitelistforcerelay'\]"),
    ],
    Path("test/functional/vault_avoidreuse.py"): [
        re.compile(r"Unknown vault flag: key_origin_metadata"),
        re.compile(r"setvaultflag, 'key_origin_metadata'"),
    ],
    # The dangling-script lint has to name the deleted helpers in its allowlist.
    Path("test/lint/lint-dangling-script-references.py"): [
        re.compile(r"contrib/devtools/test_deterministic_coverage\.sh"),
        re.compile(r"test/functional/feature_presegwit_node_upgrade\.py"),
    ],
    # Imported script-vector JSON keys, not the RPC/REST product names.
    Path("src/test/script_tests.cpp"): [
        re.compile(r'"scriptPubKey"|"scriptSig"'),
    ],
    Path("src/test/script_standard_tests.cpp"): [
        re.compile(r'"scriptPubKey"'),
    ],
}


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, encoding="utf8").strip())


def tracked_files(root: Path) -> list[Path]:
    raw = subprocess.check_output(["git", "ls-files", "-z"], text=True, encoding="utf8").split("\0")
    paths = []
    for name in raw:
        if not name:
            continue
        relpath = Path(name)
        if relpath in SKIP_FILES:
            continue
        if any(relpath == prefix or relpath.is_relative_to(prefix) for prefix in SKIP_PREFIXES):
            continue
        paths.append(root / relpath)
    return paths


def is_binary(path: Path) -> bool:
    try:
        return b"\0" in path.read_bytes()[:8192]
    except OSError:
        return False


def line_allowed(relpath: Path, line: str) -> bool:
    return any(pattern.search(line) for pattern in ALLOWED.get(relpath, []))


def match_rule(relpath: str, line: str) -> "Rule | None":
    for rule in RULES:
        if rule.path_scope is not None and not rule.path_scope.search(relpath):
            continue
        if rule.pattern.search(line):
            return rule
    return None


def wrapped_failures(relpath: Path, rel: str, lines: list[str]) -> list[str]:
    """Rule matches that straddle a line break, which match_rule cannot see.

    Every rule is offered to the matcher, not just the multi-word ones: a rule
    with no whitespace or wildcard in it cannot produce a match spanning a
    boundary, because the join puts a separator at every boundary and a
    spanning match has to consume it. Filtering by hand would only be a second
    place to get the classification wrong.

    First rule wins per starting line, as in the per-line pass, and a finding
    is dropped when any line it spans carries an exemption.
    """
    failures = []
    reported: set[int] = set()
    joined = WrappedText(lines)
    for rule in RULES:
        if rule.path_scope is not None and not rule.path_scope.search(rel):
            continue
        for match in joined.matches(rule.pattern):
            if match.first_line in reported:
                continue
            span = lines[match.first_line - 1:match.last_line]
            if any(line_allowed(relpath, line) for line in span):
                continue
            reported.add(match.first_line)
            failures.append(
                f"{relpath}:{match.first_line}: {rule.name}: "
                f"{match.text.strip()} ({match.where()})"
            )
    return failures


def self_test() -> int:
    matcher_failures = report_self_test()
    cases = [
        ("src/qt/CMakeLists.txt", "qt5_add_translation(qm_files ${ts_files})", "dead Qt application-catalog pipeline"),
        ("src/vault/scriptpubkeyman.h", "std::vector<CKeyID> GetAffectedKeys(const CScript& spk, const SigningProvider& provider);", "dead vault key, record and load surfaces"),
        ("src/vault/receive.h", "bool AllInputsMine(const CVault& vault, const CTransaction& tx, const isminefilter& filter);", "dead vault key, record and load surfaces"),
        ("src/vault/transaction.h", "    void CopyFrom(const CVaultTx&);", "dead vault key, record and load surfaces"),
        ("src/vault/vaultdb.h", "    bool EraseMasterKey(unsigned int id);", "dead vault key, record and load surfaces"),
        ("src/vault/vaultdb.h", "    LOAD_FAIL = 7,", "dead vault key, record and load surfaces"),
        ("src/vault/scriptpubkeyman.cpp", "const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000;", "dead vault key, record and load surfaces"),
        ("src/consensus/amount.h", 'inline const std::string CURRENCY_ATOM{"cinnabar"};', "fee-era currency atom"),
        ("src/validation.h", "    void ResetChainstates() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", "dead node, script and agent surfaces"),
        ("src/common/args.h", "    void SetConfigFilePath(fs::path);", "dead node, script and agent surfaces"),
        ("src/script/signingprovider.h", "    virtual std::set<CScriptID> GetCScripts() const;", "dead node, script and agent surfaces"),
        ("src/agent/headerchain.h", "    HeaderAcceptResult AcceptHeaders(std::span<const CBlockHeader> headers);", "dead node, script and agent surfaces"),
        ("src/kernel/relaypool_entry.h", "    static constexpr ExplicitCopyTag ExplicitCopy{};", "dead node, script and agent surfaces"),
        ("src/rpc/protocol.h", "    RPC_VAULT_ALREADY_UNLOCKED     = -17, //!< Vault is already unlocked", "dead node, script and agent surfaces"),
        ("src/qt/vaultmodeltransaction.h", "    unsigned int getTransactionSize();", "dead GUI query surfaces"),
        ("src/qt/clientmodel.h", "    bool isReleaseVersion() const;", "dead GUI query surfaces"),
        ("src/qt/trafficgraphwidget.h", "    std::chrono::minutes getGraphRange() const;", "dead GUI query surfaces"),
        ("src/qt/rpcconsole.h", "        MC_ERROR,", "dead GUI query surfaces"),
        ("src/qt/qvaluecombobox.cpp", "void QValueComboBox::setRole(int _role)", "dead combobox role setter"),
        ("src/qt/quicksilver.qrc", '        <file alias="eye_plus">res/icons/eye_plus.png</file>', "watch-only eye icons"),
        ("src/qt/clientmodel.h", "    CONNECTIONS_NONE = 0,", "unused peer-count flag"),
        ("src/interfaces/node.h", "    virtual void initLogging() = 0;", "dead internal interface methods"),
        ("src/vault/vaultdb.h", "    bool IsEncrypted();", "dead vault encryption database probe"),
        ("src/kernel/relaypool_entry.h", "struct NewRelayPoolTransactionInfo {", "dead relaypool notification payload"),
        ("src/kernel/relaypool_entry.h", "struct TransactionInfo {", "dead relaypool transaction-info wrapper"),
        ("src/qt/optionsdialog.h", "    void proxyIpChecks(QValidatedLineEdit*, uint16_t);", "dead Qt state and signal leftovers"),
        ("src/test/util/net.h", "class DynSock : public ZeroSock", "dead test utility surfaces"),
        ("src/chain.h", "    BLOCK_VALID_RESERVED = 1,", "dead block-status reservations"),
        ("src/chain.h", "    BLOCK_STATUS_RESERVED = 256,", "dead block-status reservations"),
        ("src/agent/agentclient.h", "    std::vector<AgentPeerOutboundMessage> DrainAllOutboundMessages();", "dead internal sweep surfaces"),
        ("src/net.h", "    void AddLocalServices(ServiceFlags services);", "dead internal sweep surfaces"),
        ("src/validation.h", "    bool HasCoinsViews() const;", "dead internal sweep surfaces"),
        ("src/vault/scriptpubkeyman.h", "    virtual void RewriteDB() {}", "dead internal sweep surfaces"),
        ("src/test/fuzz/util/descriptor.h", "    bool IdIsXpub(uint8_t idx) const;", "dead internal sweep surfaces"),
        ("src/vault/interfaces.cpp", "    const std::vector<std::string> m_vault_filenames;", "dead internal sweep surfaces"),
        ("src/rpc/protocol.h", "    RPC_OUT_OF_MEMORY = -7,", "dead internal sweep surfaces"),
        ("src/script/script_error.h", "#define SCRIPT_ERR_LAST SCRIPT_ERR_ERROR_COUNT", "dead internal sweep surfaces"),
        ("src/common/args.h", "    // ALLOW_BOOL = 0x02,", "prelaunch source tombstones"),
        ("src/node/blockstorage.cpp", "// BlockTreeDB::DB_TXINDEX_BLOCK{'T'};", "prelaunch source tombstones"),
        ("src/rpc/relaypool.cpp", "A specific exception, RPC_TRANSACTION_ALREADY_IN_UTXO_SET, may throw", "misnamed UTXO-set RPC exception"),
        ("src/vault/spend.h", "int CalculateMaximumSignedInputSize(const CTxOut&, const COutPoint, const SigningProvider*, bool, const CCoinControl*);", "dead vault coin-selection parameters"),
        ("src/vault/spend.h", "util::Result<PreSelectedInputs> FetchSelectedInputs(const CVault&, const CCoinControl&, const CoinSelectionParams&);", "dead vault coin-selection parameters"),
        ("src/vault/spend.cpp", "util::Result<SelectionResult> AttemptSelection(interfaces::Chain& chain, const CAmount& target, OutputGroupTypeMap& groups,", "dead vault coin-selection parameters"),
        ("src/vault/spend.cpp", "util::Result<SelectionResult> ChooseSelectionResult(interfaces::Chain& chain, const CAmount& target, Groups& groups,", "dead vault coin-selection parameters"),
        ("src/script/signingprovider.h", "    virtual bool HaveCScript(const CScriptID &scriptid) const { return false; }", "dead HaveCScript query"),
        ("src/vault/scriptpubkeyman.h", "    virtual bool SetupGeneration(bool force = false) { return false; }", "dead SetupGeneration"),
        ("src/agent/agentpeerset.h", "    std::vector<AgentPeerSetAction> SendTransactionToAll(const CTransaction& transaction);", "dead SendTransactionToAll"),
        ("src/agent/agentclient.h", "    const AgentClientOptions& Options() const { return m_options; }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    AgentPeerSet& Peers() { return m_peers; }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    AgentPeerSetAction RemovePeer(AgentPeerId peer_id) { return m_peers.RemovePeer(peer_id); }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    std::vector<AgentPeerSetAction> AnnounceTransactionToAll(const CTransaction& transaction,", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    bool HasHeaderStorePath() const { return m_options.header_store_path.has_value(); }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    const std::optional<fs::path>& HeaderStorePath() const { return m_options.header_store_path; }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    bool loaded_from_store() const { return status == HeaderStoreResult::OK; }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    bool started_fresh() const { return status == HeaderStoreResult::FILE_NOT_FOUND; }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    bool HasAnyOutboundMessages() const { return m_peers.HasOutboundMessages(); }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    size_t TotalOutboundMessageCount() const { return m_peers.OutboundMessageCount(); }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    std::optional<AgentPeerOutboundMessage> PopNextOutboundMessage() { return m_peers.PopNextOutboundMessage(); }", "dead AgentClient wrappers"),
        ("src/agent/agentclient.h", "    size_t MaxTxInventory() const { return m_peers.MaxTxInventory(); }", "dead AgentClient wrappers"),
        ("src/vault/db.h", "    std::atomic<int> m_refcount{0};", "dead vault database refcount"),
        ("src/vault/db.h", "    virtual void AddRef() = 0;", "dead vault database refcount"),
        ("src/vault/sqlite.h", "    void AddRef() override { assert(false); }", "dead vault database refcount"),
        ("src/vault/db.h", "    virtual bool Rewrite(const char* pszSkip=nullptr) = 0;", "BDB rewrite skip-key"),
        ("src/vault/vault.h", "    bool MarkReplaced(const uint256& originalHash, const uint256& newHash);", "dead vault RBF replacement surface"),
        ("src/vault/rpc/transactions.cpp", '           {RPCResult::Type::STR_HEX, "replaced_by_txid", /*optional=*/true, "Only if \'category\' is \'send\'. The txid if this tx was replaced."},', "dead vault RBF replacement surface"),
        ("contrib/completions/bash/quicksilver-cli.bash", "            getblock|getblockheader|getrelaypoolancestors|getrelaypooldescendants|getrawtransaction|gettransaction|listreceivedbyaddress)", "boolean verbosity completion"),
        ("src/qt/optionsmodel.cpp", '    } else if (settings.contains("UseEmbeddedMonospacedFont")) {', "legacy Qt font-migration key"),
        ("src/qt/optionsmodel.h", "    void checkAndMigrate();", "empty Qt settings-version stamp"),
        ("src/qt/optionsmodel.cpp", '    static const char strSettingsVersionKey[] = "nSettingsVersion";', "empty Qt settings-version stamp"),
        ("src/test/ipc_test.h", "void IpcPipeTest();", "removed multiprocess IPC test harness"),
        ("cmake/module/Maintenance.cmake", "        COMMAND ${PYTHON_COMMAND} ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} ${osx_volname} -translations-dir=${QT_TRANSLATIONS_DIR} -zip", "removed macdeploy helper"),
        ("contrib/devtools/test_deterministic_coverage.sh", "# test_deterministic_coverage.sh", "removed deterministic-coverage helper"),
        ("src/init/common.h", "//! @brief Common init functions shared by quicksilver-node, quicksilver-vault, etc.", "removed multiprocess node binary name"),
        ("src/qt/sendcoinsdialog.cpp", '    question_string.append(tr("Sending uses proof-of-work instead of a fee. Nothing is deducted for network fees; the full amount arrives after this desktop solves the required work."));', "fee-contrast product copy"),
        ("src/qt/sendcoinsdialog.cpp", '    disclosure << tr("Quicksilver sends the full amount without a fee. Before broadcast, this desktop prepares a transfer proof; that work can take time.");', "fee-contrast product copy"),
        ("src/vault/rpc/vault.cpp", '                throw JSONRPCError(RPC_VAULT_ERROR, "gethdkeys is not available for non-descriptor vault");', "non-descriptor vault product error"),
        ("src/vault/archive.cpp", '        return util::Error{_("Only descriptor vaults can be archived.")};', "only-descriptor-vaults product error"),
        ("src/vault/rpc/vault.cpp", '    throw JSONRPCError(RPC_VAULT_ERROR, "Compiled without sqlite support (required for descriptor vault)");', "sqlite required for descriptor vault copy"),
        ("src/vault/vault.h", '    {"descriptor_vault", VAULT_FLAG_DESCRIPTORS},', "descriptor_vault public flag name"),
        ("src/rpc/mining.cpp", '    return RPCHelpMan{"getnetworkhashps",', "bitcoin-era networkhashps RPC"),
        ("src/rpc/mining.cpp", '    obj.pushKV("networkhashps",    getnetworkhashps().HandleRequest(request));', "bitcoin-era networkhashps RPC"),
        ("src/test/txpackage_tests.cpp", "BOOST_AUTO_TEST_CASE(package_no_cpfp_tests)", "fee-era CPFP package test name"),
        ("src/test/miner_tests.cpp", "    void TestPrioritisedMining(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", "fee-era prioritised mining test name"),
        ("src/test/miner_tests.cpp", "    void TestPackageSelection(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", "fee-era package-selection test name"),
        ("src/interfaces/vault.h", "    virtual std::vector<VaultTxOut> getCoins(const std::vector<COutPoint>& outputs) = 0;", "dead blocking vault interface methods"),
        ("src/interfaces/vault.h", "    virtual VaultTx getVaultTxDetails(const uint256& txid,", "dead blocking vault interface methods"),
        ("src/interfaces/vault.h", "    virtual CoinsList listCoins() = 0;", "dead blocking vault interface methods"),
        ("src/interfaces/vault.h", "    virtual CAmount getBalance() = 0;", "dead blocking vault interface methods"),
        ("src/vault/spend.h", "    size_t TypesCount() const { return coins.size(); }", "dead CoinsResult TypesCount"),
        ("src/rpc/output_script.cpp", "    wpkh(<pubkey>)                                    Native segwit P2PKH outputs for the given pubkey", "native-segwit product copy"),
        ("src/quicksilver-tx.cpp", '            throw std::runtime_error("Uncompressed pubkeys are not useable for SegWit outputs");', "segwit-output product error"),
        ("src/psqt.h", '                        throw std::ios_base::failure("Size of key was not the expected size for the type global xpub");', "global xpub parse error"),
        ("doc/JSON-RPC-interface.md", "example, a vault transaction that was BIP-125-replaced in the relay pool prior to", "BIP-125 replacement product copy"),
        ("src/init.cpp", '                   strprintf("Relay and mine transactions whose data-carrying raw scriptPubKey "', "datacarrier scriptPubKey help"),
        ("doc/descriptors.md", "     this in the GUI by going to the `Send` tab in the multisig vault and creating an unsigned transaction (PSQT)", "Send-tab product copy"),
        ("src/outputtype.h", "CTxDestination GetDestinationForKey(const CPubKey& key, OutputType);", "dead GetDestinationForKey helpers"),
        ("src/outputtype.h", "std::vector<CTxDestination> GetAllDestinationsForKey(const CPubKey& key);", "dead GetDestinationForKey helpers"),
        ("src/rpc/mining.cpp", "    const bool fPreSegWit = !DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_SEGWIT);", "pre-segwit GBT branch"),
        ("test/functional/test_runner.py", "    'feature_presegwit_node_upgrade.py',", "pre-segwit node-upgrade test"),
        ("src/qt/quicksilverunits.h", '#define REAL_THIN_SP_UTF8 "\\xE2\\x80\\x89"', "dead thin-space UTF-8 macros"),
        ("src/qt/quicksilverunits.h", "#define THIN_SP_UTF8 REAL_THIN_SP_UTF8", "dead thin-space UTF-8 macros"),
        ("src/node/interface_ui.h", "        ICON_MASK = (ICON_INFORMATION | ICON_WARNING | ICON_ERROR),", "dead message-box icon mask"),
        ("src/protocol.h", "    static constexpr SerParams V1_DISK{{CNetAddr::Encoding::V1}, Format::Disk};", "CAddress V1 disk format"),
        ("src/protocol.h", "            if (stored_format_version == 0) {", "CAddress V1 disk version-0 path"),
        ("src/qt/forms/optionsdialog.ui", "             <string>Whether to show coin control features or not.</string>", "coin-control product copy"),
        ("src/qt/forms/optionsdialog.ui", "             <string>Enable coin &amp;control features</string>", "coin-control product copy"),
        ("src/qt/forms/optionsdialog.ui", "               <string>Full path to a %1 compatible script (e.g. C:\\Downloads\\hwi.exe or /Users/you/Downloads/hwi.py). Beware: malware can steal your coins!</string>", "malware steal coins copy"),
        ("src/qt/forms/overviewpage.ui", "               <string>Balance handed to an agent. These coins are still yours, but only the agent can spend them until you sweep them back.</string>", "delegated coins-are-yours copy"),
        ("src/qt/paymentserver.cpp", "                               \"Due to widespread security flaws in BIP70 it's strongly recommended that any merchant instructions to switch vaults be ignored.\\n\"", "BIP70 merchant-instruction copy"),
        ("contrib/linearize/example-linearize.cfg", "max_height=313000", "bitcoin-era linearize height"),
        ("contrib/linearize/linearize-hashes.py", "        settings['max_height'] = 313000", "bitcoin-era linearize height"),
        ("contrib/linearize/example-linearize.cfg", "output_file=/home/example/Downloads/bootstrap.dat", "bootstrap.dat linearize example"),
        ("src/rpc/blockchain.cpp", '                {RPCResult::Type::NUM, "swtotal_size", /*optional=*/true, "Total size of all segwit transactions"},', "bitcoin-era JSON getblockstats sw* keys"),
        ("src/rpc/blockchain.cpp", '    ret_all.pushKV("witness_total_size", witness_total_size);', None),
        ("src/rpc/blockchain.cpp", '            if (stat == "swtotal_size") {', None),
        ("test/functional/data/rpc_getblockstats.json", '      "swtotal_size": 0,', "bitcoin-era JSON getblockstats sw* keys"),
        ("test/functional/data/rpc_getblockstats.json", '      "witness_total_size": 0,', None),
        ("src/rpc/rawtransaction.cpp", '            r.pushKV("segwit", std::move(sr));', "bitcoin-era JSON decodescript segwit key"),
        ("src/rpc/rawtransaction.cpp", '            r.pushKV("witness", std::move(sr));', None),
        ("src/qt/quicksilvergui.cpp", '    agentAllotmentAction->setStatusTip(tr("Fund and review shared-key agent wallets"));', "agent-wallet product copy"),
        ("src/qt/quicksilvergui.cpp", '    agentAllotmentAction->setStatusTip(tr("Fund and review shared-key agent vaults"));', "agent-vault product copy"),
        ("src/qt/quicksilvergui.cpp", '    agentAllotmentAction->setStatusTip(tr("Fund and review shared-key agent allotments"));', None),
        ("src/vault/rpc/coins.cpp", '                {"minimumAmount", UniValueType()},', "camelCase fund option"),
        ("src/rpc/client.cpp", '    { "listunspent", 4, "minimumAmount"},', "camelCase fund option"),
        ("src/vault/rpc/coins.cpp", '        if (options.exists("minimumAmount")) {', None),
        ("src/vault/rpc/coins.cpp", '            throw JSONRPCError(RPC_INVALID_PARAMETER, "Use minimum_amount instead of minimumAmount");', None),
        ("src/rpc/mining.cpp", '    return RPCHelpMan{"getnetworkworkps",', None),
        ("src/test/txpackage_tests.cpp", "BOOST_AUTO_TEST_CASE(package_no_work_subsidy_tests)", None),
        ("src/test/miner_tests.cpp", "    void TestSurplusWorkMining(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", None),
        ("src/test/miner_tests.cpp", "    void TestNoPackageBoost(const CScript& scriptPubKey, const std::vector<CTransactionRef>& txFirst) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", None),
        ("doc/build-osx.md", "###### Descriptor Vault Support", "descriptor vault product copy"),
        ("CMakeLists.txt", '  message("   - descriptor vaults (SQLite) ...... ${WITH_SQLITE}")', "descriptor vault product copy"),
        ("CMakeLists.txt", '  message("   - SQLite ........................ ${WITH_SQLITE}")', None),
        ("src/vault/vault.h", "    VAULT_FLAG_DESCRIPTORS = (1ULL << 34),", None),
        ("src/vault/scriptpubkeyman.h", "class DescriptorScriptPubKeyMan : public ScriptPubKeyMan {", None),
        ("src/vault/test/vaultload_tests.cpp", "    VaultDescriptor vault_descriptor(std::make_shared<DummyDescriptor>(unknown_desc), 0, 0, 0, 0);", None),
        ("src/core_write.cpp", '            out.pushKV("scriptPubKey", std::move(o));', "bitcoin-era JSON scriptPubKey key"),
        ("src/core_write.cpp", '            in.pushKV("scriptSig", std::move(o));', "bitcoin-era JSON scriptSig key"),
        ("src/vault/rpc/coins.cpp", '                        entry.pushKV("redeemScript", HexStr(redeemScript));', "bitcoin-era JSON redeemScript key"),
        ("src/vault/rpc/coins.cpp", '                        entry.pushKV("witnessScript", HexStr(witnessScript));', "bitcoin-era JSON witnessScript key"),
        ("src/test/rpc_tests.cpp", '      "\\"vout\\":1,\\"scriptPubKey\\":\\"a914b10c9df5f7edf436c697f02f1efdba4cf399615187\\","', "bitcoin-era JSON scriptPubKey key"),
        ("src/test/rpc_tests.cpp", '      "\\"redeemScript\\":\\"512103debedc17b3df2badbcdd86d5feb4562b86fe182e5998abd8bcd4f122c6155b1b21027e940bb73ab8732bfdf7f9216ecefca5b94d6df834e77e108f68e66f126044c052ae\\"}]"', "bitcoin-era JSON redeemScript key"),
        ("src/test/rpc_tests.cpp", '      "\\"scriptSig\\":\\"\\","', "bitcoin-era JSON scriptSig key"),
        ("src/test/rpc_tests.cpp", '      "\\"witnessScript\\":\\"\\","', "bitcoin-era JSON witnessScript key"),
        ("src/test/rpc_tests.cpp", '      "\\"vout\\":1,\\"output_script\\":\\"a914b10c9df5f7edf436c697f02f1efdba4cf399615187\\","', None),
        ("src/test/rpc_tests.cpp", '      "\\"redeem_script\\":\\"512103debedc17b3df2badbcdd86d5feb4562b86fe182e5998abd8bcd4f122c6155b1b21027e940bb73ab8732bfdf7f9216ecefca5b94d6df834e77e108f68e66f126044c052ae\\"}]"', None),
        ("src/rpc/rawtransaction_util.cpp", '        if (prevOut.exists("scriptPubKey")) {', None),
        ("src/rpc/rawtransaction_util.cpp", '            throw JSONRPCError(RPC_INVALID_PARAMETER, "Use output_script instead of scriptPubKey");', None),
        ("src/rpc/rawtransaction_util.cpp", '        if (prevOut.exists("redeemScript")) {', None),
        ("src/rpc/rawtransaction_util.cpp", '            throw JSONRPCError(RPC_INVALID_PARAMETER, "Use redeem_script instead of redeemScript");', None),
        ("src/rpc/rawtransaction_util.cpp", '        if (prevOut.exists("witnessScript")) {', None),
        ("src/rpc/rawtransaction_util.cpp", '            throw JSONRPCError(RPC_INVALID_PARAMETER, "Use witness_script instead of witnessScript");', None),
        ("src/quicksilver-tx.cpp", '            if (prevOut.exists("scriptPubKey")) {', None),
        ("src/quicksilver-tx.cpp", '                throw std::runtime_error("Use output_script instead of scriptPubKey");', None),
        ("src/core_write.cpp", '        out.pushKV("output_script", std::move(o));', None),
        ("src/primitives/transaction.h", "    CScript scriptPubKey;", None),
        ("src/qt/quicksilvergui.cpp", 'QStringLiteral(":/icons/eye")', None),
        ("src/crypto/cuckatoo/bench/gpu_csv.h", "bool GpuCsvOk(const GpuCsvVerdict& v);", "dead GPU CSV verdict helper"),
        ("src/qt/sendcoinsrecipient.h", "std::string sPaymentRequest;", "dead BIP70 payment-request field"),
        ("src/qt/transactiondesc.cpp", 'if (r.first == "PaymentRequest")', "dead BIP70 payment-request object"),
        ("src/qt/transactionrecord.cpp", "// Sent to IP, or other non-address transaction like OP_EVAL", "dead IP-payment / OP_EVAL residue"),
        ("src/qt/README.md", "- (Deprecated) Used to process BIP21 payment URI requests.", "BIP21 payment server mislabeled deprecated"),
        ("src/interfaces/vault.h", "virtual bool isLegacy() = 0;", "dead isLegacy abstraction"),
        ("src/bench/vault_ismine.cpp", "static void VaultIsMine(benchmark::Bench& bench, bool legacy_vault, int num_combo = 0)", "dead legacy_vault abstraction"),
        ("src/test/fuzz/tx_pool.cpp", "void RollingFeeUpdate() EXCLUSIVE_LOCKS_REQUIRED(!cs)", "dead RollingFeeUpdate hook"),
        ("src/test/fuzz/package_eval.cpp", "const auto amount_fee = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, amount_in);", "dead amount_fee residue"),
        ("src/vault/spend.cpp", "return util::Error{strprintf(_(\"Not solvable pre-selected input %s\"), outpoint.ToString())}; // Not solvable, can't estimate size for fee", "fee-era prose"),
        ("src/netbase.cpp", 'if (net == "tor") {', "tor-as-onion compatibility alias"),
        ("src/test/netbase_tests.cpp", 'BOOST_CHECK_EQUAL(ParseNetwork("tor"), NET_ONION);', "tor-as-onion compatibility alias"),
        ("src/test/netbase_tests.cpp", 'BOOST_CHECK_EQUAL(ParseNetwork("tor"), NET_UNROUTABLE);', None),
        ("src/init.cpp", 'argsman.AddArg("-allowignoredconf",', "allowignoredconf compatibility flag"),
        ("src/rpc/rawtransaction.cpp", '"For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\\n"', "dictionary-form RPC compatibility"),
        ("src/rpc/rawtransaction_util.cpp", "    const bool outputs_is_obj = outputs_in.isObject();", "dictionary-form outputs implementation"),
        ("src/vault/rpc/spend.cpp", '                {"changeAddress", UniValueType(UniValue::VSTR)},', "camelCase fund option"),
        ("src/rpc/client.cpp", '    { "fundrawtransaction", 1, "changePosition"},', "camelCase fund option"),
        ("src/vault/coinselection.h", "    CAmount GetEffectiveValue() const { return txout.nValue; }", "fee-era effective value"),
        ("src/vault/coincontrol.h", "    std::optional<unsigned int> m_confirm_target;", "dead confirm-target"),
        ("src/validation.cpp", 'state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-BIP30",', "historical duplicate-tx exception"),
        ("src/addrman_impl.h", "V3_BIP155 = 3,", "pre-Quicksilver addrman format"),
        ("src/vault/vaultdb.h", "static const int VERSION_WITH_HDDATA=10;", "pre-Quicksilver key metadata version"),
        ("src/vault/test/vault_tests.cpp", "struct LegacyAgentWalletRecordV2 {", "pre-Quicksilver agent vault record"),
        ("src/vault/vaultutil.h", "static constexpr int FEATURE_LATEST = 169900;", "inherited vault feature stamp"),
        ("src/txdb.cpp", "static constexpr uint8_t DB_COINS{'c'};", "old coins database probe"),
        ("src/rpc/rawtransaction.cpp", '{"verbosity|verbose", RPCArg::Type::NUM,', "boolean verbosity alias"),
        ("src/rpc/rawtransaction.cpp", 'result.pushKV("global_xpubs", std::move(global_xpubs));', "legacy decodepsqt result name"),
        ("src/rpc/request.cpp", 'if (jsonrpc_version.get_str() == "1.0") {', "JSON-RPC 1.0 alias"),
        ("src/rpc/request.h", "    V1_LEGACY,", "legacy JSON-RPC server mode"),
        ("src/undo.h", "            unsigned int nVersionDummy;", "prelaunch serialized-format tombstone"),
        ("src/net_processing.cpp", "        if (node.GetCommonVersion() >= SENDHEADERS_VERSION) {", "historical P2P feature gate"),
        ("test/functional/test_framework/test_node.py", "        if self.version_is_at_least(260000):", "previous-release functional harness"),
        ("src/rpc/blockchain.cpp", 'RPCArg::Default{"hash_serialized_3"}', "legacy UTXO hash implementation"),
        ("src/vault/rpc/encrypt.cpp", "If the passphrase was set with a version of this software prior to 25.0", "inherited release migration"),
        ("doc/JSON-RPC-interface.md", "protocol in v27.0 and prior releases.", "upstream release RPC wording"),
        ("doc/descriptors.md", "pkh([d34db33f/44'/0'/0']qpub...)", "SLIP-44 coin type 0 in derivation example"),
        ("src/rpc/output_script.cpp", 'OutputType output_type = OutputType::BASE58;', "createmultisig base58 default"),
        ("src/test/gpu_solver_tests.cpp", "// incident on box1 failed to say for seven hours.", "lab host name"),
        ("src/httprpc.cpp", "Config options rpcuser and rpcpassword will soon be deprecated.", "undocumented rpcuser deprecation"),
        ("src/vault/rpc/spend.cpp", '        if (options.exists("changeAddress")) {', None),
        ("src/vault/rpc/spend.cpp", '        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_address instead of changeAddress");', None),
        ("src/common/settings.cpp", "// Weird behavior preserved for backwards compatibility: Apply negated", "settings compatibility quirk"),
        ("src/qt/optionsmodel.cpp", "    // Migrate and delete legacy GUI settings that have now moved to <datadir>/settings.json.", "legacy Qt settings migration"),
        ("test/functional/feature_minivault_utxo.py", "        # Regtest subsidy ramps every block, so the pre-mined MiniVault", "Regtest capitalization"),
        ("doc/multisig-tutorial.md", "declare -A xpubs", "public xpub/xprv wording"),
        ("test/functional/vault_gethdkeys.py", "        xpub = xpub_info[0][\"qpub\"]", "public xpub/xprv wording"),
        ("src/qt/transactiondesc.cpp", "    else if (wtx.value_map.count(\"from\") && !wtx.value_map[\"from\"].empty())", "dead from-map value"),
        ("src/vault/vaultutil.h", "    //! encrypted. To support this behavior, descriptor vaults unlike legacy vaults", "legacy vault comment"),
        ("test/functional/vault_send.py", "        # Generate future inputs; bech32 spends are smaller than legacy spends,", "legacy address/spend wording"),
        ("src/net_processing.cpp", "        m_txreconciliation = std::make_unique<TxReconciliationTracker>(TXRECONCILIATION_VERSION);", "removed Erlay/txreconciliation surface"),
        ("src/protocol.h", 'inline constexpr const char* SENDTXRCNCL{"sendtxrcncl"};', "removed Erlay/txreconciliation surface"),
        ("src/init.cpp", 'argsman.AddArg("-txreconciliation", strprintf("Enable transaction reconciliations per BIP 330 (default: %d)"', "removed Erlay/txreconciliation surface"),
        ("test/functional/test_framework/p2p.py", '    b"sendtxrcncl": msg_sendtxrcncl,', "removed Erlay/txreconciliation surface"),
        ("src/CMakeLists.txt", "  node/minisketchwrapper.cpp", "removed minisketch dependency"),
        ("cmake/minisketch.cmake", "add_library(minisketch STATIC EXCLUDE_FROM_ALL", "removed minisketch dependency"),
        ("src/rpc/mining.cpp", "static RPCHelpMan generate()", "hidden generate RPC tombstone"),
        ("src/rpc/mining.cpp", '        {"hidden", &generate},', "hidden generate RPC tombstone"),
        ("src/rpc/mining.cpp", '        {"hidden", &generateblock},', None),
        ("contrib/completions/bash/quicksilver-cli.bash", "            listtransactions|setban)", "watchonly listtransactions completion"),
        ("contrib/completions/bash/quicksilver-cli.bash", "            getbalance|getnewaddress|listtransactions|sendmany)", None),
        ("src/vault/vaultutil.h", "    VAULT_FLAG_KEY_ORIGIN_METADATA = (1ULL << 1),", "unused key-origin vault flag"),
        ("src/vault/vault.h", '    {"last_hardened_xpub_cached", VAULT_FLAG_LAST_HARDENED_XPUB_CACHED},', "descriptor-cache upgrade path"),
        ("src/vault/rpc/vault.cpp", '                        {RPCResult::Type::STR_HEX, "hdseedid", /*optional=*/true, "the Hash160 of the HD seed (only present when HD is enabled)"},', "hdseedid RPC field"),
        ("src/vault/rpc/addresses.cpp", '                ret.pushKV("hdseedid", meta->hd_seed_id.GetHex());', "hdseedid RPC field"),
        ("src/vault/vaultdb.h", "    CKeyID hd_seed_id; //id of the HD seed used to derive this key", "unused CKeyMetadata hd_seed_id"),
        ("src/init.cpp", '    argsman.AddArg("-checkpoints", strprintf("Enable rejection of any forks', "empty-table -checkpoints flag"),
        ("src/node/chainstatemanager_args.cpp", "    if (auto value{args.GetBoolArg(\"-checkpoints\")}) opts.checkpoints_enabled = *value;", "empty-table -checkpoints flag"),
        ("src/test/fuzz/p2p_headers_presync.cpp", '    static auto setup = MakeNoLogFileContext<HeadersSyncSetup>(ChainType::MAIN, {.extra_args = {"-checkpoints=0"}});', "empty-table -checkpoints flag"),
        ("src/kernel/chainparams.cpp", "                // Quicksilver: fresh chain, no checkpoints.", None),
        ("src/kernel/chainstatemanager_opts.h", "static constexpr bool DEFAULT_CHECKPOINTS_ENABLED{true};", "checkpoint walker and empty maps"),
        ("src/node/blockstorage.h", "    const CBlockIndex* GetLastCheckpoint(const CCheckpointData& data) EXCLUSIVE_LOCKS_REQUIRED(cs_main);", "checkpoint walker and empty maps"),
        ("src/consensus/validation.h", "    BLOCK_CHECKPOINT,        //!< the block failed to meet one of our checkpoints", "checkpoint walker and empty maps"),
        ("src/kernel/chainparams.cpp", "        checkpointData = {", "checkpoint walker and empty maps"),
        ("src/test/pow_tests.cpp", "    BOOST_CHECK(params.Checkpoints().mapCheckpoints.empty());", "checkpoint walker and empty maps"),
        ("src/validation.cpp", '            return state.Invalid(BlockValidationResult::BLOCK_CHECKPOINT, "bad-fork-prior-to-checkpoint");', "checkpoint walker and empty maps"),
        ("src/net_processing.cpp", "    case BlockValidationResult::BLOCK_INVALID_HEADER:", None),
        ("src/init.cpp", '    argsman.AddArg("-forcednsseed", strprintf("Always query for peer addresses via DNS lookup', "empty-table -forcednsseed flag"),
        ("src/init.cpp", '    argsman.AddArg("-dnsseed", strprintf("Query for peer addresses via DNS lookup', None),
        ("src/vault/vaultdb.cpp", "const std::unordered_set<std::string> UNSUPPORTED_TYPES{CRYPTED_KEY, CSCRIPT, DEFAULTKEY, HDCHAIN, KEYMETA, KEY, OLD_KEY, POOL, WATCHMETA, WATCHS};", "unsupported-types reject set"),
        ("src/vault/vault.cpp", "        } else if (nLoadVaultRet == DBErrors::UNSUPPORTED_RECORD) {", "unsupported-types reject set"),
        ("src/vault/crypter.h", "    unsigned int nDerivationMethod;", "CMasterKey dummy derivation slots"),
        ("src/vault/crypter.h", "    std::vector<unsigned char> vchOtherDerivationParameters;", "CMasterKey dummy derivation slots"),
        ("src/vault/crypter.h", "    unsigned int nDeriveIterations;", None),
        ("src/vault/rpc/vault.cpp", '    obj.pushKV("format", pvault->GetDatabase().Format());', "constant getvaultinfo format field"),
        ("src/vault/rpc/vault.cpp", '                        {RPCResult::Type::STR, "format", "the database format (sqlite)"},', "constant getvaultinfo format field"),
        ("src/vault/rpc/vault.cpp", '    obj.pushKV("descriptors", pvault->IsVaultFlagSet(VAULT_FLAG_DESCRIPTORS));', "constant getvaultinfo descriptors field"),
        ("src/vault/rpc/vault.cpp", '                xpub_info.pushKV("descriptors", std::move(descriptors));', None),
        ("src/vault/vault.cpp", "    uint64_t vault_creation_flags = options.create_flags | VAULT_FLAG_DESCRIPTORS;", None),
        ("src/rpc/mining.cpp", '                    {"str", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "client side supported feature, \'longpoll\', \'coinbasevalue\', \'proposal\', \'serverlist\', \'workid\'"},', "GBT request capabilities help"),
        ("src/rpc/mining.cpp", '    result.pushKV("capabilities", std::move(aCaps));', None),
        ("src/rpc/mining.cpp", '    result.pushKV("coinbaseaux", std::move(aux));', "GBT constant coinbaseaux/vbrequired"),
        ("src/rpc/mining.cpp", '    result.pushKV("vbrequired", int(0));', "GBT constant coinbaseaux/vbrequired"),
        ("src/rpc/mining.cpp", '    result.pushKV("noncerange", "00000000ffffffff");', "GBT hashing-miner noncerange"),
        ("src/rpc/mining.cpp", '                {RPCResult::Type::STR_HEX, "noncerange", "A range of valid nonces"},', "GBT hashing-miner noncerange"),
        ("src/vault/rpc/spend.cpp", '                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, return a JSON object with the transaction id instead of a hex string."},', "dropped send* verbose wrapper"),
        ("src/vault/rpc/spend.cpp", '                    RPCResult{"if verbose is set to true",', "dropped send* verbose wrapper"),
        ("src/test/script_tests.cpp", "BOOST_AUTO_TEST_CASE(script_assets_test)", "never-run script_assets_test"),
        ("src/test/fuzz/CMakeLists.txt", "  script_assets_test_minimizer.cpp", "never-run script_assets_test"),
        ("src/rpc/blockchain.cpp", '        {"blockchain", &loadtxoutset},', "dropped UTXO-snapshot bootstrap surface"),
        ("src/rpc/blockchain.cpp", '        {"blockchain", &dumptxoutset},', "dropped UTXO-snapshot bootstrap surface"),
        ("src/rpc/blockchain.cpp", '        {"blockchain", &getchainstates},', "dropped UTXO-snapshot bootstrap surface"),
        ("src/kernel/chainparams.h", "struct AssumeutxoData {", "dropped assumeUTXO identifier"),
        ("src/kernel/chainparams.h", "struct AssumeutxoHash : public BaseHash<uint256> {", "dropped assumeUTXO identifier"),
        ("src/kernel/chainparams.h", "    std::vector<AssumeutxoData> m_assumeutxo_data;", "dropped assumeUTXO identifier"),
        ("src/kernel/chainparams.h", "    std::optional<AssumeutxoData> AssumeutxoForHeight(int height) const", "dropped assumeUTXO identifier"),
        ("src/kernel/chainparams.h", "    std::optional<AssumeutxoData> AssumeutxoForBlockhash(const uint256& blockhash) const", "dropped assumeUTXO identifier"),
        ("src/kernel/chainparams.h", "    std::vector<int> GetAvailableSnapshotHeights() const;", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    [[nodiscard]] util::Result<CBlockIndex*> ActivateSnapshot(", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    [[nodiscard]] util::Result<void> PopulateAndValidateSnapshot(", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    SnapshotCompletionResult MaybeCompleteSnapshotValidation() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    std::unique_ptr<Chainstate> m_snapshot_chainstate GUARDED_BY(::cs_main);", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    bool DetectSnapshotChainstate() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    Chainstate& ActivateExistingSnapshot(uint256 base_blockhash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    bool ValidatedSnapshotCleanup() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    [[nodiscard]] bool DeleteSnapshotChainstate() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);", "dropped UTXO-snapshot bootstrap surface"),
        ("src/node/utxo_snapshot.h", "static constexpr std::array<uint8_t, 5> SNAPSHOT_MAGIC_BYTES = {'u', 't', 'x', 'o', 0xff};", "dropped UTXO-snapshot bootstrap surface"),
        ("src/node/utxo_snapshot.h", "class SnapshotMetadata", "dropped UTXO-snapshot bootstrap surface"),
        ("src/node/utxo_snapshot.h", 'constexpr std::string_view SNAPSHOT_CHAINSTATE_SUFFIX = "_snapshot";', "dropped UTXO-snapshot bootstrap surface"),
        ("src/rpc/blockchain.h", "UniValue CreateUTXOSnapshot(", "dropped UTXO-snapshot bootstrap surface"),
        ("src/net_processing.cpp", "    void TryDownloadingHistoricalBlocks(const Peer& peer, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, const CBlockIndex* from_tip, const CBlockIndex* target_block) EXCLUSIVE_LOCKS_REQUIRED(cs_main);", "dropped UTXO-snapshot bootstrap surface"),
        ("src/interfaces/chain.h", "    virtual bool hasAssumedValidChain() = 0;", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    bool IsSnapshotActive() const;", "dropped UTXO-snapshot bootstrap surface"),
        ("src/validation.h", "    bool BackgroundSyncInProgress() const EXCLUSIVE_LOCKS_REQUIRED(GetMutex()) {", "dropped UTXO-snapshot bootstrap surface"),
        ("src/coins.h", "    void EmplaceCoinInternalDANGER(COutPoint&& outpoint, Coin&& coin);", "dropped UTXO-snapshot bootstrap surface"),
        ("src/node/blockstorage.h", "    ASSUMED = 1,", None),
        ("src/node/blockstorage.cpp", "        case BlockfileType::ASSUMED: os << \"assumed\"; break;", "dropped UTXO-snapshot bootstrap surface"),
        ("src/node/utxo_snapshot.cpp", '        data_dir / fs::u8path(strprintf("chainstate%s", SNAPSHOT_CHAINSTATE_SUFFIX));', "dropped UTXO-snapshot bootstrap surface"),
        ("contrib/devtools/README.md", "The utxo_to_sqlite helper dumped the UTXO set to sqlite.", "dropped UTXO-snapshot bootstrap surface"),
        ("src/init.cpp", "    argsman.AddArg(\"-reindex\", \"If enabled, wipe chain state and ledger index, and rebuild them from blk*.dat files on disk. Also wipe and rebuild other optional indexes that are active.\", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);", None),
        ("doc/design/chain-identity.md", "- no UTXO-snapshot bootstrap;", None),
        ("ci/test/03_test_script.sh", '  DIR_UNIT_TEST_DATA="${DIR_UNIT_TEST_DATA}" LD_LIBRARY_PATH="${DEPENDS_DIR}/${HOST}/lib" CTEST_OUTPUT_ON_FAILURE=ON ctest --stop-on-failure "${MAKEJOBS}" --timeout $(( TEST_RUNNER_TIMEOUT_FACTOR * 60 ))', "never-run script_assets_test"),
        ("src/test/script_tests.cpp", "BOOST_AUTO_TEST_CASE(bip341_keypath_test_vectors)", None),
        ("src/kernel/chain.h", "enum class ChainstateRole {", "dropped chainstate-role surface"),
        ("src/kernel/chain.cpp", "        case ChainstateRole::ASSUMEDVALID: os << \"assumedvalid\"; break;", "dropped chainstate-role surface"),
        ("src/kernel/chain.cpp", "        case ChainstateRole::BACKGROUND: os << \"background\"; break;", "dropped chainstate-role surface"),
        ("src/validation.h", "    ChainstateRole GetRole() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)", "dropped chainstate-role surface"),
        ("src/node/blockstorage.cpp", "        chain.GetRole(), last_block_can_prune, count);", "dropped chainstate-role surface"),
        ("src/validationinterface.h", "    virtual void BlockConnected(ChainstateRole role, const std::shared_ptr<const CBlock> &block, const CBlockIndex *pindex) {}", "dropped chainstate-role surface"),
        ("src/node/blockstorage.h", "enum BlockfileType {", "dropped dual-blockfile type"),
        ("src/node/blockstorage.cpp", "BlockfileType BlockManager::BlockfileTypeForHeight(int height)", "dropped dual-blockfile type"),
        ("src/node/blockstorage.cpp", "    return BlockfileType::NORMAL;", "dropped dual-blockfile type"),
        ("src/node/blockstorage.h", "        const auto& normal = m_blockfile_cursors[BlockfileType::NORMAL].value_or(empty_cursor);", "dropped dual-blockfile type"),
        ("src/kernel/chain.h", "    BACKGROUND,", None),
        ("src/init.cpp", '    argsman.AddArg("-assumevalid=<hex>", strprintf("If this block is in the chain assume that it and its ancestors are valid and potentially skip their script verification", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);', None),
        ("src/consensus/params.h", "    uint256 defaultAssumeValid;", None),
        ("src/node/blockstorage.h", "struct BlockfileCursor {", None),
        ("src/init.cpp", '    argsman.AddArg("-whitelistrelay", "Add relay permission", ArgsManager::ALLOW_ANY, OptionsCategory::NODE_RELAY);', "removed implicit-whitelist compatibility flags"),
        ("src/rpc/mining.cpp", '            {"dummy", RPCArg::Type::STR, RPCArg::DefaultHint{"ignored"}, "dummy value, for compatibility with BIP22."},', "ignored BIP22 submitblock argument"),
        ("src/rpc/protocol.h", "    RPC_TRANSACTION_ERROR = RPC_VERIFY_ERROR,", "obsolete RPC transaction-error alias"),
        ("src/vault/vault.cpp", "void CVault::UpgradeDescriptorCache()", "descriptor-cache upgrade path"),
        ("src/vault/vault.cpp", "DBErrors CVault::ReorderTransactions()", "unordered vault-transaction upgrade"),
        ("src/vault/vaultdb.cpp", '    pvault->VaultLogPrintf("pruned keys=extraneous-encryption reason=no-private-keys");', "vault-load auto-repair"),
        ("src/interfaces/chain.h", "    const CBlockUndo* undo_data = nullptr;", "unused BlockInfo undo payload"),
        ("src/index/coinstatsindex.cpp", "        // will be removed in upcoming commit", "stale future-commit note"),
        ("src/test/fuzz/txdownloadman.cpp", "// it may request low-feerate parent of orphan.", "fee-era reconsiderable-parent wording"),
        # Negative / out-of-scope cases
        ("src/vault/spend.cpp", "        // For backwards compatibility, we convert P2PK output scripts into PKHash destinations", None),
        ("src/logging.cpp", '    {"tor", HgLog::TOR},', None),
        ("src/test/txpackage_tests.cpp", "    // low-feerate \"invalid on its own\" parent, package-feerate assertions) is dropped.", None),
        ("doc/design/network-naming.md", "| `regtest` | Quicksilver Sandbox | `sandbox` | `ChainType::SANDBOX` |", None),
        ("test/functional/vault_taproot.py", '        "xpub": "squb6UShJgvLuWbXipGNCNRv3Pr9txKiTmqRtD1GWiRyb5xMMpJrjaZwsaKr48tBeXTbipjPmwadie4Fw2FLhDFVjJSYBtDxkazyNGqfbkLi4ut",', None),
        ("src/vault/rpc/spend.cpp", "        + HelpExampleCli(\"send\", \"'{\\\"\" + EXAMPLE_ADDRESS[0] + \"\\\": 0.1}'\\n\") +", None),
    ]
    # (relpath, line, must_be_allowed) — exercises the ALLOWED exemption map,
    # which match_rule alone does not consult.
    allowed_cases = [
        ("src/test/net_tests.cpp", '    BOOST_CHECK(std::ranges::find(ALL_NET_MESSAGE_TYPES, "sendtxrcncl") == ALL_NET_MESSAGE_TYPES.end());', True),
        ("src/test/net_tests.cpp", "    m_txreconciliation->ForgetPeer(nodeid);", False),
        ("test/functional/feature_config_args.py", "            expected_msg='Error: Error parsing command line arguments: Invalid parameter -checkpoints=0',", True),
        ("test/functional/feature_config_args.py", "            expected_msg='Error: Error parsing command line arguments: Invalid parameter -forcednsseed=1',", True),
        ("test/functional/feature_config_args.py", "            'Error: Error parsing command line arguments: Invalid parameter -whitelistrelay')", True),
        ("test/functional/feature_config_args.py", "            ['-whitelistforcerelay'],", True),
        ("test/functional/vault_avoidreuse.py", "        assert_raises_rpc_error(-8, \"Unknown vault flag: key_origin_metadata\", self.nodes[0].setvaultflag, 'key_origin_metadata', True)", True),
    ]
    failures = []
    for rel, line, should_allow in allowed_cases:
        if line_allowed(Path(rel), line) != should_allow:
            failures.append(f"{rel}: {line!r} ALLOWED expected {should_allow}")
    for rel, line, expected in cases:
        rule = match_rule(rel, line)
        got = rule.name if rule else None
        if got != expected:
            failures.append(f"{rel}: {line!r} expected {expected!r}, got {got!r}")
    if failures:
        print("Dead-surface residue lint self-test failures:")
        print("\n".join(failures))
        return 1
    if matcher_failures != 0:
        return 1
    print("Dead-surface residue lint self-test OK")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if len(sys.argv) != 1:
        print(f"Usage: {sys.argv[0]} [--self-test]", file=sys.stderr)
        return 2

    if report_self_test() != 0:
        return 1

    root = repo_root()
    failures = []
    for path in tracked_files(root):
        if not path.exists() or is_binary(path):
            continue
        relpath = path.relative_to(root)
        rel = relpath.as_posix()
        try:
            lines = path.read_text(encoding="utf8", errors="replace").splitlines()
        except OSError as err:
            print(f"failed to read {relpath}: {err}", file=sys.stderr)
            return 1
        for line_number, line in enumerate(lines, start=1):
            if line_allowed(relpath, line):
                continue
            rule = match_rule(rel, line)
            if rule is not None:
                failures.append(f"{relpath}:{line_number}: {rule.name}: {line.strip()}")
        failures.extend(wrapped_failures(relpath, rel, lines))

    if failures:
        print("Quicksilver dead-surface residue remains:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
