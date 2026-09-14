// Copyright (c) 2017-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_VAULTTESTS_H
#define QUICKSILVER_QT_TEST_VAULTTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
} // namespace interfaces

class VaultTests : public QObject
{
 public:
    explicit VaultTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void agentAllotmentPageScrollsWithinLaptopViewport();
    void agentAllotmentImportsNodePeers();
    void agentAllotmentRelaysInBackgroundWithPeerFallback();
    void vaultTests();
    void mineMintPageRendersStatus();
    void mineMintPageRefusesToPresentIsolatedMiningAsSuccess();
    void networkPageFormatsStatus();
    void networkPageDiagnosesBootstrapAndAddsPeers();
    void networkPageHidesTheManualTorRecipeWhenAProxyIsCarryingOnions();
    void nodeWindowSaysWhyEveryFieldReadsNotApplicable();
    void launchPageSaysWhenConsensusHasNoPeers();
    void miningPollStopsWithTheClientModel();
    void consensusCancelDoesNotShowStarting();
    void vaultModelReturnsSendFailureReasonWithoutShowingIt();
    void vaultModelReturnsCommitFailureWithoutEmittingSendSuccess();
    void vaultInterfaceSkipsUninitializedTipWithoutConsensus();
    void vaultModelLoadsStoredConfirmationBeforeConsensus();
    void thinHeaderSourceRefreshesDetachedVault();
    void vaultModelSkipsCoinControlOutputsBeforeConsensusClientModel();
    void overviewPageMasksValuesWithoutClientModel();
    void vaultModelReportsProofOfWorkInFlightWhilePreparing();
    void vaultModelPassesCancelRequestIntoTheGrind();
    void vaultModelRejectsSendBeforeConsensusClientModel();
    void vaultModelPollsBalanceFromVaultTipWithoutClientModel();
    void vaultModelCachesEncryptionStatus();
    void sendGenerationGuardDropsStaleResult();
    void sendWorkResourceTextClassifiesGpuSolver();
    void cpuFallbackWarningPolicyMatchesNetworkAndPreference();
    void transferPageShowsItsFormAndTransmitButtonTogether();
    void requestPageAddressTypeCopyHasNoBitcoinVocabulary();
    void signVerifyMessageRejectsNonBase58WithoutBitcoinVocabulary();
    void sendEntryAddressBookFillsDestinationWithoutNestedEventLoop();
    void signVerifyAddressBookDoesNotNestEventLoop();
    void closeVaultDoesNotNestEventLoop();
    void closeAllVaultsDoesNotNestEventLoop();
    void abandonProofOfWorkConfirmDoesNotNestEventLoop();
    void encryptPassphraseConfirmDoesNotNestEventLoop();
    void customChangeAddressConfirmDoesNotNestEventLoop();
    void unlockVaultDoesNotNestEventLoop();
    void unlockFailedDoesNotNestEventLoop();
    void unlockRuntimeErrorDoesNotNestEventLoop();
    void changePassMismatchDoesNotNestEventLoop();
    void unloadWithUnrelatedModalDoesNotDefer();
    void unloadWithVaultParentedModalDefers();
};

#endif // QUICKSILVER_QT_TEST_VAULTTESTS_H
