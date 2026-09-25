// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/introtests.h>

#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/intro.h>

#include <util/fs.h>

#include <fstream>

#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QRegularExpression>
#include <QSpinBox>
#include <QString>
#include <QTemporaryDir>

//! The Phase 6 S2 walk found both of these live on the first screen of a clean install.
void IntroTests::explanationLeavesNoPlaceholderAndNoInheritedYear()
{
    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);
    QLabel* explanation = intro.findChild<QLabel*>(QStringLiteral("lblExplanation1"));
    QVERIFY(explanation);
    const QString text = explanation->text();

    // Quicksilver has never launched, so no year can be claimed for its first transfers.
    // The inherited string named 2009, a year belonging to the chain this one forked from.
    QVERIFY(!text.contains(QStringLiteral("2009")));

    // Every placeholder must be consumed. QString::arg leaves unmatched markers in place, so
    // an argument list that falls out of step with the .ui string shows the user a raw "%3".
    QVERIFY(!text.contains(QRegularExpression(QStringLiteral("%[0-9]"))));

    // The figure that is claimed has to be the one passed in.
    QVERIFY(text.contains(QStringLiteral("128")));
}

//! Phase 6 S1-8: the first screen still described the initial block download of the
//! chain this one forked from.
//!
//! The figures on it are ours and correct -- 128 GB is first-year *growth*, documented
//! at chainparams.cpp and doc/design/chain-storage.md -- but the sentences around them
//! were inherited, and they promise a download of history that does not exist.
//! Quicksilver starts at height 0: on launch day there is nothing to download. The
//! replacement has to stay true a year later, when there is.
void IntroTests::explanationDoesNotPromiseADownloadOfHistoryThatMayNotExist()
{
    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);

    QLabel* first_label = intro.findChild<QLabel*>(QStringLiteral("lblExplanation1"));
    QVERIFY(first_label);
    const QString first = first_label->text();
    // The figure is a projection of what the chain will grow by, not the weight of a
    // download waiting to happen.
    QVERIFY(first.contains(QStringLiteral("first year")));
    QVERIFY(!first.contains(QStringLiteral("download and process the full block chain")));
    // G12: clicking OK does not start a node. The desktop shows the main window with
    // consensus Optional and not running, and nothing downloads or validates until the
    // separate opt-in -- so this screen must not describe OK as the moment it begins.
    QVERIFY(!first.contains(QStringLiteral("When you click OK")));
    QVERIFY(!first.contains(QStringLiteral("downloading and validating")));
    // It still has to say what the storage figure is for, or the consent is meaningless.
    QVERIFY(first.contains(QStringLiteral("Consensus")));
    QVERIFY(!first.contains(QRegularExpression(QStringLiteral("%[0-9]"))));

    QLabel* second_label = intro.findChild<QLabel*>(QStringLiteral("lblExplanation2"));
    QVERIFY(second_label);
    const QString second = second_label->text();
    // There is no initial synchronisation on a chain nobody has mined yet.
    QVERIFY(!second.contains(QStringLiteral("initial synchronisation")));
    // The hardware warning is worth keeping -- validation is demanding whenever it happens.
    QVERIFY(second.contains(QStringLiteral("hardware problems")));
    QVERIFY(!second.contains(QRegularExpression(QStringLiteral("%[0-9]"))));

    QLabel* pruning = intro.findChild<QLabel*>(QStringLiteral("lblExplanation3"));
    QVERIFY(pruning);
    const QString pruning_text = pruning->text();
    // F-113: the first-run screen used to show 28 GB retained and 128 GB
    // processed without saying why both were true. Spell out the 2 + 26 GB
    // retained split and identify 128 GB as processed block volume.
    QVERIFY(pruning_text.contains(QStringLiteral("2 GB of recent blocks")));
    QVERIFY(pruning_text.contains(QStringLiteral("26 GB of chain state")));
    QVERIFY(pruning_text.contains(QStringLiteral("about 28 GB on disk")));
    QVERIFY(pruning_text.contains(QStringLiteral("128 GB first-year block-data")));
    QVERIFY(pruning_text.contains(QStringLiteral("processed volume, not retained storage")));
    QVERIFY(!pruning_text.contains(QRegularExpression(QStringLiteral("%[0-9]"))));

    // The storage panel carried the same promise in shorter form.
    QLabel* size_warning = intro.findChild<QLabel*>(QStringLiteral("sizeWarningLabel"));
    QVERIFY(size_warning);
    QVERIFY(!size_warning->text().contains(QStringLiteral("will download and store")));
    QVERIFY(size_warning->text().contains(QStringLiteral("When Consensus is running")));
}

void IntroTests::pruneSuffixReadsGrammaticallyWithNoCatalogue()
{
    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);
    QLabel* suffix = intro.findChild<QLabel*>(QStringLiteral("lblPruneSuffix"));
    QVERIFY(suffix);
    const QString text = suffix->text();

    // No translator is installed here, and none is installed for a user whose locale has no
    // catalogue. Qt's "%n day(s)" idiom renders the literal "(s)" in that case.
    QVERIFY(!text.contains(QStringLiteral("(s)")));
    QVERIFY(!text.contains(QStringLiteral("%n")));
    QVERIFY(text.contains(QStringLiteral("day")));
}

//! Phase 6 S3. The free-space line sits directly under the data-directory chooser on
//! the same first screen, and it carried three more numerus strings. Its counts are
//! whole gigabytes, so a count of one is reachable in all three -- and was rendering
//! "1 GB(s)".
//!
//! FreespaceChecker::ST_OK is 0; the enum is defined inside intro.cpp and is not
//! nameable from here.
void IntroTests::freeSpaceLabelReadsGrammaticallyWithNoCatalogue()
{
    constexpr int ST_OK{0};
    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);
    QLabel* free_space = intro.findChild<QLabel*>(QStringLiteral("freeSpace"));
    QVERIFY(free_space);
    QCheckBox* prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(prune);

    // Keeping the whole chain makes the requirement the two sizes passed in, 154 GB,
    // which is deterministic -- the pruned requirement follows a spin box default.
    prune->setChecked(false);

    // Plenty of room: the requirement is not mentioned at all.
    intro.setStatus(ST_OK, QString(), 500 * GB_BYTES);
    QCOMPARE(free_space->text(), QStringLiteral("500 GB of space available."));

    // Not enough room.
    intro.setStatus(ST_OK, QString(), 10 * GB_BYTES);
    QCOMPARE(free_space->text(), QStringLiteral("10 GB of space available (of 154 GB needed)."));

    // Enough, but within 10 GB of the requirement.
    intro.setStatus(ST_OK, QString(), 160 * GB_BYTES);
    QCOMPARE(free_space->text(),
             QStringLiteral("160 GB of space available (154 GB needed for full chain)."));

    // A single available gigabyte: the count the numerus idiom existed to inflect.
    intro.setStatus(ST_OK, QString(), 1 * GB_BYTES);
    QCOMPARE(free_space->text(), QStringLiteral("1 GB of space available (of 154 GB needed)."));

    // And a single *required* gigabyte, which reaches the singular in the other two
    // strings. A 1 GB chain with no chain state is not a real configuration, but the
    // string it produces is the one a user with a small prune target reads.
    Intro tiny(nullptr, /*blockchain_size_gb=*/1, /*chain_state_size_gb=*/0);
    QLabel* tiny_free_space = tiny.findChild<QLabel*>(QStringLiteral("freeSpace"));
    QVERIFY(tiny_free_space);
    QCheckBox* tiny_prune = tiny.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(tiny_prune);
    tiny_prune->setChecked(false);

    tiny.setStatus(ST_OK, QString(), 0);
    QCOMPARE(tiny_free_space->text(), QStringLiteral("0 GB of space available (of 1 GB needed)."));

    tiny.setStatus(ST_OK, QString(), 5 * GB_BYTES);
    QCOMPARE(tiny_free_space->text(),
             QStringLiteral("5 GB of space available (1 GB needed for full chain)."));

    // Whatever the branch, no unresolved numerus form and no unconsumed placeholder.
    for (const QString& text : {free_space->text(), tiny_free_space->text()}) {
        QVERIFY2(!text.contains(QStringLiteral("(s)")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("%n")), qPrintable(text));
        QVERIFY2(!text.contains(QRegularExpression(QStringLiteral("%[0-9]"))), qPrintable(text));
    }
}

//! F-123. The dialog is the desktop's first-run experience, not a data directory picker,
//! so its trigger has to be "has this desktop ever been set up", not "does some directory
//! exist". The old condition lost the welcome screen, the storage figures, the data
//! directory choice and -- because no prune setting is then written at all -- the pruning
//! default that doc/design/chain-storage.md section 3 requires an operator to be shown,
//! silently leaving them archival. It reproduced on the documented path: getting-started.md
//! leads with quicksilver-daemon, which creates the directory.
void IntroTests::datadirExistenceIsNotTheFirstRunQuestion()
{
    // The defect, stated exactly: a directory is there, this desktop has never been set up.
    QVERIFY(Intro::IsNeeded(/*desktop_configured=*/false, /*datadir_exists=*/true));

    // A desktop that has been through the dialog is not asked again...
    QVERIFY(!Intro::IsNeeded(/*desktop_configured=*/true, /*datadir_exists=*/true));
    // ...unless the directory it was pointed at has gone, which is the case upstream's
    // condition existed to catch and which must keep working.
    QVERIFY(Intro::IsNeeded(/*desktop_configured=*/true, /*datadir_exists=*/false));
    QVERIFY(Intro::IsNeeded(/*desktop_configured=*/false, /*datadir_exists=*/false));
}

//! Chain data is what makes a data directory's storage policy settled. Vault and settings
//! files are not chain data: an operator with no history has nothing to lose by choosing.
void IntroTests::chainDataIsFoundUnderTheNetworkSubdirectoryOnly()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const fs::path root{GUIUtil::QStringToPath(temp.path())};

    // A path that is not there at all, and the empty path a dialog holds before a
    // directory is chosen -- which must not be probed relative to the working directory.
    QVERIFY(!Intro::DataDirHasChainData(root / "absent", ""));
    QVERIFY(!Intro::DataDirHasChainData(fs::path{}, ""));

    // An empty directory is a first run.
    const fs::path empty{root / "empty"};
    QVERIFY(fs::create_directories(empty));
    QVERIFY(!Intro::DataDirHasChainData(empty, ""));

    // The shape a walk subject was found in: a settings file carrying only the generated
    // warning key, and an empty vaults directory. Still a first run.
    const fs::path no_chain{root / "no-chain"};
    QVERIFY(fs::create_directories(no_chain / "vaults"));
    { std::ofstream settings{no_chain / "settings.json"}; settings << "{}"; }
    QVERIFY(!Intro::DataDirHasChainData(no_chain, ""));

    // Either half of an initialised chain settles the question. A node creates both
    // before it has fetched anything, so this fires for the daemon-first operator whose
    // node has run for one minute as well as for one carrying a year of history.
    const fs::path blocks_only{root / "blocks-only"};
    QVERIFY(fs::create_directories(blocks_only / "blocks"));
    QVERIFY(Intro::DataDirHasChainData(blocks_only, ""));

    const fs::path chainstate_only{root / "chainstate-only"};
    QVERIFY(fs::create_directories(chainstate_only / "chainstate"));
    QVERIFY(Intro::DataDirHasChainData(chainstate_only, ""));

    // Networks other than main keep their chain in a subdirectory, and the two must not
    // be confused in either direction: a publictest chain does not settle main's policy,
    // and main's chain does not settle publictest's.
    const fs::path net{root / "net"};
    QVERIFY(fs::create_directories(net / "publictest" / "blocks"));
    QVERIFY(Intro::DataDirHasChainData(net, "publictest"));
    QVERIFY(!Intro::DataDirHasChainData(net, ""));
    QVERIFY(!Intro::DataDirHasChainData(blocks_only, "publictest"));
}

//! The counterpart decision to F-123's trigger: once the dialog is shown to an operator
//! who already has a chain, it must not offer to change that chain's storage. Both
//! defaults are harmful there -- checked prunes history they already paid for, unchecked
//! leaves an already-pruned node unable to start without -reindex -- so no setting is
//! written at all and the dialog says so.
void IntroTests::pruneChoiceIsWithheldFromADataDirThatAlreadyHoldsAChain()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const fs::path root{GUIUtil::QStringToPath(temp.path())};
    const fs::path fresh{root / "fresh"};
    const fs::path with_chain{root / "with-chain"};
    QVERIFY(fs::create_directories(fresh));
    QVERIFY(fs::create_directories(with_chain / "blocks"));

    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);
    QCheckBox* prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    QSpinBox* prune_gb = intro.findChild<QSpinBox*>(QStringLiteral("pruneGB"));
    QLabel* prune_suffix = intro.findChild<QLabel*>(QStringLiteral("lblPruneSuffix"));
    QLabel* existing_chain = intro.findChild<QLabel*>(QStringLiteral("lblExistingChain"));
    QVERIFY(prune);
    QVERIFY(prune_gb);
    QVERIFY(prune_suffix);
    QVERIFY(existing_chain);

    // A directory with no chain in it: the choice is offered, and it is the pruning
    // default chain-storage.md section 3 settled.
    intro.setDataDirectory(GUIUtil::PathToQString(fresh));
    QVERIFY(!prune->isHidden());
    QVERIFY(!prune_gb->isHidden());
    QVERIFY(!prune_suffix->isHidden());
    QVERIFY(existing_chain->isHidden());
    QVERIFY(prune->isChecked());
    QVERIFY(intro.getPruneMiB().has_value());
    QVERIFY(*intro.getPruneMiB() > 0);

    // Point it at a directory that already holds a chain: the control goes away and
    // nothing is returned to write.
    intro.setDataDirectory(GUIUtil::PathToQString(with_chain));
    QVERIFY(prune->isHidden());
    QVERIFY(prune_gb->isHidden());
    QVERIFY(prune_suffix->isHidden());
    QVERIFY(!existing_chain->isHidden());
    QVERIFY(!intro.getPruneMiB().has_value());

    // The disclosure has to name what is happening, and carry no unconsumed placeholder.
    const QString text = existing_chain->text();
    QVERIFY(!text.isEmpty());
    QVERIFY(text.contains(QStringLiteral("already holds a chain")));
    QVERIFY(!text.contains(QRegularExpression(QStringLiteral("%[0-9]"))));

    // And back again, so the withholding tracks the selection rather than latching.
    intro.setDataDirectory(GUIUtil::PathToQString(fresh));
    QVERIFY(!prune->isHidden());
    QVERIFY(existing_chain->isHidden());
    QVERIFY(intro.getPruneMiB().has_value());
}
