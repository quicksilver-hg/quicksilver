// Copyright (c) 2019-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_OPTIONTESTS_H
#define QUICKSILVER_QT_TEST_OPTIONTESTS_H

#include <common/settings.h>
#include <qt/optionsmodel.h>
#include <univalue.h>

#include <QObject>

class OptionTests : public QObject
{
    Q_OBJECT
public:
    explicit OptionTests(interfaces::Node& node);

private Q_SLOTS:
    void optionsUseBenchPanelAndExplainAutomaticThreads();
    void init(); // called before each test function execution.
    void leftoverQSettingsKeysAreIgnored();
    void legacyDisplayUnitSettingIsIgnored();
    void displayUnitsAreFocused();
    void displayUnitSerializationIsCompact();
    void languageRowIsHiddenWhileNoCatalogueShips();
    void hiddenLanguageRowLeavesStoredValueAlone();
    void languageRowListsShippedCatalogues();
    void integerGetArgBug();
    void gpuSolverSettingPersistsAndRequiresRestart();
    void gpuSolverDialogRejectsInvalidPath();
    void externalSignerSurfacesAreHiddenNotGreyedOut();
    void cpuFallbackWarningPreferencePersists();
    void allowCpuBlockMiningPersistsWithoutRestart();
    void mainTabFitsAtTheOpeningSize();
    void mainTabMatchesCardSurface();
    void documentModeStripHasNoLightBase();
    void parametersInteraction();
    void extractFilter();
    void openConfDoesNotNestEventLoop();
    void resetDoesNotNestEventLoop();
    void gpuSolverBrowseDoesNotNestEventLoop();
    void displayTabHasNoDeadFontChoiceAndAlignsItsFields();

private:
    interfaces::Node& m_node;
    common::Settings m_previous_settings;
};

#endif // QUICKSILVER_QT_TEST_OPTIONTESTS_H
