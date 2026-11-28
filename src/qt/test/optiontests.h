// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_OPTIONTESTS_H
#define QUICKSILVER_QT_TEST_OPTIONTESTS_H

#include <common/settings.h>
#include <qt/optionsmodel.h>
#include <univalue.h>

#include <QMap>
#include <QObject>
#include <QString>
#include <QVariant>

#include <optional>
#include <string>

//! F-595: keeps the real bundled helper off the build machine's hardware for the
//! whole test binary. A helper a test named itself still runs.
void ShieldOptionsDialogFromBundledHelper();

class OptionTests : public QObject
{
    Q_OBJECT
public:
    explicit OptionTests(interfaces::Node& node);

private Q_SLOTS:
    void optionsUseBenchPanelAndExplainAutomaticThreads();
    void init(); // called before each test function execution.
    void cleanup(); // called after each test function execution, failed or not.
    void leftoverQSettingsKeysAreIgnored();
    void legacyDisplayUnitSettingIsIgnored();
    void displayUnitsAreFocused();
    void displayUnitSerializationIsCompact();
    void newInstallShowsCinnabarAndKeepsAStoredUnit();
    void languageRowIsHiddenWhileNoCatalogueShips();
    void hiddenLanguageRowLeavesStoredValueAlone();
    void languageRowListsShippedCatalogues();
    void integerGetArgBug();
    void gpuSolverSettingPersistsAndRequiresRestart();
    void gpuSolverDialogRejectsInvalidPath();
    void commandLineGraphicsHelperIsShown();
    void gpuDevicesSettingRoundTrips();
    void gpuCardsListAndModes();
    void choosingCardsRequiresASelection();
    void sameNamedCardsShowTheirIds();
    void gpuCardStatsRefresh();
    void amdgpuAdvisoryFollowsTheKernelLimit();
    void dialogTestsDoNotRunTheBundledHelper();
    void injectedDiscoveryFillsTheDialog();
    void defaultDiscoveryRunnerListsThroughTheHelper();
    void missingBundledHelperIsNamed();
    void cardsHintNeedsACard();
    void tickingBuiltInGraphicsAsksFirst();
    void missingChosenCardKeepsItsName();
    void onlyChosenCardMissingStaysChosen();
    void faultedCardWithoutSlotIsNamedById();
    void automaticRowsFollowTheMinersSelection();
    void cpuOnlyHidesTheList();
    void savedGpuDevicesSelectTheMatchingRadio();
    void externalSignerSurfacesAreAbsent();
    void cpuFallbackWarningPreferencePersists();
    void allowCpuBlockMiningPersistsWithoutRestart();
    void hardwareTabFitsAtTheOpeningSize();
    void graphicsSettingsOpenOnTheHardwareTab();
    void mainTabMatchesCardSurface();
    void documentModeStripHasNoLightBase();
    void parametersInteraction();
    void extractFilter();
    void openConfDoesNotNestEventLoop();
    void resetDoesNotNestEventLoop();
    void gpuSolverBrowseDoesNotNestEventLoop();
    void displayTabHasNoDeadFontChoiceAndAlignsItsFields();
    void coinControlIsAlwaysOnTransfer();
    void storedCoinControlSettingIsDropped();
    void settingsPruneMinimumIsTheBlockFileFloor();
    void storedPruneSizeBelowTheFloorIsRaised();
    void confPruneSizeBelowTheFloorIsNotWritten();
    void storedPrevPruneSizeBelowTheFloorIsRaised();
    void savedGpuModeOpensWithoutARestartWarning();
    void switchedOffInvalidProxyLeavesOkEnabled();
    void emptyProxyHostBlocksOk();
    void loadedInvalidProxyHostIsChecked();
    void loadedHostNameProxyIsValid();
    void ipv6ProxyHostRoundTrips();
    void optionsModelIsAFlatEditOnlyList();

private:
    interfaces::Node& m_node;
    common::Settings m_previous_settings;
    //! settings.json as this class found it (nullopt: absent).
    std::optional<std::string> m_previous_settings_file;
    QMap<QString, QVariant> m_previous_qsettings;
};

#endif // QUICKSILVER_QT_TEST_OPTIONTESTS_H
