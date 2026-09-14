// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/storagecoststests.h>

#include <qt/consensusreviewpage.h>
#include <qt/desktoplaunchpage.h>
#include <qt/intro.h>
#include <qt/platformstyle.h>

#include <QCheckBox>
#include <QLabel>
#include <QString>

#include <memory>

namespace {
//! The mainnet and publictest figures from chainparams.cpp, injected directly so these
//! assertions hold at the numbers a real user sees. The Qt suite selects sandbox, where
//! both sizes are zero, so reading them from Params() here would assert nothing.
constexpr uint64_t BLOCKCHAIN_SIZE_GB{128};
constexpr uint64_t CHAIN_STATE_SIZE_GB{26};

//! 128 + 26. Written out rather than computed: a test that repeats the implementation's
//! arithmetic is not an oracle. If chainparams moves, these must fail and be re-read.
const QString ARCHIVAL_TOTAL{QStringLiteral("154 GB per year")};
const QString CHAIN_STATE{QStringLiteral("26 GB per year")};
const QString PRUNE_WINDOW{QStringLiteral("2 GB recent-block window")};
} // namespace

//! F-126: this screen hardcoded "152 GB per year" while the intro dialog next door
//! derived 154 from the same two chainparams values. 152 is M5's measured total, which
//! carries a negative block_index term from a LevelDB compaction that happened during
//! the measurement window -- noise, not a saving an operator ever sees. The figure the
//! product quotes is the sum chainparams publishes. See doc/design/chain-storage.md.
void StorageCostsTests::launchPageQuotesTheDerivedArchivalTotal()
{
    const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(QStringLiteral("other"))};
    DesktopLaunchPage page(style.get(), BLOCKCHAIN_SIZE_GB, CHAIN_STATE_SIZE_GB, nullptr);

    QLabel* cost = page.findChild<QLabel*>(QStringLiteral("desktopLaunchCostCopy"));
    QVERIFY(cost);
    const QString text = cost->text();

    QVERIFY(text.contains(PRUNE_WINDOW));
    QVERIFY(text.contains(CHAIN_STATE));
    QVERIFY(text.contains(ARCHIVAL_TOTAL));
    QVERIFY(!text.contains(QStringLiteral("152")));
}

void StorageCostsTests::consensusReviewQuotesTheDerivedArchivalTotal()
{
    const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(QStringLiteral("other"))};
    ConsensusReviewPage page(style.get(), BLOCKCHAIN_SIZE_GB, CHAIN_STATE_SIZE_GB, nullptr);

    QLabel* cost = page.findChild<QLabel*>(QStringLiteral("consensusStorageCostValue"));
    QVERIFY(cost);
    const QString text = cost->text();

    QVERIFY(text.contains(PRUNE_WINDOW));
    QVERIFY(text.contains(CHAIN_STATE));
    QVERIFY(text.contains(ARCHIVAL_TOTAL));
    QVERIFY(!text.contains(QStringLiteral("152")));
}

//! The defect was never one wrong number -- it was two adjacent screens disagreeing with
//! a third about the same quantity. This is the assertion that would have caught it.
void StorageCostsTests::bothScreensAgreeWithIntroOnTheArchivalTotal()
{
    const std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate(QStringLiteral("other"))};
    DesktopLaunchPage launch(style.get(), BLOCKCHAIN_SIZE_GB, CHAIN_STATE_SIZE_GB, nullptr);
    ConsensusReviewPage review(style.get(), BLOCKCHAIN_SIZE_GB, CHAIN_STATE_SIZE_GB, nullptr);
    Intro intro(nullptr, BLOCKCHAIN_SIZE_GB, CHAIN_STATE_SIZE_GB);

    // The intro ships prune checked (intro.cpp), so by default it quotes the pruned
    // requirement. Unchecking it is what makes it state the archival total, the same
    // quantity the two desktop screens describe as a yearly growth figure.
    QCheckBox* prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(prune);
    prune->setChecked(false);
    QLabel* intro_storage = intro.findChild<QLabel*>(QStringLiteral("sizeWarningLabel"));
    QVERIFY(intro_storage);

    const QString total{QStringLiteral("154 GB")};
    QVERIFY(launch.findChild<QLabel*>(QStringLiteral("desktopLaunchCostCopy"))->text().contains(total));
    QVERIFY(review.findChild<QLabel*>(QStringLiteral("consensusStorageCostValue"))->text().contains(total));
    QVERIFY(intro_storage->text().contains(total));
}
