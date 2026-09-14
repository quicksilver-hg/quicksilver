// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_SENDCOINSDIALOG_H
#define QUICKSILVER_QT_SENDCOINSDIALOG_H

#include <gpu/detection_service.h>
#include <qt/clientmodel.h>
#include <qt/vaultmodel.h>
#include <vault/coincontrol.h>

#include <QDialog>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <memory>

class PlatformStyle;
class SendCoinsEntry;
class SendCoinsRecipient;
enum class SynchronizationState;

namespace Ui {
    class SendCoinsDialog;
}

QT_BEGIN_NAMESPACE
class QUrl;
class QShowEvent;
QT_END_NAMESPACE

/** Dialog for Quicksilver transfers. */
class SendCoinsDialog : public QDialog
{
    Q_OBJECT

public:
    enum class GpuSolverProbeStatus {
        Unchecked,
        Checking,
        Available,
        Failed,
        TimedOut,
    };

    explicit SendCoinsDialog(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~SendCoinsDialog();

    void setClientModel(ClientModel *clientModel);
    void setModel(VaultModel *model);

    /** Set up the tab chain manually, as Qt messes up the tab chain by default in some cases (issue https://bugreports.qt-project.org/browse/QTBUG-10907).
     */
    QWidget *setupTabChain(QWidget *prev);

    void setAddress(const QString &address);
    void pasteEntry(const SendCoinsRecipient &rv);
    bool handlePaymentRequest(const SendCoinsRecipient &recipient);

    // Only used for testing-purposes
    vault::CCoinControl* getCoinControl() { return m_coin_control.get(); }

    // Generation token for an async PoW solve. Bumped on every send/cancel; a worker
    // result is only honoured while its captured generation still matches.
    //
    // The generation alone only discards the answer. It used to be the whole of
    // "cancel", so the interface reported the work stopped while the worker ground on
    // for minutes, holding the vault and the GPU. bumpSolveGeneration now also asks
    // the grind itself to stop, which is why it is no longer inline.
    quint64 currentSolveGeneration() const { return m_solve_generation; }
    void bumpSolveGeneration();
    bool acceptSolveResult(quint64 gen) const { return gen == m_solve_generation; }
    static QString sendWorkResourceTextForTesting(bool requires_configured_gpu_solver, const QString& solver_path, GpuSolverProbeStatus probe_status = GpuSolverProbeStatus::Unchecked);
    static QStringList gpuSolverProbeArguments(uint8_t edgebits);
    static bool cpuFallbackWarningRequiredForTesting(bool slow_network, GpuSolverProbeStatus probe_status, bool warning_enabled);

public Q_SLOTS:
    void clear();
    void reject() override;
    void accept() override;
    SendCoinsEntry *addEntry();
    void updateTabsAndLabels();
    void setBalance(const interfaces::VaultBalances& balances);

Q_SIGNALS:
    void coinsSent(const uint256& txid);
    void solverSettingsRequested();
    void prepareSendConfirmationReadyForTesting();
    //! Always emitted once an accepted asynchronous preparation finishes, including
    //! failures. This keeps tests from misreporting a preparation error as a timeout.
    void sendPreparationFinishedForTesting(int status, const QString& reason);

protected:
    void showEvent(QShowEvent* event) override;

private:
    Ui::SendCoinsDialog *ui;
    ClientModel* clientModel{nullptr};
    VaultModel* model{nullptr};
    std::unique_ptr<vault::CCoinControl> m_coin_control;
    std::unique_ptr<VaultModelTransaction> m_current_transaction;
    bool fNewRecipientAllowed{true};
    const PlatformStyle *platformStyle;
    quint64 m_solve_generation{0};
    QElapsedTimer m_send_work_started;
    QTimer m_send_work_update_timer;
    QProcess* m_gpu_solver_probe{nullptr};
    QString m_gpu_solver_probe_path;
    GpuSolverProbeStatus m_gpu_solver_probe_status{GpuSolverProbeStatus::Unchecked};
    gpu::GpuState m_gpu_state;
    bool m_startup_warning_queued{false};

    // Copy PSQT to clipboard and offer to save it.
    void presentPSQT(PartiallySignedQuicksilverTransaction& psqt);
    // Process VaultModel::SendCoinsReturn and generate a pair consisting
    // of a message and message flags for use in Q_EMIT message().
    // Additional parameter msgArg can be used via .arg(msgArg).
    void processSendCoinsReturn(const VaultModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg = QString());
    bool prepareTransactionForAsync(std::unique_ptr<VaultModelTransaction>& transaction, vault::CCoinControl& coin_control);
    // Format confirmation message for m_current_transaction after prepare succeeds.
    bool PrepareSendText(QString& question_string, QString& informative_text, QString& detailed_text) const;
    void finishSendPrepare(std::unique_ptr<VaultModelTransaction> transaction, const VaultModel::SendCoinsReturn& prepare_status, quint64 generation);
    void confirmAndSendPreparedTransaction();
    void handleSendConfirmationFinished(int retval);
    void finishSendFlow(bool send_failure);
    void setSendControlsEnabled(bool enabled);
    void beginSendWorkProgress();
    void completeSendWorkProgress();
    void failSendWorkProgress();
    void hideSendWorkProgress();
    void updateSendWorkElapsed();
    void updateSendWorkDisclosure();
    void updateSendWorkGraphs(uint32_t nonce);
    QString sendWorkGraphsTriedText() const;
    QString sendWorkGraphsConfirmationText() const;
    QString sendWorkResourceText() const;
    void refreshGpuSolverProbe(bool requires_configured_gpu_solver, const QString& solver_path);
    void stopGpuSolverProbe();
    static QString sendWorkResourceText(bool requires_configured_gpu_solver, const QString& solver_path, GpuSolverProbeStatus probe_status);
    void showStartupAccelerationWarningIfNeeded();
    void confirmCpuFallbackIfNeededAndPrepare(std::unique_ptr<VaultModelTransaction> transaction, vault::CCoinControl coin_control);
    void startAsyncSendPrepare(std::unique_ptr<VaultModelTransaction> transaction, vault::CCoinControl coin_control);
    static bool cpuFallbackWarningRequired(bool slow_network, GpuSolverProbeStatus probe_status, bool warning_enabled);
    /* Sign PSQT using external signer.
     *
     * @param[in,out] psqtx the PSQT to sign
     * @param[in,out] mtx needed to attempt to finalize
     * @param[in,out] complete whether the PSQT is complete (a successfully signed multisig transaction may not be complete)
     *
     * @returns false if any failure occurred, which may include the user rejection of a transaction on the device.
     */
    bool signWithExternalSigner(PartiallySignedQuicksilverTransaction& psqt, CMutableTransaction& mtx, bool& complete);

private Q_SLOTS:
    void sendButtonClicked(bool checked);
    void cancelSendWork();
    void removeEntry(SendCoinsEntry* entry);
    void useAvailableBalance(SendCoinsEntry* entry);
    void refreshBalance();
    void coinControlFeatureChanged(bool);
    void coinControlButtonClicked();
    void coinControlChangeChecked(int);
    void coinControlChangeEdited(const QString &);
    void coinControlUpdateLabels();
    void coinControlClipboardQuantity();
    void coinControlClipboardAmount();
    void coinControlClipboardBytes();
    void coinControlClipboardChange();
    void updateNumberOfBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType synctype, SynchronizationState sync_state);

Q_SIGNALS:
    // Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);
};


#define SEND_CONFIRM_DELAY   3

class SendConfirmationDialog : public QMessageBox
{
    Q_OBJECT

public:
    SendConfirmationDialog(const QString& title, const QString& text, const QString& informative_text = "", const QString& detailed_text = "", int secDelay = SEND_CONFIRM_DELAY, bool enable_send = true, bool always_show_unsigned = true, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void countDown();
    void updateButtons();

private:
    QAbstractButton *yesButton;
    QAbstractButton *m_psqt_button;
    QTimer countDownTimer;
    int secDelay;
    QString confirmButtonText{tr("Send")};
    bool m_enable_send;
    QString m_psqt_button_text{tr("Create Unsigned")};
};

#endif // QUICKSILVER_QT_SENDCOINSDIALOG_H
