// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_INTRO_H
#define QUICKSILVER_QT_INTRO_H

#include <util/fs.h>

#include <QDialog>
#include <QMutex>
#include <QThread>

#include <optional>
#include <string>

static const bool DEFAULT_CHOOSE_DATADIR = false;

class FreespaceChecker;

namespace interfaces {
    class Node;
}

namespace Ui {
    class Intro;
}

/** Introduction screen (pre-GUI startup).
  Allows the user to choose a data directory,
  in which the vault and block chain will be stored.
 */
class Intro : public QDialog
{
    Q_OBJECT

public:
    explicit Intro(QWidget *parent = nullptr,
                   int64_t blockchain_size_gb = 0, int64_t chain_state_size_gb = 0,
                   std::string net_subdir = "");
    ~Intro();

    QString getDataDirectory();
    void setDataDirectory(const QString &dataDir);

    /**
     * The pruning choice the user made, or std::nullopt when they were not offered one
     * because the selected data directory already holds a chain. Nullopt means "leave
     * this node's storage policy alone": writing either value over an existing chain is
     * harmful, since pruning deletes history the operator already paid for and
     * unpruning leaves the node unable to start without -reindex.
     */
    std::optional<int64_t> getPruneMiB() const;

    /**
     * Whether the first-run dialog is needed.
     *
     * The dialog is the desktop's first-run experience, not merely a data directory
     * picker: it discloses what a year of chain storage costs and takes the pruning
     * choice that doc/design/chain-storage.md section 3 requires an operator to make
     * rather than inherit. So the question is whether this desktop has ever been
     * through it, not whether a directory happens to exist -- an empty directory, or
     * one quicksilver-daemon created, answered the old question and skipped every screen
     * (F-123). doc/getting-started.md leads with quicksilver-daemon, so that was the
     * documented path.
     *
     * @param desktop_configured  this desktop has completed the dialog before
     * @param datadir_exists      the candidate data directory is present on disk
     */
    static bool IsNeeded(bool desktop_configured, bool datadir_exists);

    /**
     * Whether <datadir> already holds chain data for the network kept in <net_subdir>
     * ("" for main, which lives in the data directory itself). A directory a node has
     * initialised in has a settled storage policy; the dialog discloses it but must not
     * change it. State that is not a chain -- a vaults directory, a settings file --
     * does not count: an operator with no history has nothing to lose and everything to
     * choose.
     */
    static bool DataDirHasChainData(const fs::path& datadir, const std::string& net_subdir);

    /**
     * Determine data directory. Let the user choose if the current one doesn't exist.
     * Let the user configure additional preferences such as pruning.
     *
     * @returns true if a data directory was selected, false if the user cancelled the selection
     * dialog.
     *
     * @note do NOT call global gArgs.GetDataDirNet() before calling this function, this
     * will cause the wrong path to be cached.
     */
    static bool showIfNeeded(bool& did_show_intro, std::optional<int64_t>& prune_MiB);

Q_SIGNALS:
    void requestCheck();

public Q_SLOTS:
    void setStatus(int status, const QString &message, quint64 bytesAvailable);

private Q_SLOTS:
    void on_dataDirectory_textChanged(const QString &arg1);
    void on_ellipsisButton_clicked();
    void on_dataDirDefault_clicked();
    void on_dataDirCustom_clicked();

private:
    Ui::Intro *ui;
    QThread* thread{nullptr};
    QMutex mutex;
    bool signalled{false};
    QString pathToCheck;
    const int64_t m_blockchain_size_gb;
    const int64_t m_chain_state_size_gb;
    //! Where the selected network keeps its chain, relative to the data directory.
    const std::string m_net_subdir;
    //! Set from the selected data directory; withholds the pruning choice when true.
    bool m_datadir_has_chain{false};
    //! Total required space (in GB) depending on user choice (prune or not prune).
    int64_t m_required_space_gb{0};
    uint64_t m_bytes_available{0};
    int64_t m_prune_target_gb;

    void startThread();
    void checkPath(const QString &dataDir);
    QString getPathToCheck();
    void UpdatePruneLabels(bool prune_checked);
    void UpdatePruneVisibility(const QString& data_dir);
    void UpdateFreeSpaceLabel();

    friend class FreespaceChecker;
};

#endif // QUICKSILVER_QT_INTRO_H
