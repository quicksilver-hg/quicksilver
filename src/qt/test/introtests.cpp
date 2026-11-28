// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/introtests.h>

#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/intro.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>

#include <fstream>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#ifndef WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

//! The first-run walk found both of these live on the first screen of a clean install.
void IntroTests::explanationLeavesNoPlaceholderAndNoInheritedYear()
{
    Intro intro(nullptr, /*blockchain_size_gb=*/1, /*chain_state_size_gb=*/1);
    QLabel* explanation = intro.findChild<QLabel*>(QStringLiteral("lblExplanation1"));
    QVERIFY(explanation);
    const QString text = explanation->text();

    // Quicksilver has never launched, so no year can be claimed for its first transfers.
    // The inherited string named 2009, a year belonging to the chain this one forked from.
    QVERIFY(!text.contains(QStringLiteral("2009")));

    // Every placeholder must be consumed. QString::arg leaves unmatched markers in place, so
    // an argument list that falls out of step with the .ui string shows the user a raw "%3".
    QVERIFY(!text.contains(QRegularExpression(QStringLiteral("%[0-9]"))));

    // The figure that is claimed has to be the one passed in, presented as an estimate
    // (F-601, owner 2026-11-12).
    QVERIFY(text.contains(QStringLiteral("The block data is estimated at up to about 1 GB a year.")));
}

//! First-run walk: the first screen still described the initial block download of the
//! chain this one forked from.
//!
//! The sentences around the figures were inherited, and they promised a download of
//! history that did not exist on launch day. The replacement has to stay true once the
//! chain has history, which it now does (F-601 dropped "This chain begins at height 0").
void IntroTests::explanationDoesNotPromiseADownloadOfHistoryThatMayNotExist()
{
    Intro intro(nullptr, /*blockchain_size_gb=*/1, /*chain_state_size_gb=*/1);

    QLabel* first_label = intro.findChild<QLabel*>(QStringLiteral("lblExplanation1"));
    QVERIFY(first_label);
    const QString first = first_label->text();
    // The figure is an estimate of what the chain grows by, not the weight of a download
    // waiting to happen, and the launch-day framing is gone.
    QVERIFY(first.contains(QStringLiteral("estimated at up to about 1 GB a year")));
    QVERIFY(!first.contains(QStringLiteral("height 0")));
    QVERIFY(!first.contains(QStringLiteral("first year")));
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

    // The pruning texts below describe the ticked box. Since F-705 the box starts unticked
    // (a new desktop keeps the full chain), so tick it first; the texts are unchanged.
    QCheckBox* intro_prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(intro_prune);
    intro_prune->setChecked(true);
    QLabel* pruning = intro.findChild<QLabel*>(QStringLiteral("lblExplanation3"));
    QVERIFY(pruning);
    const QString pruning_text = pruning->text();
    // F-113 asked that the retained split be spelled out. F-601 (owner, 2026-11-12):
    // at 1 / 1 the default limit (4 GB since the owner's 2026-11-22 ruling, 3 GB under F-703) is larger than a year of block data, so the total
    // is the smaller of the two plus the chain state, and the old "processed volume"
    // sentence is no longer true.
    QCOMPARE(pruning_text, QStringLiteral("With the 4 GB limit, the oldest blocks are deleted after validation once block data passes 4 GB. Expect about 2 GB on disk in all, including an estimated 1 GB of chain state."));
    QVERIFY(!pruning_text.contains(QRegularExpression(QStringLiteral("%[0-9]"))));

    // The storage panel carried the same promise in shorter form.
    QLabel* size_warning = intro.findChild<QLabel*>(QStringLiteral("sizeWarningLabel"));
    QVERIFY(size_warning);
    QVERIFY(!size_warning->text().contains(QStringLiteral("will download and store")));
    QVERIFY(size_warning->text().contains(QStringLiteral("When Consensus is running")));
    QVERIFY(size_warning->text().contains(QStringLiteral("An estimated 2 GB of data will be stored in this directory, and it will grow over time.")));

    // A limit below a year of block data: the limit plus the chain state.
    Intro larger(nullptr, /*blockchain_size_gb=*/5, /*chain_state_size_gb=*/1);
    QCheckBox* larger_prune = larger.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(larger_prune);
    larger_prune->setChecked(true);
    QCOMPARE(larger.findChild<QLabel*>(QStringLiteral("lblExplanation3"))->text(),
             QStringLiteral("With the 4 GB limit, the oldest blocks are deleted after validation once block data passes 4 GB. Expect about 5 GB on disk in all, including an estimated 1 GB of chain state."));
    QVERIFY(larger.findChild<QLabel*>(QStringLiteral("sizeWarningLabel"))->text().contains(QStringLiteral("An estimated 5 GB of data will be stored in this directory.")));
}

void IntroTests::pruneSuffixReadsGrammaticallyWithNoCatalogue()
{
    Intro intro(nullptr, /*blockchain_size_gb=*/1, /*chain_state_size_gb=*/1);
    QLabel* suffix = intro.findChild<QLabel*>(QStringLiteral("lblPruneSuffix"));
    QVERIFY(suffix);
    const QString text = suffix->text();

    // No translator is installed here, and none is installed for a user whose locale has no
    // catalogue. Qt's "%n day(s)" idiom renders the literal "(s)" in that case.
    QVERIFY(!text.contains(QStringLiteral("(s)")));
    QVERIFY(!text.contains(QStringLiteral("%n")));
    QVERIFY(text.contains(QStringLiteral("day")));

    // F-601 B5a (owner, 2026-11-12): the reach is derived from the ruled figure, the
    // limit divided by a year of block data: 4 GB / 1 GB a year = 1460 days (1095 at the
    // 3 GB default of F-703, 730 at the 2 GB default before it; 4 GB since the owner's
    // 2026-11-22 ruling). It used to
    // come from a fixed full-traffic block size and read "5 days".
    QCOMPARE(text, QStringLiteral("(sufficient to restore backups 1460 days old)"));
}

//! First-run walk. The free-space line sits directly under the data-directory chooser on
//! the same first screen, and it carried three more numerus strings. Its counts are
//! whole gigabytes, so a count of one is reachable in all three -- and was rendering
//! "1 GB(s)".
//!
//! FreespaceChecker::ST_OK is 0; the enum is defined inside intro.cpp and is not
//! nameable from here. freeSpaceStatusCodesAreOkThenError pins the value through
//! the dialog's own behaviour.
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

//! S0292-C-03: the checker's status values are private to intro.cpp, so the tests
//! pass them as numbers. Pin what each number does through what the dialog shows:
//! 0 is the OK path (message as given, free space shown, OK enabled), 1 the error
//! path (message marked as an error, free space cleared, OK disabled). A reordered
//! enum fails here rather than silently turning the free-space test into an error
//! case.
void IntroTests::freeSpaceStatusCodesAreOkThenError()
{
    constexpr int ST_OK{0};
    constexpr int ST_ERROR{1};
    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);
    QLabel* free_space = intro.findChild<QLabel*>(QStringLiteral("freeSpace"));
    QLabel* error_message = intro.findChild<QLabel*>(QStringLiteral("errorMessage"));
    QDialogButtonBox* buttons = intro.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));
    QVERIFY(free_space);
    QVERIFY(error_message);
    QVERIFY(buttons);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(ok);
    QCheckBox* prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(prune);
    prune->setChecked(false);

    intro.setStatus(ST_ERROR, QStringLiteral("cannot create"), 500 * GB_BYTES);
    QCOMPARE(error_message->text(), QStringLiteral("Error: cannot create"));
    QCOMPARE(free_space->text(), QString());
    QVERIFY(!ok->isEnabled());

    intro.setStatus(ST_OK, QStringLiteral("will be created"), 500 * GB_BYTES);
    QCOMPARE(error_message->text(), QStringLiteral("will be created"));
    QCOMPARE(free_space->text(), QStringLiteral("500 GB of space available."));
    QVERIFY(ok->isEnabled());
}

//! F-123. The dialog is the desktop's first-run experience, not a data directory picker,
//! so its trigger has to be "has this desktop ever been set up", not "does some directory
//! exist". The old condition lost the welcome screen, the storage figures, the data
//! directory choice and -- because no prune setting is then written at all -- the storage
//! choice that doc/design/chain-storage.md section 3 requires an operator to be shown,
//! silently leaving them archival without having seen what that costs. It reproduced on the documented path: getting-started.md
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
    const auto* buttons = intro.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));
    QVERIFY(buttons);
    const auto* ok = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(ok);

    // A directory with no chain in it: the choice is offered, and it starts at the default
    // chain-storage.md section 3 settles. F-705 (owner, 2026-11-21) reversed that default:
    // a new desktop keeps the full chain, so the box starts unticked and the dialog writes
    // an explicit "no pruning" (0) rather than leaving the setting unwritten.
    intro.setDataDirectory(GUIUtil::PathToQString(fresh));
    QTRY_VERIFY(ok->isEnabled());
    QVERIFY(!prune->isHidden());
    QVERIFY(!prune_gb->isHidden());
    QVERIFY(!prune_suffix->isHidden());
    QVERIFY(existing_chain->isHidden());
    QVERIFY(!prune->isChecked());
    QVERIFY(intro.getPruneMiB().has_value());
    QCOMPARE(*intro.getPruneMiB(), int64_t{0});
    // Pruning stays one tick away, at the default target: 4 GB = 4e9 bytes = 3814 MiB (owner,
    // 2026-11-22; it was 3 GB = 2861 MiB under F-703, below the 2,956 MiB floor).
    prune->setChecked(true);
    QCOMPARE(*intro.getPruneMiB(), int64_t{3814});
    prune->setChecked(false);

    // Point it at a directory that already holds a chain: the control goes away and
    // nothing is returned to write.
    intro.setDataDirectory(GUIUtil::PathToQString(with_chain));
    QTRY_VERIFY(ok->isEnabled());
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
    QTRY_VERIFY(ok->isEnabled());
    QVERIFY(!prune->isHidden());
    QVERIFY(existing_chain->isHidden());
    QVERIFY(intro.getPruneMiB().has_value());
}

void IntroTests::pruneChoiceWaitsForTheLatestDirectoryCheck()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const fs::path root{GUIUtil::QStringToPath(temp.path())};
    QVERIFY(fs::create_directories(root / "existing" / "blocks"));

    Intro intro(nullptr, 128, 26);
    const auto* prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    const auto* size = intro.findChild<QSpinBox*>(QStringLiteral("pruneGB"));
    const auto* buttons = intro.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));
    QVERIFY(prune && size && buttons);
    const auto* ok = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(ok);

    intro.setDataDirectory(GUIUtil::PathToQString(root / "existing"));
    intro.setDataDirectory(GUIUtil::PathToQString(root / "fresh"));
    QVERIFY(prune->isHidden());
    QVERIFY(size->isHidden());
    QVERIFY(!ok->isEnabled());
    QTRY_VERIFY(ok->isEnabled());
    QVERIFY(!prune->isHidden());
    QVERIFY(!size->isHidden());
}

void IntroTests::vaultFolderFailureDoesNotRejectNewDataDirectory()
{
#ifdef WIN32
    QSKIP("Requires POSIX directory permissions.");
#else
    if (geteuid() == 0) QSKIP("Requires a user subject to directory permissions.");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const fs::path fresh = GUIUtil::QStringToPath(temp.path()) / "fresh";

    QSettings settings;
    const QVariant saved = settings.value("strDataDir");
    const QVariant saved_reset = settings.value("fReset");
    settings.setValue("strDataDir", GUIUtil::PathToQString(fresh));
    settings.sync();
    const auto saved_datadir = gArgs.GetArg("-datadir");
    gArgs.ForceSetArg("-datadir", "");
    const ChainType saved_chain = Params().GetChainType();

    bool saw_error{false};
    bool timed_out{false};
    std::optional<mode_t> saved_umask;
    QTimer driver;
    QElapsedTimer deadline;
    deadline.start();
    connect(&driver, &QTimer::timeout, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (auto* error = qobject_cast<QMessageBox*>(modal)) {
            saw_error = true;
            error->accept();
        } else if (auto* intro = qobject_cast<Intro*>(modal)) {
            timed_out = deadline.hasExpired(5000);
            auto* buttons = intro->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));
            auto* ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
            if (saw_error || timed_out) {
                intro->reject();
            } else if (ok && ok->isEnabled() && !saved_umask) {
                // Creating fresh succeeds in the writable temporary parent. Its
                // permissions prohibit creating fresh/vaults on the next line.
                saved_umask = umask(0222);
                ok->click();
            }
        }
    });
    driver.start(10);
    bool shown{false};
    std::optional<int64_t> prune;
    const bool accepted = Intro::showIfNeeded(shown, prune);
    driver.stop();
    if (saved_umask) umask(*saved_umask);
    SelectParams(saved_chain);
    if (saved_datadir) gArgs.ForceSetArg("-datadir", *saved_datadir);
    else gArgs.LockSettings([](common::Settings& s) { s.forced_settings.erase("datadir"); });
    if (saved.isValid()) settings.setValue("strDataDir", saved);
    else settings.remove("strDataDir");
    if (saved_reset.isValid()) settings.setValue("fReset", saved_reset);
    else settings.remove("fReset");

    const bool created = fs::is_directory(fresh);
    const bool vaults_created = fs::is_directory(fresh / "vaults");
    if (created) fs::permissions(fresh, fs::perms::owner_write, fs::perm_options::add);
    QVERIFY(shown);
    QVERIFY(!timed_out);
    QVERIFY(saved_umask.has_value());
    QVERIFY(created);
    QVERIFY(!vaults_created);
    QVERIFY(!saw_error);
    QVERIFY(accepted);
#endif
}

//! S0225-C-01: the caution line said "needed for full chain" whatever the box said. With
//! pruning on, the requirement is the prune target plus the chain state, not the chain.
void IntroTests::freeSpaceCautionNamesTheFullChainOnlyWhenThatIsTheRequirement()
{
    constexpr int ST_OK{0};
    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);
    QLabel* free_space = intro.findChild<QLabel*>(QStringLiteral("freeSpace"));
    QCheckBox* prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(free_space);
    QVERIFY(prune);

    // Pruned at the 4 GB default target: 4 + 26 = 30 GB, within 10 GB of 35.
    prune->setChecked(true);
    intro.setStatus(ST_OK, QString(), 35 * GB_BYTES);
    QCOMPARE(free_space->text(), QStringLiteral("35 GB of space available (30 GB needed)."));
    intro.setStatus(ST_OK, QString(), 10 * GB_BYTES);
    QCOMPARE(free_space->text(), QStringLiteral("10 GB of space available (of 30 GB needed)."));

    // Unticking makes it the full chain again, and the line says so.
    prune->setChecked(false);
    intro.setStatus(ST_OK, QString(), 160 * GB_BYTES);
    QCOMPARE(free_space->text(), QStringLiteral("160 GB of space available (154 GB needed for full chain)."));

    // A pruned target cannot go below the block-file floor (MIN_DISK_SPACE_FOR_BLOCK_FILES,
    // 2,956 MiB, shown as 4 GB): 1 is raised to it, and the line counts the raised target.
    Intro tiny(nullptr, /*blockchain_size_gb=*/5, /*chain_state_size_gb=*/0);
    QCheckBox* tiny_prune = tiny.findChild<QCheckBox*>(QStringLiteral("prune"));
    QSpinBox* tiny_target = tiny.findChild<QSpinBox*>(QStringLiteral("pruneGB"));
    QVERIFY(tiny_prune);
    QVERIFY(tiny_target);
    tiny_prune->setChecked(true);
    tiny_target->setValue(1);
    QCOMPARE(tiny_target->value(), 4);
    tiny.setStatus(ST_OK, QString(), 5 * GB_BYTES);
    QCOMPARE(tiny.findChild<QLabel*>(QStringLiteral("freeSpace"))->text(),
             QStringLiteral("5 GB of space available (4 GB needed)."));

    // The singular, now reachable only unpruned: a 1 GB full chain and no chain state.
    Intro one(nullptr, /*blockchain_size_gb=*/1, /*chain_state_size_gb=*/0);
    QCheckBox* one_prune = one.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(one_prune);
    one_prune->setChecked(false);
    one.setStatus(ST_OK, QString(), 5 * GB_BYTES);
    QCOMPARE(one.findChild<QLabel*>(QStringLiteral("freeSpace"))->text(),
             QStringLiteral("5 GB of space available (1 GB needed for full chain)."));
}

//! S0225-C-03 and S0225-COORD-01: the throwing fs::exists / fs::is_directory raise for a
//! path under a directory the user may not search. The free-space check ran one outside
//! its try, on a worker thread with no handler, and the chain probe ran them on the UI
//! thread for every keystroke. Such a path is now an error the dialog states.
void IntroTests::pathUnderAnUnsearchableDirectoryIsReportedNotThrown()
{
#ifdef WIN32
    QSKIP("Needs a directory without search permission, which fs::permissions cannot make on Windows (NTFS ACLs).");
#else
    if (geteuid() == 0) QSKIP("Root may search any directory, so the permission error cannot be produced.");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const fs::path locked{GUIUtil::QStringToPath(temp.path()) / "locked"};
    QVERIFY(fs::create_directories(locked));
    fs::permissions(locked, fs::perms::none);
    // Restore the permission however the test ends, or QTemporaryDir cannot clean up.
    struct Unlock {
        fs::path path;
        ~Unlock() { std::error_code ec; fs::permissions(path, fs::perms::owner_all, ec); }
    } unlock{locked};
    const fs::path datadir{locked / "datadir"};
    // Precondition: this really is the case the throwing overload raises on.
    std::error_code probe_ec;
    (void)fs::status(datadir, probe_ec);
    QVERIFY2(probe_ec, "expected permission denied on a path under a mode-000 directory");

    QVERIFY(!Intro::DataDirHasChainData(datadir, ""));
    QVERIFY(!Intro::DataDirHasChainData(locked, "publictest"));

    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);
    QLabel* error_message = intro.findChild<QLabel*>(QStringLiteral("errorMessage"));
    QDialogButtonBox* buttons = intro.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));
    QVERIFY(error_message);
    QVERIFY(buttons);
    intro.setDataDirectory(GUIUtil::PathToQString(datadir));
    QTRY_COMPARE(error_message->text(), QStringLiteral("Error: Cannot create data directory here."));
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    // The choice stays offered: nothing visible says a chain is there.
    QVERIFY(!intro.findChild<QCheckBox*>(QStringLiteral("prune"))->isHidden());
#endif
}

//! S0225-C-04: the dialog runs before InitConfig reads the config file, so a chain= set
//! only there was invisible and the existing-chain probe looked in main's directory.
void IntroTests::chainSetOnlyInTheConfigFileSelectsTheProbedNetwork()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const fs::path datadir{GUIUtil::QStringToPath(temp.path())};
    const auto write_conf = [&](const char* text) {
        std::ofstream conf{datadir / "quicksilver.conf", std::ios::trunc};
        conf << text;
    };
    const auto fresh_args = [] {
        auto args{std::make_unique<ArgsManager>()};
        SetupChainParamsBaseOptions(*args);
        return args;
    };

    // No config file: main.
    QCOMPARE(Intro::ChainTypeForDataDir(*fresh_args(), datadir), ChainType::MAIN);

    // chain= in the file, among keys the probe does not know (which must not stop it).
    write_conf("rpcuser=someone\nchain=publictest # the test network\nlisten=1\n");
    QCOMPARE(Intro::ChainTypeForDataDir(*fresh_args(), datadir), ChainType::PUBLIC_TEST);

    // The shorthand form.
    write_conf("publictest=1\n");
    QCOMPARE(Intro::ChainTypeForDataDir(*fresh_args(), datadir), ChainType::PUBLIC_TEST);

    // A key inside a network section does not choose the network.
    write_conf("[publictest]\nchain=publictest\n");
    QCOMPARE(Intro::ChainTypeForDataDir(*fresh_args(), datadir), ChainType::MAIN);

    // The command line wins over the file, as it does in InitConfig.
    write_conf("chain=publictest\n");
    {
        const auto args{fresh_args()};
        args->ForceSetArg("-chain", "sandbox");
        QCOMPARE(Intro::ChainTypeForDataDir(*args, datadir), ChainType::SANDBOX);
    }

    // A conflicting combination is InitConfig's to report: the probe uses the command line.
    {
        const auto args{fresh_args()};
        args->ForceSetArg("-sandbox", "1");
        QCOMPARE(Intro::ChainTypeForDataDir(*args, datadir), ChainType::SANDBOX);
    }

    // -conf names another file, relative to the data directory.
    {
        std::ofstream other{datadir / "other.conf"};
        other << "chain=sandbox\n";
    }
    {
        const auto args{fresh_args()};
        args->ForceSetArg("-conf", "other.conf");
        QCOMPARE(Intro::ChainTypeForDataDir(*args, datadir), ChainType::SANDBOX);
    }
}

//! S0225-C-06: -prune given to this run locks the box, so no choice is made in the
//! dialog. The value it returned was forced over the argument and written to settings,
//! so -prune=1 (manual pruning) came back as the automatic default target, this run and
//! every later one.
void IntroTests::pruneArgumentLocksTheChoiceAndNothingIsWritten()
{
    gArgs.ForceSetArg("-prune", "1");
    struct Unforce {
        ~Unforce() { gArgs.LockSettings([](common::Settings& s) { s.forced_settings.erase("prune"); }); }
    } unforce;

    Intro intro(nullptr, /*blockchain_size_gb=*/128, /*chain_state_size_gb=*/26);
    QCheckBox* prune = intro.findChild<QCheckBox*>(QStringLiteral("prune"));
    QVERIFY(prune);
    QVERIFY(prune->isChecked());
    QVERIFY(!prune->isEnabled());
    QVERIFY(!intro.getPruneMiB().has_value());
}
