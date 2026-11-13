// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/storagecoststests.h>

#include <kernel/chainparams.h>
#include <qt/consensusreviewpage.h>
#include <qt/desktoplaunchpage.h>
#include <qt/intro.h>
#include <qt/platformstyle.h>

#include <QCheckBox>
#include <QLabel>
#include <QString>

#include <memory>
#include <utility>

namespace {
//! The mainnet and publictest figures from chainparams.cpp, injected directly so these
//! assertions hold at the numbers a real user sees. The Qt suite selects sandbox, where
//! both sizes are zero, so reading them from Params() here would assert nothing.
//! F-601 (owner, 2026-11-12): 1 / 1 GB, an estimate from the measured chain plus a
//! margin, replacing the 128 / 26 GB full-traffic capacity figures.
constexpr uint64_t BLOCKCHAIN_SIZE_GB{1};
constexpr uint64_t CHAIN_STATE_SIZE_GB{1};

//! F-611 (owner, 2026-11-12): what a vault can and cannot do with consensus off. It used
//! to say consensus was "not required to use a vault"; with consensus off the vault
//! cannot see an incoming payment or send.
const QString VAULT_WITHOUT_CONSENSUS{QStringLiteral("Without consensus, the vault keeps your keys and addresses but cannot see incoming payments or send.")};
//! F-611 (owner, 2026-11-13): the review page's "Vault requirement" row. It used to say
//! consensus was "not required for holding, requesting, or transferring quicksilver",
//! which contradicted VAULT_WITHOUT_CONSENSUS two lines above it.
const QString VAULT_REQUIREMENT_COPY{QStringLiteral("Needed to see incoming payments and to send. Not needed to keep keys or give out addresses.")};

//! The signed sentences, written out rather than computed: a test that repeats the
//! implementation's arithmetic is not an oracle. If chainparams moves, these must fail
//! and be re-read.
const QString UNMEASURED_COPY{QStringLiteral("Transfers require proof-of-work. Enabling consensus records up to 2 GB of blockchain data. ") + VAULT_WITHOUT_CONSENSUS};
const QString LAUNCH_MEASURED_COPY{QStringLiteral("Transfers spend proof-of-work. Consensus storage is estimated at up to about 2 GB a year: 1 GB of block data and 1 GB of chain state. Default pruning caps the block data at 2 GB. ") + VAULT_WITHOUT_CONSENSUS};
const QString REVIEW_MEASURED_COPY{QStringLiteral("Storage is estimated at up to about 2 GB a year: 1 GB of block data and 1 GB of chain state. Default pruning caps the block data at 2 GB.")};

//! At 1 / 1 the block-data and chain-state figures are the same number, so the real
//! figures cannot show that each argument lands in its own place. Distinct figures can.
constexpr uint64_t DISTINCT_BLOCKCHAIN_GB{3};
constexpr uint64_t DISTINCT_CHAIN_STATE_GB{5};
const QString DISTINCT_SPLIT{QStringLiteral("estimated at up to about 8 GB a year: 3 GB of block data and 5 GB of chain state. Default pruning caps the block data at 2 GB.")};
} // namespace

void StorageCostsTests::launchPageUsesUnmeasuredNetworkCopy()
{
    const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(QStringLiteral("other"))};
    DesktopLaunchPage page(style.get(), 0, 0, nullptr);
    auto* cost = page.findChild<QLabel*>(QStringLiteral("desktopLaunchCostCopy"));
    QVERIFY(cost);
    QCOMPARE(cost->text(), UNMEASURED_COPY);
}

void StorageCostsTests::consensusReviewUsesUnmeasuredNetworkCopy()
{
    ConsensusReviewPage page(0, 0, nullptr);
    auto* cost = page.findChild<QLabel*>(QStringLiteral("consensusStorageCostValue"));
    QVERIFY(cost);
    QCOMPARE(cost->text(), UNMEASURED_COPY);
}

//! F-126: this screen hardcoded "152 GB per year" while the intro dialog next door
//! derived 154 from the same two chainparams values. 152 is M5's measured total, which
//! carries a negative block_index term from a LevelDB compaction that happened during
//! the measurement window -- noise, not a saving an operator ever sees. The figure the
//! product quotes is the sum chainparams publishes. See doc/design/chain-storage.md.
void StorageCostsTests::launchPageQuotesTheDerivedArchivalTotal()
{
    const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(QStringLiteral("other"))};
    // The injected figures are the ones main and publictest publish.
    for (const auto& params : {CChainParams::Main(), CChainParams::PublicTest()}) {
        QCOMPARE(params->AssumedBlockchainSize(), BLOCKCHAIN_SIZE_GB);
        QCOMPARE(params->AssumedChainStateSize(), CHAIN_STATE_SIZE_GB);
    }
    DesktopLaunchPage page(style.get(), BLOCKCHAIN_SIZE_GB, CHAIN_STATE_SIZE_GB, nullptr);

    QLabel* cost = page.findChild<QLabel*>(QStringLiteral("desktopLaunchCostCopy"));
    QVERIFY(cost);
    const QString text = cost->text();

    QCOMPARE(text, LAUNCH_MEASURED_COPY);
    QVERIFY(!text.contains(QStringLiteral("152")));

    DesktopLaunchPage distinct(style.get(), DISTINCT_BLOCKCHAIN_GB, DISTINCT_CHAIN_STATE_GB, nullptr);
    QVERIFY(distinct.findChild<QLabel*>(QStringLiteral("desktopLaunchCostCopy"))->text().contains(DISTINCT_SPLIT));
}

void StorageCostsTests::consensusReviewQuotesTheDerivedArchivalTotal()
{
    ConsensusReviewPage page(BLOCKCHAIN_SIZE_GB, CHAIN_STATE_SIZE_GB, nullptr);

    QLabel* cost = page.findChild<QLabel*>(QStringLiteral("consensusStorageCostValue"));
    QVERIFY(cost);
    const QString text = cost->text();

    QCOMPARE(text, REVIEW_MEASURED_COPY);
    QVERIFY(!text.contains(QStringLiteral("152")));

    ConsensusReviewPage distinct(DISTINCT_BLOCKCHAIN_GB, DISTINCT_CHAIN_STATE_GB, nullptr);
    QVERIFY(distinct.findChild<QLabel*>(QStringLiteral("consensusStorageCostValue"))->text().contains(DISTINCT_SPLIT));
}

//! The defect was never one wrong number -- it was two adjacent screens disagreeing with
//! a third about the same quantity. This is the assertion that would have caught it.
void StorageCostsTests::bothScreensAgreeWithIntroOnTheArchivalTotal()
{
    const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(QStringLiteral("other"))};
    // Distinct injected figures: at the real 1 / 1 the total is 2 GB, the same number as
    // the default prune limit, so a match could come from the wrong figure.
    DesktopLaunchPage launch(style.get(), DISTINCT_BLOCKCHAIN_GB, DISTINCT_CHAIN_STATE_GB, nullptr);
    ConsensusReviewPage review(DISTINCT_BLOCKCHAIN_GB, DISTINCT_CHAIN_STATE_GB, nullptr);
    Intro intro(nullptr, DISTINCT_BLOCKCHAIN_GB, DISTINCT_CHAIN_STATE_GB);

    // The intro ships prune checked (intro.cpp), so by default it quotes the pruned
    // requirement. Unchecking it is what makes it state the archival total, the same
    // quantity the two desktop screens describe as a yearly growth figure.
    QCheckBox* prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(prune);
    prune->setChecked(false);
    QLabel* intro_storage = intro.findChild<QLabel*>(QStringLiteral("sizeWarningLabel"));
    QVERIFY(intro_storage);

    const QString total{QStringLiteral("8 GB")};
    QVERIFY(launch.findChild<QLabel*>(QStringLiteral("desktopLaunchCostCopy"))->text().contains(total));
    QVERIFY(review.findChild<QLabel*>(QStringLiteral("consensusStorageCostValue"))->text().contains(total));
    QVERIFY(intro_storage->text().contains(total));
}

//! F-611: the row is the same on every chain, so check it at the unmeasured (sandbox,
//! regtest) and the measured (main, publictest) figures.
void StorageCostsTests::consensusReviewVaultRequirementSaysWhatTheVaultNeedsConsensusFor()
{
    for (const auto& [blockchain_gb, chain_state_gb] : {std::pair<uint64_t, uint64_t>{0, 0}, {BLOCKCHAIN_SIZE_GB, CHAIN_STATE_SIZE_GB}}) {
        ConsensusReviewPage page(blockchain_gb, chain_state_gb, nullptr);
        auto* row = page.findChild<QLabel*>(QStringLiteral("consensusVaultCostValue"));
        QVERIFY(row);
        QCOMPARE(row->text(), VAULT_REQUIREMENT_COPY);
    }
}
