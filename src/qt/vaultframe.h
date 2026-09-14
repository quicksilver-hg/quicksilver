// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_VAULTFRAME_H
#define QUICKSILVER_QT_VAULTFRAME_H

#include <QFrame>
#include <QMap>

class ClientModel;
class ConsensusReviewPage;
class DesktopLaunchPage;
class MineMintPage;
class MiningModel;
class NetworkPage;
class PageStack;
class PlatformStyle;
class SendCoinsRecipient;
class VaultModel;
class VaultView;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
QT_END_NAMESPACE

/**
 * A container for embedding all vault-related
 * controls into QuicksilverGUI. The purpose of this class is to allow future
 * refinements of the vault controls with minimal need for further
 * modifications to QuicksilverGUI, thus greatly simplifying merges while
 * reducing the risk of breaking top-level stuff.
 */
class VaultFrame : public QFrame
{
    Q_OBJECT

public:
    explicit VaultFrame(const PlatformStyle* platformStyle, QWidget* parent);
    ~VaultFrame();

    void setClientModel(ClientModel *clientModel);
    void setVaultRuntimeAvailable(bool available);

    bool addView(VaultView* vaultView);
    void setCurrentVault(VaultModel* vault_model);
    void removeVault(VaultModel* vault_model);
    void removeAllVaults();

    bool handlePaymentRequest(const SendCoinsRecipient& recipient);

    void showOutOfSyncWarning(bool fShow);
    void markConsensusInitializationFailed();

    QSize sizeHint() const override { return m_size_hint; }

    //! How much room the page on screen needs; see PageStack for why.
    //!
    //! These have to be answered here as well as in the stack. The scroll area
    //! that holds this frame asks the frame, and the frame's own layout would
    //! otherwise answer from QStackedLayout: QWidgetItem::heightForWidth asks a
    //! child widget's *layout* rather than the widget, so the stack's override
    //! is stepped over on the way through.
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;

Q_SIGNALS:
    void createVaultButtonClicked();
    void openVaultButtonClicked();
    void message(const QString& title, const QString& message, unsigned int style);
    void currentVaultSet();
    void consensusStateChanged(bool enabled);
    void networkRestartRequested(const QString& chain_token);
    void privacyRequested(bool privacy);

private:
    PageStack* vaultStack;
    DesktopLaunchPage* m_launch_page{nullptr};
    ConsensusReviewPage* m_consensus_review_page{nullptr};
    NetworkPage* m_network_page{nullptr};
    MineMintPage* m_mining_page{nullptr};
    MiningModel* m_mining_model{nullptr};
    QWidget* m_no_vault_page{nullptr};
    QLabel* m_no_vault_body{nullptr};
    QPushButton* m_no_vault_create_button{nullptr};
    QPushButton* m_no_vault_open_button{nullptr};
    VaultView* m_current_vault_view{nullptr};
    ClientModel* clientModel{nullptr};
    QMap<VaultModel*, VaultView*> mapVaultViews;
    bool m_consensus_enabled{false};
    bool m_consensus_start_failed{false};
    bool m_privacy{false};
    bool m_vault_runtime_available{false};

    bool bOutOfSync{false};

    const PlatformStyle *platformStyle;

    const QSize m_size_hint;

    void updateLaunchSummary();
    void connectLaunchSummarySignals(VaultModel* vault_model);
    bool currentVaultBackupRecorded() const;
    void setCurrentVaultBackupRecorded(bool backed_up);
    void updateConsensusState();
    void setConsensusEnabled(bool enabled);
    void acceptConsensusAndShowStatus();
    void refreshNetworkPage();
    void refreshTopLevelMiningAddressHelper();
    //! Push the connection count to every surface that has to distinguish an isolated
    //! node from a connected one. -1 means "not known yet", not "zero".
    void setPeerCount(int peers);
    //! Hand a user-typed peer address to the node; reports the outcome on the page.
    void addPeer(const QString& address);

public:
    VaultView* currentVaultView() const;
    VaultModel* currentVaultModel() const;
    bool consensusEnabled() const { return m_consensus_enabled; }

    /** Undo a consensus opt-in the user declined to see through.
     *
     * The opt-in is saved by the page that offers it, before anything is asked of the
     * open vault. If the user then backs out of closing it, the saved answer has to go
     * back with them -- otherwise the desktop believes consensus is on while nothing
     * has started it, which is the state a failed start already leaves behind.
     */
    void revertConsensusOptIn() { setConsensusEnabled(false); }

public Q_SLOTS:
    /** Switch to the desktop launch page */
    void gotoLaunchPage();
    /** Switch to overview (home) page */
    void gotoOverviewPage();
    /** Switch to the active vault, or to vault creation/opening if none exists */
    void gotoVaultPage();
    /** Switch to history (transactions) page */
    void gotoHistoryPage();
    /** Switch to mine/mint page */
    void gotoMineMintPage();
    /** Switch to agent allotments page */
    void gotoAgentAllotmentPage();
    /** Switch to consensus review page */
    void gotoNetworkPage();
    /** Switch to node status page */
    void gotoNetworkStatusPage();
    /** Switch to request page */
    void gotoReceiveCoinsPage();
    /** Switch to transfer page */
    void gotoSendCoinsPage(QString addr = "");
    /** Apply the global balance privacy mode to launch-surface summaries */
    void setPrivacy(bool privacy);

    /** Show Sign/Verify Message dialog and switch to sign message tab */
    void gotoSignMessageTab(QString addr = "");
    /** Show Sign/Verify Message dialog and switch to verify message tab */
    void gotoVerifyMessageTab(QString addr = "");

    /** Load Partially Signed Quicksilver Transaction */
    void gotoLoadPSQT(bool from_clipboard = false);

    /** Encrypt the vault */
    void encryptVault();
    /** Backup the vault */
    void backupVault();
    /** Change encrypted vault passphrase */
    void changePassphrase();
    /** Ask for passphrase to unlock vault temporarily */
    void unlockVault();

    /** Show used sending addresses */
    void usedSendingAddresses();
    /** Show used receiving addresses */
    void usedReceivingAddresses();
};

#endif // QUICKSILVER_QT_VAULTFRAME_H
