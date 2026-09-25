// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <chainparamsbase.h>
#include <qt/intro.h>
#include <qt/forms/ui_intro.h>
#include <util/chaintype.h>
#include <util/fs.h>

#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>

#include <common/args.h>
#include <interfaces/node.h>
#include <util/fs_helpers.h>
#include <validation.h>

#include <QFileDialog>
#include <QSettings>
#include <QMessageBox>

#include <cmath>

/* Check free space asynchronously to prevent hanging the UI thread.

   Up to one request to check a path is in flight to this thread; when the check()
   function runs, the current path is requested from the associated Intro object.
   The reply is sent back through a signal.

   This ensures that no queue of checking requests is built up while the user is
   still entering the path, and that always the most recently entered path is checked as
   soon as the thread becomes available.
*/
class FreespaceChecker : public QObject
{
    Q_OBJECT

public:
    explicit FreespaceChecker(Intro *intro);

    enum Status {
        ST_OK,
        ST_ERROR
    };

public Q_SLOTS:
    void check();

Q_SIGNALS:
    void reply(int status, const QString &message, quint64 available);

private:
    Intro *intro;
};

#include <qt/intro.moc>

FreespaceChecker::FreespaceChecker(Intro *_intro)
{
    this->intro = _intro;
}

void FreespaceChecker::check()
{
    QString dataDirStr = intro->getPathToCheck();
    fs::path dataDir = GUIUtil::QStringToPath(dataDirStr);
    uint64_t freeBytesAvailable = 0;
    int replyStatus = ST_OK;
    QString replyMessage = tr("A new data directory will be created.");

    /* Find first parent that exists, so that fs::space does not fail */
    fs::path parentDir = dataDir;
    fs::path parentDirOld = fs::path();
    while(parentDir.has_parent_path() && !fs::exists(parentDir))
    {
        parentDir = parentDir.parent_path();

        /* Check if we make any progress, break if not to prevent an infinite loop here */
        if (parentDirOld == parentDir)
            break;

        parentDirOld = parentDir;
    }

    try {
        freeBytesAvailable = fs::space(parentDir).available;
        if(fs::exists(dataDir))
        {
            if(fs::is_directory(dataDir))
            {
                QString separator = "<code>" + QDir::toNativeSeparators("/") + tr("name") + "</code>";
                replyStatus = ST_OK;
                replyMessage = tr("Directory already exists. Add %1 if you intend to create a new directory here.").arg(separator);
            } else {
                replyStatus = ST_ERROR;
                replyMessage = tr("Path already exists, and is not a directory.");
            }
        }
    } catch (const fs::filesystem_error&)
    {
        /* Parent directory does not exist or is not accessible */
        replyStatus = ST_ERROR;
        replyMessage = tr("Cannot create data directory here.");
    }
    Q_EMIT reply(replyStatus, replyMessage, freeBytesAvailable);
}

namespace {
//! Return pruning size that will be used if automatic pruning is enabled.
int GetPruneTargetGB()
{
    int64_t prune_target_mib = gArgs.GetIntArg("-prune", 0);
    // >1 means automatic pruning is enabled by config, 1 means manual pruning, 0 means no pruning.
    return prune_target_mib > 1 ? PruneMiBtoGB(prune_target_mib) : DEFAULT_PRUNE_TARGET_GB;
}
} // namespace

Intro::Intro(QWidget *parent, int64_t blockchain_size_gb, int64_t chain_state_size_gb,
             std::string net_subdir) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::Intro),
    m_blockchain_size_gb(blockchain_size_gb),
    m_chain_state_size_gb(chain_state_size_gb),
    m_net_subdir(std::move(net_subdir)),
    m_prune_target_gb{GetPruneTargetGB()}
{
    ui->setupUi(this);
    ui->welcomeLabel->setText(ui->welcomeLabel->text().arg(CLIENT_NAME));
    ui->storageLabel->setText(ui->storageLabel->text().arg(CLIENT_NAME));

    ui->lblExplanation1->setText(ui->lblExplanation1->text()
        .arg(CLIENT_NAME)
        .arg(m_blockchain_size_gb)
    );
    ui->lblExplanation2->setText(ui->lblExplanation2->text().arg(CLIENT_NAME));

    const int min_prune_target_GB = std::ceil(MIN_DISK_SPACE_FOR_BLOCK_FILES / 1e9);
    ui->pruneGB->setRange(min_prune_target_GB, std::numeric_limits<int>::max());
    // Quicksilver prunes by default; keeping the whole chain is the deliberate opt-in.
    // Upstream defaults this off and only turns it on for a disk that looks too small,
    // which suits a chain that grows ~60 GB/year. This one grows about 128 GB/year of
    // block data, so an unprompted archival default would quietly hand every desktop
    // operator a cost they did not choose. See doc/design/chain-storage.md.
    ui->prune->setChecked(true);
    if (gArgs.IsArgSet("-prune")) {
        ui->prune->setChecked(gArgs.GetIntArg("-prune", 0) >= 1);
        ui->prune->setEnabled(false);
    }
    ui->pruneGB->setValue(m_prune_target_gb);
    ui->pruneGB->setToolTip(ui->prune->toolTip());
    ui->lblPruneSuffix->setToolTip(ui->prune->toolTip());
    UpdatePruneVisibility(ui->dataDirectory->text());

    connect(ui->prune, &QCheckBox::toggled, [this](bool prune_checked) {
        UpdatePruneLabels(prune_checked);
        UpdateFreeSpaceLabel();
    });
    connect(ui->pruneGB, qOverload<int>(&QSpinBox::valueChanged), [this](int prune_GB) {
        m_prune_target_gb = prune_GB;
        UpdatePruneLabels(ui->prune->isChecked());
        UpdateFreeSpaceLabel();
    });

    startThread();
}

Intro::~Intro()
{
    delete ui;
    /* Ensure thread is finished before it is deleted */
    thread->quit();
    thread->wait();
}

QString Intro::getDataDirectory()
{
    return ui->dataDirectory->text();
}

void Intro::setDataDirectory(const QString &dataDir)
{
    ui->dataDirectory->setText(dataDir);
    if(dataDir == GUIUtil::getDefaultDataDirectory())
    {
        ui->dataDirDefault->setChecked(true);
        ui->dataDirectory->setEnabled(false);
        ui->ellipsisButton->setEnabled(false);
    } else {
        ui->dataDirCustom->setChecked(true);
        ui->dataDirectory->setEnabled(true);
        ui->ellipsisButton->setEnabled(true);
    }
}

std::optional<int64_t> Intro::getPruneMiB() const
{
    if (m_datadir_has_chain) return std::nullopt;
    switch (ui->prune->checkState()) {
    case Qt::Checked:
        return PruneGBtoMiB(m_prune_target_gb);
    case Qt::Unchecked: default:
        return 0;
    }
}

bool Intro::IsNeeded(bool desktop_configured, bool datadir_exists)
{
    return !desktop_configured || !datadir_exists;
}

bool Intro::DataDirHasChainData(const fs::path& datadir, const std::string& net_subdir)
{
    if (datadir.empty()) return false;  // Nothing is selected yet; a relative probe would read the cwd.
    const fs::path net_dir{net_subdir.empty() ? datadir : datadir / fs::PathFromString(net_subdir)};
    return fs::is_directory(net_dir / "blocks") || fs::is_directory(net_dir / "chainstate");
}

bool Intro::showIfNeeded(bool& did_show_intro, std::optional<int64_t>& prune_MiB)
{
    did_show_intro = false;

    QSettings settings;
    /* strDataDir is written only once this dialog has been completed, so its absence is
       what "this desktop has never been set up" means. */
    const bool desktop_configured = settings.contains("strDataDir");
    /* If data directory provided on command line, no need to look at settings
       or show a picking dialog */
    if(!gArgs.GetArg("-datadir", "").empty())
        return true;
    /* 1) Default data directory for operating system */
    QString dataDir = GUIUtil::getDefaultDataDirectory();
    /* 2) Allow QSettings to override default dir */
    dataDir = settings.value("strDataDir", dataDir).toString();

    if(IsNeeded(desktop_configured, fs::exists(GUIUtil::QStringToPath(dataDir))) || gArgs.GetBoolArg("-choosedatadir", DEFAULT_CHOOSE_DATADIR) || settings.value("fReset", false).toBool() || gArgs.GetBoolArg("-resetguisettings", false))
    {
        /* Use selectParams here to guarantee Params() can be used by node interface */
        try {
            SelectParams(gArgs.GetChainType());
        } catch (const std::exception&) {
            return false;
        }

        /* If current default data directory does not exist, let the user choose one */
        Intro intro(nullptr, Params().AssumedBlockchainSize(), Params().AssumedChainStateSize(), BaseParams().DataDir());
        intro.setDataDirectory(dataDir);
        intro.setWindowIcon(QIcon(":icons/quicksilver"));
        did_show_intro = true;

        while(true)
        {
            if(!intro.exec())
            {
                /* Cancel clicked */
                return false;
            }
            dataDir = intro.getDataDirectory();
            try {
                if (TryCreateDirectories(GUIUtil::QStringToPath(dataDir))) {
                    // If a new data directory has been created, make vaults subdirectory too
                    TryCreateDirectories(GUIUtil::QStringToPath(dataDir) / "vaults");
                }
                break;
            } catch (const fs::filesystem_error&) {
                QMessageBox::critical(nullptr, CLIENT_NAME,
                    tr("Error: Specified data directory \"%1\" cannot be created.").arg(dataDir));
                /* fall through, back to choosing screen */
            }
        }

        // Additional preferences:
        prune_MiB = intro.getPruneMiB();

        settings.setValue("strDataDir", dataDir);
        settings.setValue("fReset", false);
    }
    /* Only override -datadir if different from the default, to make it possible to
     * override -datadir in the quicksilver.conf file in the default data directory
     * (to be consistent with quicksilver-daemon behavior)
     */
    if(dataDir != GUIUtil::getDefaultDataDirectory()) {
        gArgs.SoftSetArg("-datadir", fs::PathToString(GUIUtil::QStringToPath(dataDir))); // use OS locale for path setting
    }
    return true;
}

void Intro::setStatus(int status, const QString &message, quint64 bytesAvailable)
{
    switch(status)
    {
    case FreespaceChecker::ST_OK:
        ui->errorMessage->setText(message);
        ui->errorMessage->setStyleSheet("");
        break;
    case FreespaceChecker::ST_ERROR:
        ui->errorMessage->setText(tr("Error") + ": " + message);
        ui->errorMessage->setStyleSheet("QLabel { color: #800000 }");
        break;
    }
    /* Indicate number of bytes available */
    if(status == FreespaceChecker::ST_ERROR)
    {
        ui->freeSpace->setText("");
    } else {
        m_bytes_available = bytesAvailable;
        // Upstream re-derives the checkbox here from free space, which would UNCHECK it on
        // any roomy disk and so undo Quicksilver's pruning default. The default does not
        // depend on how much room this particular machine happens to have: keeping the whole
        // chain is a choice to serve history to other people, not a consequence of disk size.
        UpdateFreeSpaceLabel();
    }
    /* Don't allow confirm in ERROR state */
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(status != FreespaceChecker::ST_ERROR);
}

void Intro::UpdateFreeSpaceLabel()
{
    // Singular and plural as separate strings rather than the "%n GB" idiom, for the
    // reason set out in qt/maturity.cpp: with no app catalogue installed Qt leaves the
    // count unresolved, and this is the very first screen of a clean install.
    const qulonglong gb_available = m_bytes_available / GB_BYTES;
    const qlonglong gb_needed = m_required_space_gb;
    QString freeString = gb_available == 1
                             ? tr("1 GB of space available")
                             : tr("%1 GB of space available").arg(gb_available);
    if (m_bytes_available < m_required_space_gb * GB_BYTES) {
        const QString needed = gb_needed == 1 ? tr("(of 1 GB needed)")
                                              : tr("(of %1 GB needed)").arg(gb_needed);
        freeString += " " + needed;
        ui->freeSpace->setStyleSheet("QLabel { color: #800000 }");
    } else if (m_bytes_available / GB_BYTES - m_required_space_gb < 10) {
        const QString needed = gb_needed == 1 ? tr("(1 GB needed for full chain)")
                                              : tr("(%1 GB needed for full chain)").arg(gb_needed);
        freeString += " " + needed;
        ui->freeSpace->setStyleSheet("QLabel { color: #999900 }");
    } else {
        ui->freeSpace->setStyleSheet("");
    }
    ui->freeSpace->setText(freeString + ".");
}

void Intro::on_dataDirectory_textChanged(const QString &dataDirStr)
{
    /* Disable OK button until check result comes in */
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    UpdatePruneVisibility(dataDirStr);
    checkPath(dataDirStr);
}

void Intro::UpdatePruneVisibility(const QString& data_dir)
{
    m_datadir_has_chain = DataDirHasChainData(GUIUtil::QStringToPath(data_dir), m_net_subdir);
    const bool offer_prune_choice = !m_datadir_has_chain;
    ui->prune->setVisible(offer_prune_choice);
    ui->pruneGB->setVisible(offer_prune_choice);
    ui->lblPruneSuffix->setVisible(offer_prune_choice);
    ui->lblExistingChain->setVisible(m_datadir_has_chain);
    /* With no choice on offer the honest figure is the archival one: this node keeps
       whatever it already keeps, and the dialog is not about to change it. */
    UpdatePruneLabels(offer_prune_choice && ui->prune->isChecked());
    UpdateFreeSpaceLabel();
}

void Intro::on_ellipsisButton_clicked()
{
    QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(nullptr, tr("Choose data directory"), ui->dataDirectory->text()));
    if(!dir.isEmpty())
        ui->dataDirectory->setText(dir);
}

void Intro::on_dataDirDefault_clicked()
{
    setDataDirectory(GUIUtil::getDefaultDataDirectory());
}

void Intro::on_dataDirCustom_clicked()
{
    ui->dataDirectory->setEnabled(true);
    ui->ellipsisButton->setEnabled(true);
}

void Intro::startThread()
{
    thread = new QThread(this);
    FreespaceChecker *executor = new FreespaceChecker(this);
    executor->moveToThread(thread);

    connect(executor, &FreespaceChecker::reply, this, &Intro::setStatus);
    connect(this, &Intro::requestCheck, executor, &FreespaceChecker::check);
    /*  make sure executor object is deleted in its own thread */
    connect(thread, &QThread::finished, executor, &QObject::deleteLater);

    thread->start();
}

void Intro::checkPath(const QString &dataDir)
{
    mutex.lock();
    pathToCheck = dataDir;
    if(!signalled)
    {
        signalled = true;
        Q_EMIT requestCheck();
    }
    mutex.unlock();
}

QString Intro::getPathToCheck()
{
    QString retval;
    mutex.lock();
    retval = pathToCheck;
    signalled = false; /* new request can be queued now */
    mutex.unlock();
    return retval;
}

void Intro::UpdatePruneLabels(bool prune_checked)
{
    m_required_space_gb = m_blockchain_size_gb + m_chain_state_size_gb;
    QString storageRequiresMsg = tr("At least %1 GB of data will be stored in this directory, and it will grow over time.");
    if (prune_checked && m_prune_target_gb <= m_blockchain_size_gb) {
        m_required_space_gb = m_prune_target_gb + m_chain_state_size_gb;
        storageRequiresMsg = tr("Approximately %1 GB of data will be stored in this directory.");
    }
    ui->lblExplanation3->setVisible(prune_checked);
    ui->lblExplanation3->setText(tr("With the %1 GB recent-block limit, older blocks are deleted after validation. After the first year, expect about %2 GB on disk: %1 GB of recent blocks plus about %3 GB of chain state. The %4 GB first-year block-data figure above is processed volume, not retained storage.")
        .arg(m_prune_target_gb)
        .arg(m_required_space_gb)
        .arg(m_chain_state_size_gb)
        .arg(m_blockchain_size_gb));
    ui->pruneGB->setEnabled(prune_checked);
    static constexpr uint64_t nPowTargetSpacing = 5 * 60;  // from chainparams, which we don't have at this stage
    static constexpr uint32_t expected_block_data_size = 1217399;  // includes undo data; M5 honest shape
    const uint64_t expected_backup_days = m_prune_target_gb * 1e9 / (uint64_t(expected_block_data_size) * 86400 / nPowTargetSpacing);
    // Singular and plural as separate strings rather than the "%n day(s)" idiom, for the
    // reason set out in qt/maturity.cpp: with no catalogue installed Qt leaves the literal
    // "(s)", so a first-run user reads "backups 30 day(s) old".
    ui->lblPruneSuffix->setText(
        expected_backup_days == 1
            //: Explanatory text on the capability of the current prune target.
            ? tr("(sufficient to restore backups 1 day old)")
            //: Explanatory text on the capability of the current prune target.
            : tr("(sufficient to restore backups %1 days old)").arg(expected_backup_days));
    ui->sizeWarningLabel->setText(
        tr("When Consensus is running, %1 downloads and stores a copy of the Quicksilver block chain here.").arg(CLIENT_NAME) + " " +
        storageRequiresMsg.arg(m_required_space_gb) + " " +
        tr("The vault will also be stored in this directory.")
    );
    this->adjustSize();
}
