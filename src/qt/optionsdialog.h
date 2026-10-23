// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_OPTIONSDIALOG_H
#define QUICKSILVER_QT_OPTIONSDIALOG_H

#include <QDialog>
#include <QStringList>
#include <QValidator>
#include <crypto/cuckatoo/gpu_devices.h>

#include <optional>

class ClientModel;
class OptionsModel;
class GpuDeviceList;

QT_BEGIN_NAMESPACE
class QDataWidgetMapper;
class QComboBox;
class QLabel;
class QProcess;
class QShowEvent;
QT_END_NAMESPACE

namespace Ui {
class OptionsDialog;
}

/** Proxy address widget validator, checks for a valid proxy address.
 */
class ProxyAddressValidator : public QValidator
{
    Q_OBJECT

public:
    explicit ProxyAddressValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

/** Preferences dialog. */
class OptionsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OptionsDialog(QWidget *parent, bool enableVault);
    ~OptionsDialog();

    enum Tab {
        TAB_MAIN,
        TAB_NETWORK,
        TAB_HARDWARE,
    };

    void setClientModel(ClientModel* client_model);
    void setModel(OptionsModel *model);
    //! Live consensus, not the saved setting. Unticking writes the setting only.
    void setConsensusRunning(bool running);
    void setMapper();
    void setCurrentTab(OptionsDialog::Tab tab);
    static void configureLanguageRow(const QStringList& shipped_languages, QComboBox* languages, QLabel* language_label);

private Q_SLOTS:
    /* set OK button state (enabled / disabled) */
    void setOkButtonState(bool fState);
    void on_resetButton_clicked();
    void on_openQuicksilverConfButton_clicked();
    void on_okButton_clicked();
    void on_cancelButton_clicked();

    void on_showTrayIcon_stateChanged(int state);

    void togglePruneWarning(bool enabled);
    void showRestartWarning(bool fPersistent = false);
    void clearStatusLabel();
    void updateProxyValidationState();
    void chooseGpuSolverPath();
    void validateGpuSolverPath();
    /* query the networks, for which the default proxy is used */
    void updateDefaultProxyNets();
    void updateOkButtonState();
    void stopGpuSolverProbe();
    void updateGpuMode();
    void populateGpuDevices();

protected:
    void showEvent(QShowEvent* event) override;

Q_SIGNALS:
    void quitOnReset();
    //! The box was ticked while consensus is not running. The saved setting stays false;
    //! Continue on the review page is what turns it on.
    void consensusOptInRequested();

private:
    void applyConsensusChoice();
    bool savedConsensusEnabled() const;
    Ui::OptionsDialog *ui;
    ClientModel* m_client_model{nullptr};
    OptionsModel* model{nullptr};
    QDataWidgetMapper* mapper{nullptr};
    QProcess* m_gpu_solver_probe{nullptr};
    enum class HelperCheck { kUnchecked, kChecking, kWorks, kFailed };
    HelperCheck m_gpu_solver_check{HelperCheck::kUnchecked};
    GpuDeviceList* m_gpu_device_list{nullptr};
    std::optional<cuckatoo::GpuDiscovery> m_gpu_discovery;
    bool m_proxy_valid{true};
    bool m_gpu_solver_valid{true};
    bool m_gpu_solver_command_line{false};
    //! What the node is doing now. The checkbox itself shows the saved setting.
    bool m_consensus_running{false};
    bool m_consensus_opt_in{false};
};

#endif // QUICKSILVER_QT_OPTIONSDIALOG_H
