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
    void init();
    void cleanup();
    void gpuCardTablesPolluter();
    void gpuCardTablesStartClean();
    void probeEnvironmentPolluter();
    void probeEnvironmentStartsClean();
    void solverProbeStubRejectsBadInput();
    void agentAllotmentPageScrollsWithinLaptopViewport();
    void agentsPageHasNoSideScrollAt1200();
    void pagesKeepTheirTextInsidePanels();
    void receivePageSaysTheLedgerIsPublic();
    void agentAllotmentSetupAsksToUnlock();
    void agentAllotmentStatusNeverPromisesEnforcementLater();
    void agentAllotmentStopAsksThenRefusesCosign();
    void agentAllotmentCosignDisabledWithoutNode();
    void agentAllotmentRiskTextNamesTheCosigner();
    void agentAllotmentCosignReviewTracksPastedText();
    void agentAllotmentCosignUnlockDiesWithItsReview();
    void agentAllotmentStoppedAllotmentOffersNoFunding();
    void vaultTests();
    void backupChecksTheSavedFile();
    void agentAllotmentMakesHomeBackupNeeded();
    void backupToANonAsciiFolder();
    void backupOverAnExistingFile();
    void rpcChangesMakeHomeBackupNeeded();
    void benchTickerMatchesVaultBalances();
    void homeAndMineLinksFollowRail();
    void homeLedgerListsVaultTransactions();
    void homeLaysOutLedgerBesideInstruments();
    void homeAndLedgerReadTransactionsAlike();
    void transferSummaryTracksAmountsAgainstSpendable();
    void captureBenchPageScreenshots();
    void transferRecipientsAreCompactRows();
    void transferKeepsTheAddressReadableAndReviewInReach();
    void transferLabelFieldHoldsAReferenceLabel();
    void remainingPagesUseTheBenchGrammar();
    void benchStyleKeepsOneScaleAndQuietStates();
    void benchSpinAndCheckControlsStayWhole();
    void ledgerPageNamesItsFiltersAndFitsItsColumns();
    void sendConfirmationNamesUnconfirmedChange();
    void coinControlMarksUnconfirmedChange();
    void transferFollowsTheAvoidReuseFlag();
    void mineMintPageRendersStatus();
    void minePageSeparatesDraftPayoutAndHealthNote();
    void openingVaultDoesNotReplacePayoutOrMintAddress();
    void vaultViewAddressHelperTracksMiningModel();
    void noVaultPayoutDraftStaysSeparateFromVaultDraft();
    void payoutDraftPersistsPerVaultOnlyOnUserAction();
    void savedPayoutRequiresConfiguredAddress();
    void reviewConsensusButtonKeepsNaturalWidth();
    void aboutDialogWrapsAtWords();
    void aboutDialogLinksSourceOnce();
    void mineMintPageNamesAnArmedMinerWithNoPermittedSolver();
    void mineMintPageRefusesToPresentIsolatedMiningAsSuccess();
    void mineMintPageOnMainSaysMiningWaitsForPeers();
    void mineMintPageSaysWhenTheMinerIsWaitingToCatchUp();
    void launchPageOnMainSaysMiningWaitsForPeers();
    void mineMintStartRequiresPayoutAddress();
    void mineMintPageShowsARawScriptPayout();
    void mineMintPageNamesAnIdleNodeWithNothingPermitted();
    void networkPageFormatsStatus();
    void networkPageHidesDeveloperNetworksOnMain();
    void networkPageDiagnosesBootstrapAndAddsPeers();
    void networkPageHidesTheManualTorRecipeWhenAProxyIsCarryingOnions();
    void nodeWindowSaysWhyEveryFieldReadsNotApplicable();
    void devBuildBannerUsesTheWarningTone();
    void genesisTipHasNoLastBlockAge();
    void homeLastBlockAgesBetweenTips();
    void panelsMenuWindowsUseTheBenchGrammar();
    void menuDialogsUseTheBenchGrammar();
    void agentSpendRequestRefuseWaitsForARequest();
    void peerTableNamesTheConnectionInOneColumn();
    void noVaultPageIsOneBenchPanel();
    void launchPageSaysWhenConsensusHasNoPeers();
    void miningPollStopsWithTheClientModel();
    void consensusCancelDoesNotShowStarting();
    void vaultModelReturnsSendFailureReasonWithoutShowingIt();
    void vaultModelReturnsCommitFailureWithoutEmittingSendSuccess();
    void solverConfigurationErrorUsesIdentity();
    void vaultInterfaceSkipsUninitializedTipWithoutConsensus();
    void vaultModelLoadsStoredConfirmationBeforeConsensus();
    void thinHeaderSourceRefreshesDetachedVault();
    void thinHeaderSourceDoesNotReloadAnUnchangedFailedStore();
    void vaultModelSkipsCoinControlOutputsBeforeConsensusClientModel();
    void vaultViewMasksValuesWithoutClientModel();
    void mainWindowKeepsMaskValuesAndVaultRules();
    void mineMintStartUsesThePayoutShownAtThePress();
    void vaultModelReportsProofOfWorkInFlightWhilePreparing();
    void vaultModelPassesCancelRequestIntoTheGrind();
    void cancelBeforeGrindStartsIsHonoured();
    void cancelDuringWindDownDoesNotReachNextSend();
    void closeVaultCancelsOnlyThatVault();
    void sendDialogDestructorCancelsItsAttempt();
    void quitDuringTransferGrindCancelsAndWaits();
    void unloadDuringTransferGrindDefersDeletion();
    void unlockReleaseIsInsideTheInFlightWindow();
    void vaultLoadFromWorkerDoesNotHoldControllerMutexAcrossGuiCall();
    void handoverReportsFailedRemove();
    void vaultModelRejectsSendBeforeConsensusClientModel();
    void vaultModelPollsBalanceFromVaultTipWithoutClientModel();
    void vaultModelCachesEncryptionStatus();
    void sendGenerationGuardDropsStaleResult();
    void gpuSituationWordingIsExact();
    void discoveryRunsOffTheGuiThread();
    void cpuFallbackWarningFollowsDiscovery();
    void faultedCardsShowSendWarningDetails();
    void transferPageShowsItsFormAndTransmitButtonTogether();
    void transferCoinControlHasNoBytesRow();
    void coinControlDialogHasNoBytesRow();
    void coinControlIndicatorsMatchDialogCheckboxes();
    void copyAmountActionsCopyTheBareNumber();
    void transferSolverTextNamesRealMenuAndSetting();
    void transferPrecheckRefusesOverBalanceBeforeUnlock();
    void transferPrecheckRefusesDuplicateBeforeUnlock();
    void transferPrecheckRefusesZeroBeforeUnlock();
    void transferPrecheckAllowsStaleLowBalance();
    void transferHoldsUnlockThroughPreparation();
    void transferPrecheckDefersUncertainBalance_data();
    void transferPrecheckDefersUncertainBalance();
    void transferPreparationFailureWithoutGraphsHidesProgress();
    void requestPageNamesFormatsAndShowsGeneratedFormat();
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
    void mineGraphicsSettingsSitsBesideStop();
    void transferRecipientAreaFitsItsRow();
    void transferSummaryCountsTheRowClearAllLeaves();
    void transactionDescriptionCountsConfirmationsFromFirstBlock();
    void ledgerCoinbaseMaturesAfter101Blocks();
    void ledgerOffersNoAbandonButStillShowsAbandoned();
    void passphraseReachesTheCallerOnlyOnConfirm();
    void showPassphraseKeepsSensitiveInputHints();
    void failedPassphraseChangeLeavesVaultUnlocked();
    void vaultErrorBoxShowsANameAsPlainText();
    void requestLinkEscapesItsStoredAddress();
    void debugInputsNameOnlyOwnedAddresses();
    void incomingTransferIsNotNamedByReceivingLabel();
    void paymentConfirmsAtTheRecommendedTwentyConfirmations();
    void homeLedgerKeepsALabelAtFloor();
    void idStyledPrimaryButtonsShowDisabledState();
    void statusStripNamesConsensusOff();
    void notificationBurstPolicyCoalescesTransactions();
    void transactionBurstPostsOneImmediateNotification();
    void homeLedgerBurstCoalescesColumnFits();
    // G-01 (slice FL)
    void transferSaysAFullPoolLeftItSavedNotSent();
    void transferSaysARefusalLeftItSavedNotSent();
    void transferThroughARefusingNodeSaysSavedNotSent();
    void agentsPageSaysAKeptSpendIsSavedNotSent();
    // F-582 (slice FP)
    void expiredTransferNoticeSaysItFailed();
    void expiredTransferNoticeWaitsForTheVaultToOpen();
    // G1 (slice FN)
    void mineUseLastOffersOnlyARetainedAddressWhileStopped();
    void mineUseLastFillsAndStoresOnPress();
    void mineRetainedTargetIsMarkedNotApplied();
    void mineRetainedTargetNeverFillsTheField();
    void developerOptionsToggleReadsBothTexts();
    void mineDeveloperOptionsCollapsedUntilForcedOpen();
    void mineScriptWinsOverTheAddressRow();
    void mineInvalidScriptDisablesStart();
    void mineScriptNoteNamesTheScript();
    void mineAddressScriptMovesToThePayoutField();
    void mineAnyoneCanSpendAddressScriptStaysAScript();
    void mineSavedScriptNeedsConfiguredMinescript();
    void mineStartWithScriptReachesTheScriptOverload();
    // Slice GN
    void banTableUnbanFollowsTheSelectedSubnetAcrossRefresh();
    void peerAndBanTablesPassTheModelTester();
    void signChooserLeavesTheSharedAddressTableWhole();
    void messageChoosersListOnlyKeyHashAddresses();
    void agentSaveReceiptsAsksToUnlock();
    void receiveRequestRefusesAnUnparseableAmount();
    void paymentServerFrameDecoding();
    void paymentServerSecondInstanceKeepsTheLiveEndpoint();
    void paymentServerShortClientDoesNotBlock();
    // slice GM (F-619 … F-631)
    void unitControlSelectsWithoutClientModel();
    void psqtDialogWorksWithoutClientModel();
    void psqtDialogRefusesOutOfRangeOutputs();
    void psqtSaveReportsAWriteFailure();
    void psqtBroadcastFailureNamesTheReason();
    void vaultViewRescanProgressIsOwnedAndSingle();
    void transactionViewClosesDetailDialogsWhenDestroyed();
    void homeLedgerDetailsRespectMaskValues();
    void networkPageShowsNetworkingOff();
    void networkPageAddPeerRejectsAnyWhitespace();
    void recentRequestsSkipACorruptRecord();
    void recentRequestsRemoveListsOnlyWhatWasDeleted();
    void receiveRequestMenuActsOnTheRowUnderTheCursor();
    void thinHeaderSourceIgnoresARemovedStoreOnceATipIsApplied();
    void vaultControllerNoticesAThinHeaderRefusalOncePerChange();
    void startupVaultThatFailsToLoadIsReported();
};

#endif // QUICKSILVER_QT_TEST_VAULTTESTS_H
