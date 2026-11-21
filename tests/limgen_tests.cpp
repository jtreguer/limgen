// Qt Test suite for limgen. Every test works in a temporary folder: Omarchy's
// state, the user's background folder, the XDG folders and the fake tools are
// all made fresh, so nothing here reads or writes the real Omarchy files
// (CLAUDE.md, "Rules that must hold"). No test uses the network.

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QLockFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QStorageInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <functional>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <memory>
#include <signal.h>
#include <unistd.h>

#include "backend.h"
#include "catalogue.h"
#include "engine.h"
#include "hardware.h"
#include "library.h"
#include "modelstore.h"
#include "progress.h"
#include "sdcppengine.h"
#include "omarchy.h"
#include "palette.h"
#include "prompt.h"
#include "theme.h"
#include "translator.h"

namespace {

// A made-up theme in the stock Tokyo Night style. Unknown keys, a comment,
// single quotes and an upper-case hex are in there on purpose.
const char *const stockToml = R"(# a comment
background = "#1a1b26"
foreground = "#A9B1D6"
accent = '#7aa2f7'
cursor = "#c0caf5"
color0 = "#15161e"
red = "#f7768e"
green = "#9ece6a"
yellow = "#e0af68"
blue = "#7aa2f7"
magenta = "#bb9af7"
cyan = "#7dcfff"
bright_red = "#ff00ff"
)";

bool writeFile(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(content);
    return true;
}

bool writeScript(const QString &path, const QByteArray &body) {
    if (!writeFile(path, "#!/bin/sh\n" + body))
        return false;
    return QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeUser | QFile::ExeGroup);
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Replaces `path` with a symlink to `target` the way Omarchy does: the new
// link is made beside it and renamed over, so there is never a gap.
bool replaceSymlink(const QString &target, const QString &path) {
    const QString temporary = path + ".new";
    QFile::remove(temporary);
    return QFile::link(target, temporary) && std::rename(qPrintable(temporary), qPrintable(path)) == 0;
}

bool replaceFile(const QString &path, const QByteArray &content) {
    const QString temporary = path + ".new";
    return writeFile(temporary, content) && std::rename(qPrintable(temporary), qPrintable(path)) == 0;
}

// Saves some environment variables and puts them back on destruction, so a
// test that sets them cannot leak into the next one.
class EnvGuard {
public:
    explicit EnvGuard(const QStringList &names) {
        for (const QString &name : names)
            m_saved.insert(name, qgetenv(qPrintable(name)));
        for (const QString &name : names)
            qunsetenv(qPrintable(name));
    }
    ~EnvGuard() {
        for (auto it = m_saved.begin(); it != m_saved.end(); ++it) {
            if (it.value().isNull() && !qEnvironmentVariableIsSet(qPrintable(it.key())))
                continue;
            if (it.value().isNull())
                qunsetenv(qPrintable(it.key()));
            else
                qputenv(qPrintable(it.key()), it.value());
        }
    }

private:
    QHash<QString, QByteArray> m_saved;
};

// A fake Omarchy install in a temp folder: state/theme is a symlink to one of
// themes/<name>, the way a theme switch leaves it.
struct Fixture {
    QTemporaryDir root;
    QString state, backgrounds, themes;
    // For the L2 tests: limgen's own folders and the fake GPU sources.
    QString models, data, cache, drm;

    Fixture() {
        const QString base = root.path();
        models = base + "/models";
        data = base + "/data";
        cache = base + "/cache";
        drm = base + "/drm";
        QDir().mkpath(drm); // empty: the host's real GPUs must never be read
        state = base + "/state/omarchy/current";
        backgrounds = base + "/config/omarchy/backgrounds";
        themes = base + "/themes";
        QDir().mkpath(state);
        QDir().mkpath(backgrounds);
    }
    bool ok() const { return root.isValid(); }

    OmarchyPaths paths() const {
        OmarchyPaths paths;
        paths.state = state;
        paths.backgrounds = backgrounds;
        paths.language = root.path() + "/config/omarchy/language";
        return paths;
    }
    bool addTheme(const QString &name, const QByteArray &colors) {
        return writeFile(themes + "/" + name + "/colors.toml", colors);
    }
    // Makes `name` the current theme: theme.name and the theme link, both by
    // rename, like omarchy-theme-set.
    bool switchTo(const QString &name) {
        return replaceSymlink(themes + "/" + name, state + "/theme") &&
               replaceFile(state + "/theme.name", name.toUtf8() + "\n");
    }
};

QByteArray tomlWith(const QString &background, const QString &foreground, const QString &accent,
                    const QString &extra = QString()) {
    return QString("background = \"%1\"\nforeground = \"%2\"\naccent = \"%3\"\n%4")
        .arg(background, foreground, accent, extra).toUtf8();
}

QStringList roles(const Palette &palette) {
    QStringList list;
    for (const PaletteColor &colour : palette.colors)
        list << colour.role;
    return list;
}

} // namespace

class LimgenTests : public QObject {
    Q_OBJECT

private slots:
    void cleanup() {
        // Translator changes the application-wide locale and layout direction.
        QLocale::setDefault(QLocale::c());
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
    }

    // ---- Palette ---------------------------------------------------------
    void palette_picksRolesThenTwoMostChromaticNamedColours();
    void palette_namedColoursOrderedByChroma();
    void palette_skipsNamedColourEqualToOneAlreadyPicked();
    void palette_skipsDuplicateHexBetweenNamedColours();
    void palette_ignoresBrightColoursUnknownKeysAndComments();
    void palette_acceptsSingleQuotesAndLowerCasesHex();
    void palette_commentedOutRoleIsMissing();
    void palette_missingOrMalformedRoleNamesTheRole_data();
    void palette_missingOrMalformedRoleNamesTheRole();
    void palette_onlyThreeRolesGivesThreeColours();
    void palette_modeTakenAsGiven_data();
    void palette_modeTakenAsGiven();
    void palette_modeFallsBackToBackgroundLightness_data();
    void palette_modeFallsBackToBackgroundLightness();
    void palette_moodFollowsMode();
    void palette_describePinned_data();
    void palette_describePinned();
    void palette_describeSeparatesBlueFromViolet();
    void palette_describeInvalidInput_data();
    void palette_describeInvalidInput();
    void palette_wordsDeduplicatedInOrder();
    void palette_labOfWhiteAndBlack();

    // ---- Prompt ----------------------------------------------------------
    void prompt_cinematicMatchesBenchmarkFormat();
    void prompt_invalidPaletteLeavesOutPaletteAndMood();
    void prompt_defaultPaletteLeavesOutPaletteAndMood();
    void prompt_emptySubjectStartsWithStyleLead();
    void prompt_subjectWhitespaceSimplified();
    void prompt_styleIdsRoundTrip();
    void prompt_unknownStyleIdLeavesStyleUntouched();
    void prompt_negativesHaveSharedListAndStyleExtras_data();
    void prompt_negativesHaveSharedListAndStyleExtras();
    void prompt_positivesDifferPerStyle();

    // ---- Omarchy ---------------------------------------------------------
    void omarchy_slugFromThemeName();
    void omarchy_slugUsesFirstLineOnly();
    void omarchy_unsafeSlugsAreRejected_data();
    void omarchy_unsafeSlugsAreRejected();
    void omarchy_noThemeNameFile();
    void omarchy_slugCheckIsAnchoredAtTheEnd();
    void omarchy_readColorsMissingFileSetsError();
    void omarchy_readColorsReturnsText();
    void omarchy_themeBackgroundsFiltersAndSorts();
    void omarchy_userBackgroundsFiltersAndSorts();
    void omarchy_userBackgroundDirectory();
    void omarchy_parseMonitorsSkipsDisabledAndEmpty();
    void omarchy_parseMonitorsTransformSwapsSize_data();
    void omarchy_parseMonitorsTransformSwapsSize();
    void omarchy_parseMonitorsBadJson_data();
    void omarchy_parseMonitorsBadJson();
    void omarchy_largestPicksMostPixels();
    void omarchy_monitorsRunsHyprctl();
    void omarchy_monitorsEmptyWithoutHyprctl();
    void omarchy_monitorsEmptyWhenHyprctlFails();
    void omarchy_monitorsGivesUpOnSlowHyprctl();
    void omarchy_changedFiresOnThemeSwitchAndAgain();
    void omarchy_changedFiresWhenCreatedLater();
    void omarchy_pathsFromEnvironment();
    void omarchy_pathsFromXdgWhenNoOverrides();
    void omarchy_pathsEmptyOverrideIsIgnored();

    // ---- Backend ---------------------------------------------------------
    void backend_withThemeHasSwatchesAndNoStatus();
    void backend_withoutThemeHasStatusAndNoSwatches();
    void backend_brokenColorsPutsPaletteErrorInStatus();
    void backend_missingColorsFileGivesStatus();
    void backend_paletteChangedAfterThemeSwitch();

    // ---- CLI (the built binary) -----------------------------------------
    void cli_paletteText();
    void cli_paletteJson();
    void cli_paletteWithoutThemeFails();
    void cli_paletteWithMissingColorsFails();
    void cli_paletteWithBrokenColorsFails();
    void cli_paletteRejectsExtraArgumentAndBadOption();
    void cli_unknownCommandIsUsageError();
    void cli_helpCommands();
    void cli_helpForUnknownTopicIsUsageError();
    void cli_openWithArgumentsNeedsNoDisplay();
    void cli_version();
    void cli_runsWithoutDisplayUnderGtkPlatformTheme();

    // ---- Theme and Translator -------------------------------------------
    void theme_fallbackAccentAndForeground();
    void theme_readsAccentAndPicksForeground_data();
    void theme_readsAccentAndPicksForeground();
    void theme_followsAccentChange();
    void theme_radiusFromHyprctl();
    void theme_radiusStaysZeroWhenHyprctlFails();
    void translator_englishFallback();
    void translator_regionalFallsBackToBase();
    void translator_pluralFormsRussian_data();
    void translator_pluralFormsRussian();
    void translator_pluralFallsBackToOther();
    void translator_readLanguageRejectsJunk();
    void translator_followsLanguageFileChange();

    // ---- L2: Catalogue ---------------------------------------------------
    void catalogue_builtInIsValidAndPinned();
    void catalogue_builtInFilesAndLevelsAreConsistent();
    void catalogue_loadRejectsBrokenCatalogues_data();
    void catalogue_loadRejectsBrokenCatalogues();
    void catalogue_loadAcceptsTheUnchangedCatalogue();
    void catalogue_lookups();
    void catalogue_defaultModelsDirectory();
    void catalogue_pathAndState();
    void catalogue_stateVerifiesSha256();
    void catalogue_missingFiles();

    // ---- L2: Hardware ----------------------------------------------------
    void hardware_parseNvidiaSmi();
    void hardware_parseNvidiaSmiNameWithCommas();
    void hardware_parseNvidiaSmiSkipsJunk_data();
    void hardware_parseNvidiaSmiSkipsJunk();
    void hardware_readDrmAmdIntelAndSkips();
    void hardware_readDrmAmdWithoutMemoryFiles();
    void hardware_readDrmMissingRoot();
    void hardware_detectWithFakes();
    void hardware_detectIgnoresFailingOrMissingSmi();
    void hardware_detectGivesUpOnSlowSmi();
    void hardware_sourcesFromEnvironment();
    void hardware_matching();

    // ---- L2: ProgressParser ----------------------------------------------
    void progress_zImageLogGivesAllEvents();
    void progress_kleinLogGivesAllEvents();
    void progress_chunkSizeDoesNotChangeEvents_data();
    void progress_chunkSizeDoesNotChangeEvents();
    void progress_tileProgressAfterSamplingIsIgnored();
    void progress_ignoresOtherTotalsAndOutOfRangeSteps();
    void progress_itPerSecondBecomesSeconds();
    void progress_stripsAnsiAndCarriageReturns();
    void progress_finishParsesUnterminatedLine();
    void progress_extraSampledLinesAreIgnored();
    void progress_esrganOomLogGivesErrors();

    // ---- L2: SdCppEngine, static -----------------------------------------
    void engine_argumentsFullRequest();
    void engine_argumentsOptionsExistInPinnedHelp();
    void engine_argumentsNegativeOnlyWithGuidance();
    void engine_argumentsPreviewNeedsFlagAndTaeFile();
    void engine_argumentsTilingAndSampler();
    void engine_argumentsRolesMapToOptions();
    void engine_parseVersion_data();
    void engine_parseVersion();
    void engine_parseDevices();
    void engine_deviceBackend_data();
    void engine_deviceBackend();
    void engine_probeGpuSkipsCpu();
    void engine_classify_data();
    void engine_classify();
    void engine_classifyRealLogs();
    void engine_classifyDetailHasErrorLines();
    void engine_draftPaths();
    void engine_defaultProgram();
    void engine_probeMissingProgram();
    void engine_probeWrongVersion();
    void engine_probeNoGpu();
    void engine_probeOk();

    // ---- L2: SdCppEngine against bin/fake-sd-cli --------------------------
    void engine_runFinishesWithDrafts();
    void engine_runReportsPreviewsWithImageIndices();
    void engine_previewsFromAnEarlierAttemptAreNotReported();
    void engine_runSecondStartWhileRunningIsIgnored();
    void engine_oomOnceRetriesWithVaeTiling();
    void engine_oomFailsAfterOneRetry();
    void engine_oomWithTilingAlreadyOnIsNotRetried();
    void engine_errorExit0Fails();
    void engine_wrongSizeFails();
    void engine_failModeFails();
    void engine_cancelDuringHangLeavesNoFiles();
    void engine_missingProgramFails();

    // ---- L2: Library -----------------------------------------------------
    void library_isValidId_data();
    void library_isValidId();
    void library_newIdFormatAndUtcDate();
    void library_defaultDirectory();
    void library_addDraftWritesImageAndSidecar();
    void library_addDraftWithoutThemeHasNullTheme();
    void library_addDraftNeverOverwritesAndIdsDiffer();
    void library_addDraftFromMissingSourceFails();
    void library_addDraftIntoUnwritableFolderFails();
    void library_findRoundTrip();
    void library_findRejectsBadEntries();

    // ---- L2: CLI doctor and generate -------------------------------------
    void cli_doctorJson();
    void cli_doctorText();
    void cli_doctorDefaultModelMissing();
    void cli_doctorTooLittleFreeMemory();
    void cli_doctorUnknownMemoryIsNotAProblem();
    void cli_doctorNoGpuWrongVersionOrMissingEngine();
    void cli_generateJsonWithTheme();
    void cli_generateTextOutput();
    void cli_generateRandomSeedCountsUp();
    void cli_generatePromptOverridesBuiltPrompt();
    void cli_generateStyleOptionIsHonoured();
    void cli_generateSelectsModelById();
    void cli_generateWithoutThemeStillWorks();
    void cli_generateUsageErrors_data();
    void cli_generateUsageErrors();
    void cli_generateModelNotInstalled();
    void cli_generateNeedsNoPreviewDecoder();
    void cli_generateTooLittleFreeMemory();
    void cli_generateEngineProblems_data();
    void cli_generateEngineProblems();
    void cli_generateOomOnceSucceeds();
    void cli_generateInterruptedExits130();
    void cli_generateKeepsTheThemeItStartedWith();
    void cli_generateInterruptedByTerminalExits130();
    void cli_helpListsDoctorAndGenerate();

    // ---- L3: ModelStore --------------------------------------------------
    void store_freshDownloadInstalls();
    void store_severalFilesInstallInOrder();
    void store_resumeSendsRangeAndEndsCorrect();
    void store_serverIgnoringRangeStartsOver();
    void store_wrongContentRangeStartsOver();
    void store_completePartIsVerifiedWithoutRequest();
    void store_corruptCompletePartIsRemoved();
    void store_status416OnShortPartDoesNotInstall();
    void store_oversizedPartStartsAgain();
    void store_droppedConnectionIsRetriedAndResumed();
    void store_retriesExhaustedFailsAndKeepsPart();
    void store_finalErrorsFailAtOnce_data();
    void store_finalErrorsFailAtOnce();
    void store_checksumMismatchRemovesPart();
    void store_oversizeBodyIsRemoved();
    void store_redirectIsFollowed();
    void store_redirectKeepsRange();
    void store_wrongSizedTargetIsReplaced();
    void store_cancelKeepsPartAndNextStartResumes();
    void store_cancelBeforeStartDoesNothing();
    void store_startWhileRunningIsIgnored();
    void store_neverReachesBeyondLocalhost();

    // ---- L3: ModelStore::plan --------------------------------------------
    void plan_freshListsLevelFilesThenUpscaler();
    void plan_upscalerOptional();
    void plan_installedFilesAreSkipped();
    void plan_sharedFileOfInstalledModelIsSkipped();
    void plan_sameFileTwiceInLevelIsPlannedOnce();
    void plan_partBytes();
    void plan_availableBytes();
    void plan_fitsOnDisk();

    // ---- L3: CLI models --------------------------------------------------
    void cli_modelsListText();
    void cli_modelsListJson();
    void cli_modelsRunsHereFollowsFreeMemory();
    void cli_modelsUsageErrors_data();
    void cli_modelsUsageErrors();
    void cli_modelsPullAlreadyInstalled();
    void cli_modelsPullWithoutYesAsksNothingAndWritesNothing();
    void cli_modelsPullHeldLock();
    void cli_notInstalledMessagesNameThePullCommand();
    void cli_helpModels();

    // ---- L3 review: behaviour changes and end-to-end pulls ----------------
    void store_dropEveryRequestStillFinishes();
    void store_redirectLoopFailsWithoutRetries();
    void store_unreadableCompletePartFailsAndIsKept();
    void store_cancelBetweenHashAndVerifiedEmitsOnce();
    void store_cancelFromProgressDuringVerifyEmitsOnce();
    void cli_pullEndToEnd();
    void cli_pullEndToEndJson();
    void cli_pullSecondTimeDownloadsNothing();
    void cli_pullInterruptedResumes();
    void cli_pullNotFoundFails();
    void cli_pullChecksumMismatchFails();
    void cli_modelsListWithCustomCatalogue();
    void cli_pullPromptYes();
    void cli_pullPromptDecline();
    void cli_pullReplansAfterTheLock();
    void cli_brokenCatalogueFileIsReported();

private:
    struct CliResult {
        int exitCode = -1;
        QString out, err;
        bool started = false;
    };
    CliResult runCli(const Fixture &fixture, const QStringList &arguments,
                     const QHash<QString, QString> &extraEnvironment = {}) const;
    QProcessEnvironment cliEnvironment(const Fixture &fixture, const QHash<QString, QString> &extraEnvironment) const;
    static void writeStockTheme(Fixture &fixture, const QByteArray &colors = stockToml) {
        QVERIFY(fixture.addTheme("night-city", colors));
        QVERIFY(fixture.switchTo("night-city"));
    }
};

// ============================== Palette ==================================

void LimgenTests::palette_picksRolesThenTwoMostChromaticNamedColours() {
    const Palette palette = Palette::fromToml(stockToml);
    QVERIFY2(palette.isValid(), qPrintable(palette.error));
    // Chroma in OKLCH: magenta #bb9af7 and blue #7aa2f7 beat red, green,
    // yellow and cyan here, but blue is the accent, so it is skipped and the
    // next most chromatic named colour takes its place.
    QCOMPARE(palette.colors.size(), 5);
    QCOMPARE(palette.colors[0].role, QString("background"));
    QCOMPARE(palette.colors[0].hex, QString("#1a1b26"));
    QCOMPARE(palette.colors[1].role, QString("foreground"));
    QCOMPARE(palette.colors[1].hex, QString("#a9b1d6"));
    QCOMPARE(palette.colors[2].role, QString("accent"));
    QCOMPARE(palette.colors[2].hex, QString("#7aa2f7"));
    QVERIFY(!roles(palette).contains("blue"));
    QVERIFY(!roles(palette).contains("bright_red"));
    for (const PaletteColor &colour : palette.colors)
        QVERIFY2(!colour.words.isEmpty(), qPrintable(colour.role));
}

void LimgenTests::palette_namedColoursOrderedByChroma() {
    // The file lists the dull colours first, so file order and chroma order
    // differ. Pure #ff00ff and #00ff00 are the two most chromatic.
    const QByteArray toml = tomlWith("#101010", "#eeeeee", "#445566",
                                     "red = \"#a05050\"\n"
                                     "yellow = \"#808060\"\n"
                                     "green = \"#00ff00\"\n"
                                     "magenta = \"#ff00ff\"\n");
    const Palette palette = Palette::fromToml(toml);
    QVERIFY2(palette.isValid(), qPrintable(palette.error));
    QCOMPARE(roles(palette), (QStringList{"background", "foreground", "accent", "magenta", "green"}));
}

void LimgenTests::palette_skipsNamedColourEqualToOneAlreadyPicked() {
    // Magenta is the most chromatic named colour and is also the accent, in
    // upper case in the file. Without the skip it would be picked again and
    // push green out.
    const QByteArray toml = tomlWith("#101010", "#eeeeee", "#ff00ff",
                                     "magenta = \"#FF00FF\"\n"
                                     "blue = \"#0000ff\"\n"
                                     "green = \"#00ff00\"\n"
                                     "red = \"#ff0000\"\n");
    const Palette palette = Palette::fromToml(toml);
    QVERIFY2(palette.isValid(), qPrintable(palette.error));
    QCOMPARE(roles(palette), (QStringList{"background", "foreground", "accent", "blue", "green"}));
    QStringList hexes;
    for (const PaletteColor &colour : palette.colors)
        hexes << colour.hex;
    hexes.removeDuplicates();
    QCOMPARE(hexes.size(), palette.colors.size());
}

void LimgenTests::palette_skipsDuplicateHexBetweenNamedColours() {
    // Two named colours with the same value count once.
    const QByteArray toml = tomlWith("#101010", "#eeeeee", "#445566",
                                     "red = \"#ff00ff\"\n"
                                     "magenta = \"#ff00ff\"\n"
                                     "green = \"#00ff00\"\n");
    const Palette palette = Palette::fromToml(toml);
    QVERIFY2(palette.isValid(), qPrintable(palette.error));
    QCOMPARE(palette.colors.size(), 5);
    QCOMPARE(roles(palette).mid(3).size(), 2);
    QVERIFY(roles(palette).contains("green"));
    // Only one of red and magenta is in.
    QCOMPARE(int(roles(palette).contains("red")) + int(roles(palette).contains("magenta")), 1);
}

void LimgenTests::palette_ignoresBrightColoursUnknownKeysAndComments() {
    const QByteArray toml = tomlWith("#101010", "#eeeeee", "#445566",
                                     "bright_green = \"#00ff00\"\n"
                                     "cursor = \"#ff00ff\"\n"
                                     "color1 = \"#ff0000\"\n"
                                     "# red = \"#ff0000\"\n"
                                     "   # blue = \"#0000ff\"\n"
                                     "orange = \"#c08040\"\n");
    const Palette palette = Palette::fromToml(toml);
    QVERIFY2(palette.isValid(), qPrintable(palette.error));
    QCOMPARE(roles(palette), (QStringList{"background", "foreground", "accent", "orange"}));
}

void LimgenTests::palette_acceptsSingleQuotesAndLowerCasesHex() {
    const Palette palette = Palette::fromToml("background = '#ABCDEF'\nforeground = \"#FFFFFF\" # trailing comment\n"
                                              "accent = \"#0A0B0C\"\n");
    QVERIFY2(palette.isValid(), qPrintable(palette.error));
    QCOMPARE(palette.colors[0].hex, QString("#abcdef"));
    QCOMPARE(palette.colors[1].hex, QString("#ffffff"));
    QCOMPARE(palette.colors[2].hex, QString("#0a0b0c"));
}

void LimgenTests::palette_commentedOutRoleIsMissing() {
    const Palette palette = Palette::fromToml("background = \"#000000\"\nforeground = \"#ffffff\"\n"
                                              "# accent = \"#ff0000\"\n");
    QVERIFY(!palette.isValid());
    QVERIFY2(palette.error.contains("accent"), qPrintable(palette.error));
    QVERIFY(palette.colors.size() < 3);
}

void LimgenTests::palette_missingOrMalformedRoleNamesTheRole_data() {
    QTest::addColumn<QString>("toml");
    QTest::addColumn<QString>("role");
    const QString ok = "background = \"#000000\"\n", ok2 = "foreground = \"#ffffff\"\n",
                  ok3 = "accent = \"#ff0000\"\n";
    QTest::newRow("empty text, background first") << QString() << "background";
    QTest::newRow("missing background") << ok2 + ok3 << "background";
    QTest::newRow("missing foreground") << ok + ok3 << "foreground";
    QTest::newRow("missing accent") << ok + ok2 << "accent";
    QTest::newRow("short background") << "background = \"#12345\"\n" + ok2 + ok3 << "background";
    QTest::newRow("no hash in foreground") << ok + "foreground = \"ffffff\"\n" + ok3 << "foreground";
    QTest::newRow("non-hex accent") << ok + ok2 + "accent = \"#gggggg\"\n" << "accent";
    QTest::newRow("colour name as accent") << ok + ok2 + "accent = \"red\"\n" << "accent";
    QTest::newRow("unquoted value") << ok + ok2 + "accent = #ff0000\n" << "accent";
    QTest::newRow("eight digit hex") << "background = \"#000000ff\"\n" + ok2 + ok3 << "background";
}

void LimgenTests::palette_missingOrMalformedRoleNamesTheRole() {
    QFETCH(QString, toml);
    QFETCH(QString, role);
    const Palette palette = Palette::fromToml(toml);
    QVERIFY(!palette.isValid());
    QVERIFY2(palette.error.contains(role), qPrintable(palette.error));
    // The other two role names must not be in the message, or it could not
    // tell the user which one to fix.
    for (const QString &other : QStringList{"background", "foreground", "accent"})
        if (other != role)
            QVERIFY2(!palette.error.contains(other), qPrintable(palette.error));
}

void LimgenTests::palette_onlyThreeRolesGivesThreeColours() {
    const Palette palette = Palette::fromToml(tomlWith("#000000", "#ffffff", "#ff0000"));
    QVERIFY(palette.isValid());
    QCOMPARE(roles(palette), (QStringList{"background", "foreground", "accent"}));
    QCOMPARE(palette.words.size(), 3);
}

void LimgenTests::palette_modeTakenAsGiven_data() {
    QTest::addColumn<QString>("background");
    QTest::addColumn<QString>("mode");
    // The stated mode wins over what the background suggests.
    QTest::newRow("dark on light background") << "#ffffff" << "dark";
    QTest::newRow("light on dark background") << "#000000" << "light";
}

void LimgenTests::palette_modeTakenAsGiven() {
    QFETCH(QString, background);
    QFETCH(QString, mode);
    const Palette palette = Palette::fromToml(tomlWith(background, "#808080", "#ff0000", "mode = \"" + mode + "\"\n"));
    QVERIFY(palette.isValid());
    QCOMPARE(palette.mode, mode);
}

void LimgenTests::palette_modeFallsBackToBackgroundLightness_data() {
    QTest::addColumn<QString>("background");
    QTest::addColumn<QString>("extra");
    QTest::addColumn<QString>("mode");
    QTest::newRow("missing, dark") << "#1a1b26" << "" << "dark";
    QTest::newRow("missing, light") << "#fafafa" << "" << "light";
    QTest::newRow("junk, dark") << "#1a1b26" << "mode = \"auto\"\n" << "dark";
    QTest::newRow("junk, light") << "#fafafa" << "mode = \"auto\"\n" << "light";
    QTest::newRow("wrong case is junk") << "#fafafa" << "mode = \"Dark\"\n" << "light";
    QTest::newRow("empty string") << "#fafafa" << "mode = \"\"\n" << "light";
}

void LimgenTests::palette_modeFallsBackToBackgroundLightness() {
    QFETCH(QString, background);
    QFETCH(QString, extra);
    QFETCH(QString, mode);
    const Palette palette = Palette::fromToml(tomlWith(background, "#808080", "#ff0000", extra));
    QVERIFY(palette.isValid());
    QCOMPARE(palette.mode, mode);
}

void LimgenTests::palette_moodFollowsMode() {
    const Palette dark = Palette::fromToml(tomlWith("#000000", "#ffffff", "#ff0000", "mode = \"dark\"\n"));
    const Palette light = Palette::fromToml(tomlWith("#000000", "#ffffff", "#ff0000", "mode = \"light\"\n"));
    QCOMPARE(dark.mood, Palette::moodFor("dark"));
    QCOMPARE(light.mood, Palette::moodFor("light"));
    QVERIFY(dark.mood != light.mood);
    // Pinned words, since they are model input.
    QCOMPARE(Palette::moodFor("dark"), QString("night, low-key lighting, deep shadows"));
    QCOMPARE(Palette::moodFor("light"), QString("bright airy daylight, high-key lighting, soft shadows"));
}

void LimgenTests::palette_describePinned_data() {
    QTest::addColumn<QString>("hex");
    QTest::addColumn<QString>("words");
    QTest::newRow("tokyo background") << "#1a1b26" << "near-black indigo";
    QTest::newRow("tokyo foreground") << "#a9b1d6" << "muted lavender";
    QTest::newRow("tokyo blue") << "#7aa2f7" << "blue";
    QTest::newRow("tokyo red") << "#f7768e" << "light rose";
    QTest::newRow("tokyo green") << "#9ece6a" << "light green";
    QTest::newRow("deep navy") << "#05182e" << "muted navy";
    QTest::newRow("nord charcoal") << "#2e3440" << "charcoal";
    QTest::newRow("nord off-white") << "#d8dee9" << "off-white";
    QTest::newRow("white") << "#ffffff" << "near-white";
    QTest::newRow("black") << "#000000" << "near-black";
    QTest::newRow("upper case input") << "#FFFFFF" << "near-white";
    // These two follow what the code does today. The task listed #808080 as
    // "grey"; the code says "dark grey" (OKLCH lightness is about 60, and the
    // grey band starts at 62).
    QTest::newRow("mid grey") << "#808080" << "dark grey";
    QTest::newRow("grey") << "#a0a0a0" << "grey";
    QTest::newRow("light grey") << "#c0c0c0" << "light grey";
    QTest::newRow("pure red") << "#ff0000" << "vivid red";
    QTest::newRow("pure blue") << "#0000ff" << "dark vivid blue";
    QTest::newRow("violet") << "#8000ff" << "vivid violet";
    // Light and saturated is "bright", not "pale vivid"; "cream" is kept for
    // light colours with little chroma. The default accent is a golden yellow.
    QTest::newRow("default accent") << "#FFD60A" << "bright amber";
    QTest::newRow("pure yellow") << "#ffff00" << "bright yellow";
    QTest::newRow("cream") << "#f6dcac" << "cream";
    QTest::newRow("muted cream") << "#dcd7ba" << "muted cream";
}

void LimgenTests::palette_describePinned() {
    QFETCH(QString, hex);
    QFETCH(QString, words);
    QCOMPARE(Palette::describe(hex), words);
}

void LimgenTests::palette_describeSeparatesBlueFromViolet() {
    // CIELAB puts pure blue and violet only 6 degrees apart, which is why the
    // names come from OKLCH. All three must land in different families.
    const QString red = Palette::describe("#ff0000"), blue = Palette::describe("#0000ff"),
                  violet = Palette::describe("#8000ff");
    QVERIFY(blue.contains("blue"));
    QVERIFY(violet.contains("violet"));
    QVERIFY(red.contains("red"));
    QVERIFY(!blue.contains("violet"));
    QVERIFY(!violet.contains("blue"));
    QCOMPARE(QSet<QString>({red, blue, violet}).size(), 3);
}

void LimgenTests::palette_describeInvalidInput_data() {
    QTest::addColumn<QString>("hex");
    QTest::newRow("empty") << "";
    QTest::newRow("name") << "red";
    QTest::newRow("short") << "#12345";
    QTest::newRow("not hex") << "#gggggg";
    QTest::newRow("no hash") << "ff0000";
    QTest::newRow("long") << "#ff00001";
    // A regex `$` also matches before a final newline; the check must not.
    QTest::newRow("trailing newline") << "#ff0000\n";
}

void LimgenTests::palette_describeInvalidInput() {
    QFETCH(QString, hex);
    QCOMPARE(Palette::describe(hex), QString());
}

void LimgenTests::palette_wordsDeduplicatedInOrder() {
    // Two different greys with the same name: the name is in `words` once, at
    // its first position, while both colours stay in `colors`.
    const Palette palette = Palette::fromToml(tomlWith("#101010", "#d8dee9", "#d9dfea"));
    QVERIFY(palette.isValid());
    QCOMPARE(palette.colors.size(), 3);
    QCOMPARE(palette.colors[1].words, palette.colors[2].words);
    QCOMPARE(palette.words, (QStringList{palette.colors[0].words, palette.colors[1].words}));

    const Palette stock = Palette::fromToml(stockToml);
    QStringList expected;
    for (const PaletteColor &colour : stock.colors)
        if (!expected.contains(colour.words))
            expected << colour.words;
    QCOMPARE(stock.words, expected);
    QCOMPARE(stock.words.first(), QString("near-black indigo"));
}

void LimgenTests::palette_labOfWhiteAndBlack() {
    const Palette palette = Palette::fromToml(tomlWith("#ffffff", "#000000", "#ff0000"));
    QVERIFY(palette.isValid());
    const PaletteColor &white = palette.colors[0], &black = palette.colors[1], &red = palette.colors[2];
    QVERIFY(qAbs(white.lab[0] - 100) < 0.01);
    QVERIFY(qAbs(white.lab[1]) < 0.01);
    QVERIFY(qAbs(white.lab[2]) < 0.01);
    QVERIFY(qAbs(black.lab[0]) < 0.01);
    QVERIFY(qAbs(black.lab[1]) < 0.01);
    QVERIFY(qAbs(black.lab[2]) < 0.01);
    // Well-known CIELAB D65 value of pure sRGB red.
    QVERIFY2(qAbs(red.lab[0] - 53.24) < 0.1 && qAbs(red.lab[1] - 80.09) < 0.1 && qAbs(red.lab[2] - 67.20) < 0.1,
             qPrintable(QString("%1 %2 %3").arg(red.lab[0]).arg(red.lab[1]).arg(red.lab[2])));
}

// ============================== Prompt ===================================

namespace {
Palette handPalette() {
    Palette palette;
    palette.mode = "dark";
    palette.mood = Palette::moodFor("dark");
    palette.words = {"near-black indigo", "muted lavender", "blue"};
    return palette;
}
} // namespace

void LimgenTests::prompt_cinematicMatchesBenchmarkFormat() {
    const Prompt::Text text = Prompt::build("a lighthouse on a cliff", handPalette(), Prompt::Style::Cinematic);
    // The format bench/run uses, so the L0 results describe this default.
    QCOMPARE(text.positive,
             QString("a lighthouse on a cliff, cinematic wide shot, near-black indigo, muted lavender, blue "
                     "colour palette, night, low-key lighting, deep shadows, atmospheric, highly detailed, "
                     "desktop wallpaper"));
    QCOMPARE(Prompt::defaultStyle, Prompt::Style::Cinematic);
    QCOMPARE(Prompt::build("a lighthouse on a cliff", handPalette()).positive, text.positive);
}

void LimgenTests::prompt_invalidPaletteLeavesOutPaletteAndMood() {
    Palette palette = handPalette();
    palette.error = "no theme";
    const QString positive = Prompt::build("a fox", palette).positive;
    QCOMPARE(positive, QString("a fox, cinematic wide shot, atmospheric, highly detailed, desktop wallpaper"));
    QVERIFY(!positive.contains("colour palette"));
    QVERIFY(!positive.contains("low-key"));
}

void LimgenTests::prompt_defaultPaletteLeavesOutPaletteAndMood() {
    const QString positive = Prompt::build("a fox", Palette{}).positive;
    QCOMPARE(positive, QString("a fox, cinematic wide shot, atmospheric, highly detailed, desktop wallpaper"));
}

void LimgenTests::prompt_emptySubjectStartsWithStyleLead() {
    for (const QString &subject : {QString(), QString("   "), QString("\t\n")}) {
        const QString positive = Prompt::build(subject, handPalette()).positive;
        QVERIFY2(positive.startsWith("cinematic wide shot, near-black indigo"), qPrintable(positive));
        QVERIFY(!positive.startsWith(","));
        QVERIFY(!positive.contains(", ,"));
    }
}

void LimgenTests::prompt_subjectWhitespaceSimplified() {
    const QString positive = Prompt::build("  a  quiet\n harbour\tat dawn  ", handPalette()).positive;
    QVERIFY2(positive.startsWith("a quiet harbour at dawn, cinematic wide shot, "), qPrintable(positive));
}

void LimgenTests::prompt_styleIdsRoundTrip() {
    const QStringList ids = Prompt::styleIds();
    QCOMPARE(ids, (QStringList{"photo", "cinematic", "painting", "minimal"}));
    for (const QString &id : ids) {
        Prompt::Style style = Prompt::Style::Minimal;
        // Start from a style that is not the answer for the first three.
        if (id == "minimal")
            style = Prompt::Style::Photo;
        QVERIFY2(Prompt::styleFromId(id, &style), qPrintable(id));
        QCOMPARE(Prompt::styleId(style), id);
    }
    QCOMPARE(Prompt::styleId(Prompt::Style::Photo), QString("photo"));
    QCOMPARE(Prompt::styleId(Prompt::Style::Painting), QString("painting"));
}

void LimgenTests::prompt_unknownStyleIdLeavesStyleUntouched() {
    for (const QString &id : {QString("nope"), QString(), QString("Photo"), QString("photo ")}) {
        Prompt::Style style = Prompt::Style::Painting;
        QVERIFY2(!Prompt::styleFromId(id, &style), qPrintable(id));
        QCOMPARE(style, Prompt::Style::Painting);
    }
}

void LimgenTests::prompt_negativesHaveSharedListAndStyleExtras_data() {
    QTest::addColumn<int>("style");
    QTest::addColumn<QString>("extra");
    QTest::newRow("photo") << int(Prompt::Style::Photo) << "illustration";
    QTest::newRow("cinematic") << int(Prompt::Style::Cinematic) << "";
    QTest::newRow("painting") << int(Prompt::Style::Painting) << "photograph";
    QTest::newRow("minimal") << int(Prompt::Style::Minimal) << "clutter";
}

void LimgenTests::prompt_negativesHaveSharedListAndStyleExtras() {
    QFETCH(int, style);
    QFETCH(QString, extra);
    const QString negative = Prompt::build("x", handPalette(), Prompt::Style(style)).negative;
    for (const QString &word : QStringList{"text", "watermark", "signature", "logo", "blurry", "lowres", "jpeg artifacts"})
        QVERIFY2(negative.contains(word), qPrintable(negative));
    if (extra.isEmpty())
        QVERIFY2(!negative.endsWith(",") && !negative.endsWith(", "), qPrintable(negative));
    else
        QVERIFY2(negative.contains(extra), qPrintable(negative));
}

void LimgenTests::prompt_positivesDifferPerStyle() {
    QSet<QString> seen;
    for (const QString &id : Prompt::styleIds()) {
        Prompt::Style style;
        QVERIFY(Prompt::styleFromId(id, &style));
        seen.insert(Prompt::build("x", handPalette(), style).positive);
    }
    QCOMPARE(seen.size(), 4);
    // The palette and mood sit in every style.
    for (const QString &positive : seen) {
        QVERIFY(positive.contains("near-black indigo, muted lavender, blue colour palette"));
        QVERIFY(positive.endsWith(", desktop wallpaper"));
    }
}

// ============================== Omarchy ==================================

void LimgenTests::omarchy_slugFromThemeName() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeFile(fixture.state + "/theme.name", "tokyo-night\n"));
    const Omarchy omarchy(fixture.paths());
    QCOMPARE(omarchy.slug(), QString("tokyo-night"));
    QVERIFY(omarchy.present());

    QVERIFY(writeFile(fixture.state + "/theme.name", "  Catppuccin_2.0  \r\n"));
    QCOMPARE(omarchy.slug(), QString("Catppuccin_2.0"));
}

void LimgenTests::omarchy_slugUsesFirstLineOnly() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeFile(fixture.state + "/theme.name", "gruvbox\n../../etc\n"));
    const Omarchy omarchy(fixture.paths());
    QCOMPARE(omarchy.slug(), QString("gruvbox"));
}

void LimgenTests::omarchy_unsafeSlugsAreRejected_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("parent traversal") << "../x";
    QTest::newRow("slash") << "a/b";
    QTest::newRow("leading slash") << "/etc";
    QTest::newRow("hidden") << ".hidden";
    QTest::newRow("dot dot") << "..";
    QTest::newRow("dot") << ".";
    QTest::newRow("empty") << "";
    QTest::newRow("blank") << "   ";
    QTest::newRow("space inside") << "x y";
    QTest::newRow("dot dot inside") << "a..b";
    QTest::newRow("short hidden name") << ".a";
    QTest::newRow("backslash") << "a\\b";
    QTest::newRow("unicode") << QString::fromUtf8("th\xc3\xa8me");
}

void LimgenTests::omarchy_unsafeSlugsAreRejected() {
    QFETCH(QString, name);
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeFile(fixture.state + "/theme.name", name.toUtf8() + "\n"));
    const Omarchy omarchy(fixture.paths());
    QCOMPARE(omarchy.slug(), QString());
    QVERIFY(!omarchy.present());
    // The slug is part of a path limgen writes to: no folder must come out.
    QCOMPARE(omarchy.userBackgroundDirectory(), QString());
    QVERIFY(!Omarchy::isSafeSlug(name));
}

void LimgenTests::omarchy_noThemeNameFile() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Omarchy omarchy(fixture.paths());
    QCOMPARE(omarchy.slug(), QString());
    QVERIFY(!omarchy.present());
    QVERIFY(Omarchy::isSafeSlug("a-b_c.1"));
}

// slug() trims what it reads, but isSafeSlug guards a path limgen writes to,
// so it must not accept a name with a newline, where a regex `$` would match.
void LimgenTests::omarchy_slugCheckIsAnchoredAtTheEnd() {
    QVERIFY(!Omarchy::isSafeSlug("abc\n"));
    QVERIFY(Omarchy::isSafeSlug("abc"));
}

void LimgenTests::omarchy_readColorsMissingFileSetsError() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Omarchy omarchy(fixture.paths());
    QString error;
    QCOMPARE(omarchy.readColors(&error), QString());
    QVERIFY(!error.isEmpty());
    QVERIFY2(error.contains("colors.toml"), qPrintable(error));
    // A null error pointer must be accepted.
    QCOMPARE(omarchy.readColors(nullptr), QString());
}

void LimgenTests::omarchy_readColorsReturnsText() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    const Omarchy omarchy(fixture.paths());
    QString error;
    const QString text = omarchy.readColors(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(text, QString(stockToml));
    QCOMPARE(omarchy.colorsPath(), fixture.state + "/theme/colors.toml");
}

namespace {
void touchAll(const QString &directory, const QStringList &names) {
    for (const QString &name : names)
        QVERIFY(writeFile(directory + "/" + name, "x"));
}
const QStringList imageFixtureNames{"b.png", "a.JPG", "c.jpeg", "d.WebP", "e.PNG", "notes.txt", "f.gif", "g.png.part",
                                    "noext", "h.svg"};
const QStringList imageFixtureWanted{"a.JPG", "b.png", "c.jpeg", "d.WebP", "e.PNG"};
} // namespace

void LimgenTests::omarchy_themeBackgroundsFiltersAndSorts() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    touchAll(fixture.themes + "/night-city/backgrounds", imageFixtureNames);
    QDir().mkdir(fixture.themes + "/night-city/backgrounds/sub.png"); // a folder is not an image
    const Omarchy omarchy(fixture.paths());
    QStringList expected;
    for (const QString &name : imageFixtureWanted)
        expected << fixture.state + "/theme/backgrounds/" + name;
    QCOMPARE(omarchy.themeBackgrounds(), expected);
    for (const QString &path : omarchy.themeBackgrounds())
        QVERIFY(QFileInfo(path).isAbsolute());
}

void LimgenTests::omarchy_userBackgroundsFiltersAndSorts() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    touchAll(fixture.backgrounds + "/night-city", imageFixtureNames);
    // Another theme's folder must not leak in.
    touchAll(fixture.backgrounds + "/other", {"z.png"});
    const Omarchy omarchy(fixture.paths());
    QStringList expected;
    for (const QString &name : imageFixtureWanted)
        expected << fixture.backgrounds + "/night-city/" + name;
    QCOMPARE(omarchy.userBackgrounds(), expected);
}

void LimgenTests::omarchy_userBackgroundDirectory() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    {
        const Omarchy none(fixture.paths());
        QCOMPARE(none.userBackgroundDirectory(), QString());
        QCOMPARE(none.userBackgrounds(), QStringList());
        QCOMPARE(none.themeBackgrounds(), QStringList());
    }
    writeStockTheme(fixture);
    const Omarchy omarchy(fixture.paths());
    QCOMPARE(omarchy.userBackgroundDirectory(), fixture.backgrounds + "/night-city");
    // The folder does not exist yet; listing it is empty, and nothing is created.
    QCOMPARE(omarchy.userBackgrounds(), QStringList());
    QVERIFY(!QFileInfo::exists(fixture.backgrounds + "/night-city"));
}

void LimgenTests::omarchy_parseMonitorsSkipsDisabledAndEmpty() {
    const QByteArray json = R"([
      {"name":"DP-1","width":2560,"height":1440,"transform":0,"disabled":false},
      {"name":"HDMI-A-1","width":1920,"height":1080,"disabled":true},
      {"name":"eDP-1","width":0,"height":0},
      {"name":"DP-2","width":-5,"height":100},
      {"name":"DP-3","width":1920,"height":0},
      {"name":"DP-4","width":1920,"height":1080}
    ])";
    const QList<Monitor> monitors = Omarchy::parseMonitors(json);
    QCOMPARE(monitors.size(), 2);
    QCOMPARE(monitors[0].name, QString("DP-1"));
    QCOMPARE(monitors[0].width, 2560);
    QCOMPARE(monitors[0].height, 1440);
    QCOMPARE(monitors[1].name, QString("DP-4"));
}

void LimgenTests::omarchy_parseMonitorsTransformSwapsSize_data() {
    QTest::addColumn<int>("transform");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::newRow("normal") << 0 << 2560 << 1440;
    QTest::newRow("90") << 1 << 1440 << 2560;
    QTest::newRow("180") << 2 << 2560 << 1440;
    QTest::newRow("270") << 3 << 1440 << 2560;
    QTest::newRow("flipped") << 4 << 2560 << 1440;
    QTest::newRow("flipped 90") << 5 << 1440 << 2560;
    QTest::newRow("flipped 180") << 6 << 2560 << 1440;
    QTest::newRow("flipped 270") << 7 << 1440 << 2560;
}

void LimgenTests::omarchy_parseMonitorsTransformSwapsSize() {
    QFETCH(int, transform);
    QFETCH(int, width);
    QFETCH(int, height);
    const QByteArray json = QString(R"([{"name":"DP-1","width":2560,"height":1440,"transform":%1}])")
                                .arg(transform).toUtf8();
    const QList<Monitor> monitors = Omarchy::parseMonitors(json);
    QCOMPARE(monitors.size(), 1);
    QCOMPARE(monitors[0].width, width);
    QCOMPARE(monitors[0].height, height);
}

void LimgenTests::omarchy_parseMonitorsBadJson_data() {
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("garbage") << QByteArray("not json at all");
    QTest::newRow("object not array") << QByteArray(R"({"name":"DP-1","width":1,"height":1})");
    QTest::newRow("truncated") << QByteArray(R"([{"name":"DP-1","width":1920,"hei)");
    QTest::newRow("empty array") << QByteArray("[]");
    QTest::newRow("array of numbers") << QByteArray("[1,2,3]");
}

void LimgenTests::omarchy_parseMonitorsBadJson() {
    QFETCH(QByteArray, json);
    QVERIFY(Omarchy::parseMonitors(json).isEmpty());
}

void LimgenTests::omarchy_largestPicksMostPixels() {
    // 1920x1080 = 2073600; 1440x2560 = 3686400; 2560x1440 = 3686400 (a tie
    // keeps the first); 3840x1080 = 4147200 wins.
    QList<Monitor> monitors{{"a", 1920, 1080}, {"b", 1440, 2560}, {"c", 2560, 1440}};
    QCOMPARE(Omarchy::largest(monitors).name, QString("b"));
    monitors << Monitor{"d", 3840, 1080};
    QCOMPARE(Omarchy::largest(monitors).name, QString("d"));
    // Wide but few pixels does not win on width alone.
    QCOMPARE(Omarchy::largest({{"wide", 4000, 100}, {"tall", 1000, 1000}}).name, QString("tall"));
    const Monitor none = Omarchy::largest({});
    QCOMPARE(none.width, 0);
    QCOMPARE(none.height, 0);
    QVERIFY(none.name.isEmpty());
}

void LimgenTests::omarchy_monitorsRunsHyprctl() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/hyprctl.log";
    QVERIFY(writeScript(fixture.root.path() + "/hyprctl",
                        "echo \"$@\" > '" + log.toUtf8() + "'\n"
                        "echo '[{\"name\":\"DP-1\",\"width\":3840,\"height\":2160,\"transform\":0},"
                        "{\"name\":\"DP-2\",\"width\":1080,\"height\":1920,\"transform\":1}]'\n"));
    OmarchyPaths paths = fixture.paths();
    paths.hyprctl = fixture.root.path() + "/hyprctl";
    const Omarchy omarchy(paths);
    const QList<Monitor> monitors = omarchy.monitors();
    QCOMPARE(monitors.size(), 2);
    QCOMPARE(monitors[0].name, QString("DP-1"));
    QCOMPARE(monitors[1].width, 1920); // rotated
    QCOMPARE(monitors[1].height, 1080);
    QCOMPARE(QString(readFile(log)).trimmed(), QString("monitors -j"));
    QCOMPARE(Omarchy::largest(monitors).name, QString("DP-1"));
}

void LimgenTests::omarchy_monitorsEmptyWithoutHyprctl() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    OmarchyPaths paths = fixture.paths();
    QVERIFY(paths.hyprctl.isEmpty());
    QVERIFY(Omarchy(paths).monitors().isEmpty());
    paths.hyprctl = fixture.root.path() + "/does-not-exist";
    QVERIFY(Omarchy(paths).monitors().isEmpty());
}

void LimgenTests::omarchy_monitorsEmptyWhenHyprctlFails() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    // Valid JSON on stdout, but a non-zero exit: the answer must not be used.
    QVERIFY(writeScript(fixture.root.path() + "/hyprctl",
                        "echo '[{\"name\":\"DP-1\",\"width\":3840,\"height\":2160}]'\nexit 1\n"));
    OmarchyPaths paths = fixture.paths();
    paths.hyprctl = fixture.root.path() + "/hyprctl";
    QVERIFY(Omarchy(paths).monitors().isEmpty());
}

void LimgenTests::omarchy_monitorsGivesUpOnSlowHyprctl() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    // exec, so killing the script kills the sleep too and nothing lingers.
    QVERIFY(writeScript(fixture.root.path() + "/hyprctl", "exec sleep 30\n"));
    OmarchyPaths paths = fixture.paths();
    paths.hyprctl = fixture.root.path() + "/hyprctl";
    QElapsedTimer timer;
    timer.start();
    QVERIFY(Omarchy(paths).monitors().isEmpty());
    QVERIFY2(timer.elapsed() >= 1900, qPrintable(QString::number(timer.elapsed()))); // it did wait for the limit
    QVERIFY2(timer.elapsed() < 6000, qPrintable(QString::number(timer.elapsed())));
}

void LimgenTests::omarchy_changedFiresOnThemeSwitchAndAgain() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(fixture.addTheme("alpha", tomlWith("#111111", "#eeeeee", "#ff0000")));
    QVERIFY(fixture.addTheme("beta", tomlWith("#222222", "#dddddd", "#00ff00")));
    QVERIFY(fixture.addTheme("gamma", tomlWith("#333333", "#cccccc", "#0000ff")));
    QVERIFY(fixture.switchTo("alpha"));

    Omarchy omarchy(fixture.paths());
    QCOMPARE(omarchy.slug(), QString("alpha"));
    QSignalSpy spy(&omarchy, &Omarchy::changed);

    // First switch: theme.name and the theme link are both replaced by rename.
    QVERIFY(fixture.switchTo("beta"));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(omarchy.slug(), QString("beta"));
    QTest::qWait(500); // let the debounce settle so the next count is clean
    const int afterFirst = spy.count();

    // The watcher must have been re-armed on the new theme folder: editing
    // beta's colors.toml in place is only seen through the new watch.
    QVERIFY(writeFile(fixture.themes + "/beta/colors.toml", tomlWith("#222222", "#dddddd", "#ff8800")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > afterFirst, 3000);
    QTest::qWait(500);
    const int afterEdit = spy.count();

    // A second switch is seen too.
    QVERIFY(fixture.switchTo("gamma"));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > afterEdit, 3000);
    QCOMPARE(omarchy.slug(), QString("gamma"));
    QTest::qWait(500);
    const int afterSecond = spy.count();

    // And editing the colours of the third theme in place, through the
    // watch armed after the second switch.
    QVERIFY(writeFile(fixture.themes + "/gamma/colors.toml", tomlWith("#333333", "#cccccc", "#123456")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > afterSecond, 3000);
}

void LimgenTests::omarchy_changedFiresWhenCreatedLater() {
    // Omarchy installed after limgen started: the state folder does not exist
    // yet, and the watcher sits on the nearest ancestor that does.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    OmarchyPaths paths;
    paths.state = root.path() + "/state/omarchy/current";
    paths.backgrounds = root.path() + "/bg";
    QDir().mkpath(root.path() + "/state");
    Omarchy omarchy(paths);
    QSignalSpy spy(&omarchy, &Omarchy::changed);
    QVERIFY(writeFile(paths.state + "/theme.name", "late\n"));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(omarchy.slug(), QString("late"));
}

void LimgenTests::omarchy_pathsFromEnvironment() {
    EnvGuard guard({"LIMGEN_OMARCHY_STATE", "LIMGEN_OMARCHY_BACKGROUNDS", "LIMGEN_OMARCHY_BIN", "LIMGEN_HYPRCTL",
                    "XDG_CONFIG_HOME", "XDG_STATE_HOME", "HYPRLAND_INSTANCE_SIGNATURE"});
    qputenv("LIMGEN_OMARCHY_STATE", "/tmp/limgen-test/state");
    qputenv("LIMGEN_OMARCHY_BACKGROUNDS", "/tmp/limgen-test/backgrounds");
    qputenv("LIMGEN_OMARCHY_BIN", "/tmp/limgen-test/omarchy");
    qputenv("LIMGEN_HYPRCTL", "/tmp/limgen-test/hyprctl");
    qputenv("XDG_CONFIG_HOME", "/tmp/limgen-test/config");
    qputenv("XDG_STATE_HOME", "/tmp/limgen-test/xdgstate");
    const OmarchyPaths paths = OmarchyPaths::fromEnvironment();
    QCOMPARE(paths.state, QString("/tmp/limgen-test/state"));
    QCOMPARE(paths.backgrounds, QString("/tmp/limgen-test/backgrounds"));
    QCOMPARE(paths.omarchy, QString("/tmp/limgen-test/omarchy"));
    QCOMPARE(paths.hyprctl, QString("/tmp/limgen-test/hyprctl"));
    // The language file only follows XDG_CONFIG_HOME.
    QCOMPARE(paths.language, QString("/tmp/limgen-test/config/omarchy/language"));
}

void LimgenTests::omarchy_pathsFromXdgWhenNoOverrides() {
    EnvGuard guard({"LIMGEN_OMARCHY_STATE", "LIMGEN_OMARCHY_BACKGROUNDS", "LIMGEN_OMARCHY_BIN", "LIMGEN_HYPRCTL",
                    "XDG_CONFIG_HOME", "XDG_STATE_HOME", "HYPRLAND_INSTANCE_SIGNATURE"});
    qputenv("XDG_CONFIG_HOME", "/tmp/limgen-test/config");
    qputenv("XDG_STATE_HOME", "/tmp/limgen-test/xdgstate");
    const OmarchyPaths paths = OmarchyPaths::fromEnvironment();
    QCOMPARE(paths.state, QString("/tmp/limgen-test/xdgstate/omarchy/current"));
    QCOMPARE(paths.backgrounds, QString("/tmp/limgen-test/config/omarchy/backgrounds"));
    QCOMPARE(paths.language, QString("/tmp/limgen-test/config/omarchy/language"));
    // No Hyprland running and no override: hyprctl is not used even if installed.
    QVERIFY(paths.hyprctl.isEmpty());
}

void LimgenTests::omarchy_pathsEmptyOverrideIsIgnored() {
    EnvGuard guard({"LIMGEN_OMARCHY_STATE", "LIMGEN_OMARCHY_BACKGROUNDS", "LIMGEN_OMARCHY_BIN", "LIMGEN_HYPRCTL",
                    "XDG_CONFIG_HOME", "XDG_STATE_HOME", "HYPRLAND_INSTANCE_SIGNATURE"});
    qputenv("XDG_CONFIG_HOME", "/tmp/limgen-test/config");
    qputenv("XDG_STATE_HOME", "/tmp/limgen-test/xdgstate");
    qputenv("LIMGEN_OMARCHY_STATE", "");
    qputenv("LIMGEN_OMARCHY_BACKGROUNDS", "");
    const OmarchyPaths paths = OmarchyPaths::fromEnvironment();
    QCOMPARE(paths.state, QString("/tmp/limgen-test/xdgstate/omarchy/current"));
    QCOMPARE(paths.backgrounds, QString("/tmp/limgen-test/config/omarchy/backgrounds"));
}

// ============================== Backend ==================================

void LimgenTests::backend_withThemeHasSwatchesAndNoStatus() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    Omarchy omarchy(fixture.paths());
    Backend backend(&omarchy);
    QCOMPARE(backend.themeName(), QString("night-city"));
    QCOMPARE(backend.status(), QString());
    const QVariantList swatches = backend.swatches();
    QCOMPARE(swatches.size(), 5);
    const QVariantMap first = swatches[0].toMap(), second = swatches[1].toMap(), third = swatches[2].toMap();
    QCOMPARE(first["role"].toString(), QString("background"));
    QCOMPARE(first["hex"].toString(), QString("#1a1b26"));
    QCOMPARE(first["words"].toString(), QString("near-black indigo"));
    QCOMPARE(second["role"].toString(), QString("foreground"));
    QCOMPARE(second["hex"].toString(), QString("#a9b1d6"));
    QCOMPARE(second["words"].toString(), QString("muted lavender"));
    QCOMPARE(third["role"].toString(), QString("accent"));
    QCOMPARE(third["words"].toString(), QString("blue"));
    QCOMPARE(backend.mood(), Palette::moodFor("dark"));
    QVERIFY(backend.paletteWords().startsWith("near-black indigo, muted lavender, blue"));
    QCOMPARE(backend.paletteWords(), backend.palette().words.join(", "));
}

void LimgenTests::backend_withoutThemeHasStatusAndNoSwatches() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Omarchy omarchy(fixture.paths());
    Backend backend(&omarchy);
    QCOMPARE(backend.themeName(), QString());
    QVERIFY(backend.swatches().isEmpty());
    QVERIFY(!backend.status().isEmpty());
    QVERIFY2(backend.status().contains("theme"), qPrintable(backend.status()));
    QVERIFY2(backend.status().contains(fixture.state), qPrintable(backend.status()));
    QCOMPARE(backend.paletteWords(), QString());
    QCOMPARE(backend.mood(), QString());
    QVERIFY(!backend.palette().isValid());
}

void LimgenTests::backend_brokenColorsPutsPaletteErrorInStatus() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture, "background = \"#000000\"\nforeground = \"#ffffff\"\n");
    Omarchy omarchy(fixture.paths());
    Backend backend(&omarchy);
    QCOMPARE(backend.themeName(), QString("night-city"));
    QVERIFY(!backend.status().isEmpty());
    QVERIFY2(backend.status().contains("accent"), qPrintable(backend.status()));
    QVERIFY(backend.swatches().isEmpty());
    QCOMPARE(backend.mood(), QString());
}

void LimgenTests::backend_missingColorsFileGivesStatus() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeFile(fixture.state + "/theme.name", "bare\n"));
    Omarchy omarchy(fixture.paths());
    Backend backend(&omarchy);
    QCOMPARE(backend.themeName(), QString("bare"));
    QVERIFY2(backend.status().contains("colors.toml"), qPrintable(backend.status()));
    QVERIFY(backend.swatches().isEmpty());
}

void LimgenTests::backend_paletteChangedAfterThemeSwitch() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(fixture.addTheme("alpha", tomlWith("#111111", "#eeeeee", "#ff0000")));
    QVERIFY(fixture.addTheme("beta", tomlWith("#fafafa", "#222222", "#0000ff")));
    QVERIFY(fixture.switchTo("alpha"));
    Omarchy omarchy(fixture.paths());
    Backend backend(&omarchy);
    QCOMPARE(backend.themeName(), QString("alpha"));
    QCOMPARE(backend.palette().mode, QString("dark"));
    QSignalSpy spy(&backend, &Backend::paletteChanged);

    QVERIFY(fixture.switchTo("beta"));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(backend.themeName(), QString("beta"));
    QCOMPARE(backend.palette().mode, QString("light"));
    QCOMPARE(backend.swatches()[0].toMap()["hex"].toString(), QString("#fafafa"));
    QCOMPARE(backend.swatches()[2].toMap()["hex"].toString(), QString("#0000ff"));
    QCOMPARE(backend.status(), QString());
}

// ============================== CLI ======================================

QProcessEnvironment LimgenTests::cliEnvironment(const Fixture &fixture,
                                                const QHash<QString, QString> &extraEnvironment) const {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // The headless commands must not need a display, or a Hyprland session.
    for (const char *name : {"WAYLAND_DISPLAY", "DISPLAY", "QT_QPA_PLATFORM", "HYPRLAND_INSTANCE_SIGNATURE",
                             "LIMGEN_HYPRCTL", "LIMGEN_OMARCHY_BIN", "LIMGEN_SD_CLI",
                             "QT_QPA_PLATFORMTHEME", "LIMGEN_MODELS", "LIMGEN_NVIDIA_SMI", "LIMGEN_DRM_ROOT",
                             "FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS",
                             "LIMGEN_CATALOGUE", "LIMGEN_DOWNLOAD_BASE"})
        environment.remove(name);
    // Omarchy sessions export QT_QPA_PLATFORMTHEME=gtk3. It is removed by
    // default so the other tests do not depend on the session they run in;
    // cli_runsWithoutDisplayUnderGtkPlatformTheme puts it back.
    // Every download goes to a port where nothing listens unless a test points
    // LIMGEN_DOWNLOAD_BASE at its own server, so no CLI test can reach the
    // internet, whatever the code under test does.
    environment.insert("LIMGEN_DOWNLOAD_BASE", "http://127.0.0.1:1");
    for (auto it = extraEnvironment.begin(); it != extraEnvironment.end(); ++it)
        environment.insert(it.key(), it.value());
    const QString base = fixture.root.path();
    environment.insert("HOME", base + "/home");
    environment.insert("LIMGEN_OMARCHY_STATE", fixture.state);
    environment.insert("LIMGEN_OMARCHY_BACKGROUNDS", fixture.backgrounds);
    environment.insert("XDG_CONFIG_HOME", base + "/config");
    environment.insert("XDG_STATE_HOME", base + "/state");
    environment.insert("XDG_DATA_HOME", fixture.data);
    environment.insert("XDG_CACHE_HOME", fixture.cache);
    return environment;
}

LimgenTests::CliResult LimgenTests::runCli(const Fixture &fixture, const QStringList &arguments,
                                           const QHash<QString, QString> &extraEnvironment) const {
    CliResult result;
    const QProcessEnvironment environment = cliEnvironment(fixture, extraEnvironment);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(qEnvironmentVariable("LIMGEN_BINARY"), arguments);
    result.started = process.waitForStarted(5000);
    if (!result.started)
        return result;
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(2000);
        return result;
    }
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
    result.out = QString::fromUtf8(process.readAllStandardOutput());
    result.err = QString::fromUtf8(process.readAllStandardError());
    return result;
}

#define REQUIRE_BINARY() \
    if (qEnvironmentVariableIsEmpty("LIMGEN_BINARY")) \
        QSKIP("LIMGEN_BINARY is not set; run the suite through bin/test");

void LimgenTests::cli_paletteText() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    const CliResult result = runCli(fixture, {"palette"});
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.err, QString());
    const QStringList lines = result.out.split('\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.first(), QString("night-city (dark)"));
    QVERIFY2(lines[1].contains("background") && lines[1].contains("#1a1b26") && lines[1].contains("near-black indigo"),
             qPrintable(lines[1]));
    QVERIFY(lines[2].contains("foreground") && lines[2].contains("#a9b1d6"));
    QVERIFY(lines[3].contains("accent") && lines[3].contains("#7aa2f7"));
    QVERIFY2(result.out.contains("near-black indigo, muted lavender, blue"), qPrintable(result.out));
    QVERIFY(result.out.contains("night, low-key lighting, deep shadows"));
}

void LimgenTests::cli_paletteJson() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    const CliResult result = runCli(fixture, {"palette", "--json"});
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.err, QString());
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(result.out.toUtf8(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString() + ": " + result.out));
    const QJsonObject object = document.object();
    QCOMPARE(object["theme"].toString(), QString("night-city"));
    QCOMPARE(object["mode"].toString(), QString("dark"));
    QCOMPARE(object["mood"].toString(), Palette::moodFor("dark"));
    const Palette expected = Palette::fromToml(stockToml);
    QCOMPARE(object["words"].toArray().size(), expected.words.size());
    QCOMPARE(object["words"].toArray().first().toString(), QString("near-black indigo"));
    const QJsonArray colors = object["colors"].toArray();
    QCOMPARE(colors.size(), expected.colors.size());
    QCOMPARE(colors[0].toObject()["role"].toString(), QString("background"));
    QCOMPARE(colors[0].toObject()["hex"].toString(), QString("#1a1b26"));
    QCOMPARE(colors[0].toObject()["words"].toString(), QString("near-black indigo"));
    for (int i = 0; i < colors.size(); ++i) {
        const QJsonArray lab = colors[i].toObject()["lab"].toArray();
        QCOMPARE(lab.size(), 3);
        for (int k = 0; k < 3; ++k) {
            const double v = lab[k].toDouble();
            // Rounded to two decimals, and within that rounding of the real value.
            QVERIFY2(std::abs(v * 100 - std::round(v * 100)) < 1e-6, qPrintable(QString::number(v, 'f', 8)));
            QVERIFY2(std::abs(v - expected.colors[i].lab[k]) <= 0.0051, qPrintable(QString::number(v)));
        }
    }
}

void LimgenTests::cli_paletteWithoutThemeFails() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    for (const QStringList &arguments : {QStringList{"palette"}, QStringList{"palette", "--json"}}) {
        const CliResult result = runCli(fixture, arguments);
        QVERIFY(result.started);
        QCOMPARE(result.exitCode, 1);
        QCOMPARE(result.out, QString());
        QVERIFY2(result.err.contains("theme"), qPrintable(result.err));
    }
}

void LimgenTests::cli_paletteWithMissingColorsFails() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeFile(fixture.state + "/theme.name", "bare\n"));
    const CliResult result = runCli(fixture, {"palette"});
    QCOMPARE(result.exitCode, 1);
    QCOMPARE(result.out, QString());
    QVERIFY2(result.err.contains("colors.toml"), qPrintable(result.err));
}

void LimgenTests::cli_paletteWithBrokenColorsFails() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture, "background = \"#000000\"\nforeground = \"#ffffff\"\naccent = \"red\"\n");
    for (const QStringList &arguments : {QStringList{"palette"}, QStringList{"palette", "--json"}}) {
        const CliResult result = runCli(fixture, arguments);
        QCOMPARE(result.exitCode, 1);
        QCOMPARE(result.out, QString());
        QVERIFY2(result.err.contains("accent"), qPrintable(result.err));
        QVERIFY2(result.err.contains("colors.toml"), qPrintable(result.err));
    }
}

void LimgenTests::cli_paletteRejectsExtraArgumentAndBadOption() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    for (const QStringList &arguments : {QStringList{"palette", "extra"}, QStringList{"palette", "--bogus"},
                                         QStringList{"palette", "--json", "extra"}}) {
        const CliResult result = runCli(fixture, arguments);
        QVERIFY(result.started);
        QCOMPARE(result.exitCode, 2);
        QCOMPARE(result.out, QString()); // nothing on stdout, even with a valid theme
        QVERIFY2(!result.err.isEmpty(), qPrintable(arguments.join(' ')));
    }
}

void LimgenTests::cli_unknownCommandIsUsageError() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult result = runCli(fixture, {"frobnicate"});
    QCOMPARE(result.exitCode, 2);
    QCOMPARE(result.out, QString());
    QVERIFY2(result.err.contains("frobnicate"), qPrintable(result.err));
}

void LimgenTests::cli_helpCommands() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    for (const QStringList &arguments : {QStringList{"help"}, QStringList{"--help"}, QStringList{"-h"}}) {
        const CliResult result = runCli(fixture, arguments);
        QCOMPARE(result.exitCode, 0);
        QVERIFY2(result.out.contains("palette"), qPrintable(arguments.join(' ')));
        QCOMPARE(result.err, QString());
    }
    const CliResult topic = runCli(fixture, {"help", "palette"});
    QCOMPARE(topic.exitCode, 0);
    QVERIFY2(topic.out.contains("--json"), qPrintable(topic.out));
    const CliResult direct = runCli(fixture, {"palette", "--help"});
    QCOMPARE(direct.exitCode, 0);
    QVERIFY(direct.out.contains("--json"));
}

void LimgenTests::cli_helpForUnknownTopicIsUsageError() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult result = runCli(fixture, {"help", "bogus"});
    QCOMPARE(result.exitCode, 2);
    // A failed call says why on stderr and leaves stdout empty for scripts.
    QCOMPARE(result.out, QString());
    QVERIFY2(result.err.contains("bogus"), qPrintable(result.err));
}

// `open` with anything after it never opens the window, so it must answer
// like a command, without a display: help on --help, a usage error otherwise.
void LimgenTests::cli_openWithArgumentsNeedsNoDisplay() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult help = runCli(fixture, {"open", "--help"});
    QCOMPARE(help.exitCode, 0);
    QVERIFY2(help.out.contains("open"), qPrintable(help.out + help.err));
    const CliResult extra = runCli(fixture, {"open", "extra"});
    QCOMPARE(extra.exitCode, 2);
    QCOMPARE(extra.out, QString());
    QVERIFY2(extra.err.contains("extra"), qPrintable(extra.err));
}

void LimgenTests::cli_version() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult result = runCli(fixture, {"--version"});
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.out.trimmed(), QString("limgen 0.1.0"));
}

void LimgenTests::cli_runsWithoutDisplayUnderGtkPlatformTheme() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    // Omarchy exports QT_QPA_PLATFORMTHEME=gtk3. The GTK theme plugin then
    // opens a display on its own, whatever QT_QPA_PLATFORM says, so headless
    // commands die with "cannot open display" when there is none
    // (docs/technical.md section 9 says they must not need one).
    const CliResult result = runCli(fixture, {"palette", "--json"}, {{"QT_QPA_PLATFORMTHEME", "gtk3"}});
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(QJsonDocument::fromJson(result.out.toUtf8()).isObject());
}

// ======================= Theme and Translator ===========================

void LimgenTests::theme_fallbackAccentAndForeground() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Theme theme(fixture.state);
    QCOMPARE(theme.accent(), QString("#FFD60A"));
    QCOMPARE(theme.accentForeground(), QString("black"));
    QCOMPARE(theme.radius(), 0);
    QCOMPARE(Theme::readAccent(fixture.state + "/theme/colors.toml"), QString("#FFD60A"));
}

void LimgenTests::theme_readsAccentAndPicksForeground_data() {
    QTest::addColumn<QString>("toml");
    QTest::addColumn<QString>("accent");
    QTest::addColumn<QString>("foreground");
    QTest::newRow("dark blue gets white") << "accent = \"#1d4ed8\"\n" << "#1d4ed8" << "white";
    QTest::newRow("light yellow gets black") << "accent = \"#ffee00\"\n" << "#ffee00" << "black";
    QTest::newRow("single quotes") << "accent = '#7aa2f7'\n" << "#7aa2f7" << "black";
    QTest::newRow("amongst other keys") << "background = \"#000000\"\n  accent  =  \"#990000\"  \nred = \"#ff0000\"\n"
                                        << "#990000" << "white";
    QTest::newRow("malformed falls back") << "accent = \"blue\"\n" << "#FFD60A" << "black";
    QTest::newRow("short hex falls back") << "accent = \"#12345\"\n" << "#FFD60A" << "black";
    QTest::newRow("commented out falls back") << "# accent = \"#1d4ed8\"\n" << "#FFD60A" << "black";
    QTest::newRow("not the accent key") << "bright_accent = \"#1d4ed8\"\n" << "#FFD60A" << "black";
}

void LimgenTests::theme_readsAccentAndPicksForeground() {
    QFETCH(QString, toml);
    QFETCH(QString, accent);
    QFETCH(QString, foreground);
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeFile(fixture.state + "/theme/colors.toml", toml.toUtf8()));
    Theme theme(fixture.state);
    QCOMPARE(theme.accent(), accent);
    QCOMPARE(theme.accentForeground(), foreground);
}

void LimgenTests::theme_followsAccentChange() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(fixture.addTheme("alpha", "accent = \"#1d4ed8\"\n"));
    QVERIFY(fixture.addTheme("beta", "accent = \"#ffee00\"\n"));
    QVERIFY(fixture.addTheme("gamma", "accent = \"#7f1d1d\"\n"));
    QVERIFY(fixture.switchTo("alpha"));
    Theme theme(fixture.state);
    QCOMPARE(theme.accent(), QString("#1d4ed8"));
    QSignalSpy spy(&theme, &Theme::changed);
    QVERIFY(fixture.switchTo("beta"));
    QTRY_COMPARE_WITH_TIMEOUT(theme.accent(), QString("#ffee00"), 3000);
    QCOMPARE(theme.accentForeground(), QString("black"));
    QVERIFY(spy.count() >= 1);
    // The watcher re-armed: a second switch is followed too.
    QVERIFY(fixture.switchTo("gamma"));
    QTRY_COMPARE_WITH_TIMEOUT(theme.accent(), QString("#7f1d1d"), 3000);
    QCOMPARE(theme.accentForeground(), QString("white"));
}

void LimgenTests::theme_radiusFromHyprctl() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/hyprctl.log";
    QVERIFY(writeScript(fixture.root.path() + "/hyprctl",
                        "echo \"$@\" >> '" + log.toUtf8() + "'\n"
                        "echo '{\"option\":\"decoration:rounding\",\"int\":12,\"set\":true}'\n"));
    Theme theme(fixture.state, nullptr, fixture.root.path() + "/hyprctl");
    QSignalSpy spy(&theme, &Theme::changed);
    QTRY_COMPARE_WITH_TIMEOUT(theme.radius(), 12, 3000);
    QVERIFY(spy.count() >= 1);
    QVERIFY2(QString(readFile(log)).contains("-j getoption decoration:rounding"), qPrintable(readFile(log)));
}

void LimgenTests::theme_radiusStaysZeroWhenHyprctlFails() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/hyprctl.log";
    // Each failure mode answers a rounding that would show if it were used.
    QVERIFY(writeScript(fixture.root.path() + "/hyprctl",
                        "echo run >> '" + log.toUtf8() + "'\n"
                        "echo '{\"option\":\"decoration:rounding\",\"int\":12}'\nexit 1\n"));
    Theme theme(fixture.state, nullptr, fixture.root.path() + "/hyprctl");
    QTRY_VERIFY_WITH_TIMEOUT(readFile(log).contains("run"), 3000); // it did run
    QTest::qWait(300);
    QCOMPARE(theme.radius(), 0);

    QVERIFY(writeScript(fixture.root.path() + "/hyprctl-negative",
                        "echo '{\"option\":\"decoration:rounding\",\"int\":-3}'\n"));
    Theme negative(fixture.state, nullptr, fixture.root.path() + "/hyprctl-negative");
    QTest::qWait(500);
    QCOMPARE(negative.radius(), 0);

    QVERIFY(writeScript(fixture.root.path() + "/hyprctl-junk", "echo 'not json'\n"));
    Theme junk(fixture.state, nullptr, fixture.root.path() + "/hyprctl-junk");
    QTest::qWait(500);
    QCOMPARE(junk.radius(), 0);
}

namespace {
// A catalogue folder with one JSON file per language code.
struct Catalogs {
    QTemporaryDir dir;
    bool add(const QString &code, const QJsonObject &strings) {
        return writeFile(dir.path() + "/" + code + ".json", QJsonDocument(strings).toJson());
    }
    QString path() const { return dir.path(); }
};
QString translated(const Translator &translator, const char *text, int n = -1) {
    return translator.translate("Ctx", text, nullptr, n);
}
} // namespace

void LimgenTests::translator_englishFallback() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Catalogs catalogs;
    QVERIFY(catalogs.add("en", {{"Save", "Save it"}}));
    QVERIFY(catalogs.add("fr", {{"Open", "Ouvrir"}}));
    // No language file: English.
    const Translator english(fixture.root.path() + "/config/omarchy/language", catalogs.path());
    QCOMPARE(english.language(), QString("en"));
    QCOMPARE(translated(english, "Save"), QString("Save it"));
    QVERIFY(translated(english, "Open").isNull()); // null: Qt shows the source text
    QVERIFY(translated(english, "Unknown").isNull());

    // A language with no catalogue of its own falls back to English strings.
    QVERIFY(writeFile(fixture.root.path() + "/config/omarchy/language", "de\n"));
    const Translator german(fixture.root.path() + "/config/omarchy/language", catalogs.path());
    QCOMPARE(german.language(), QString("de"));
    QCOMPARE(translated(german, "Save"), QString("Save it"));

    // A language with a catalogue that lacks a string falls back too, and an
    // empty translation counts as missing.
    QVERIFY(catalogs.add("fr", {{"Open", "Ouvrir"}, {"Save", ""}}));
    QVERIFY(writeFile(fixture.root.path() + "/config/omarchy/language", "fr\n"));
    const Translator french(fixture.root.path() + "/config/omarchy/language", catalogs.path());
    QCOMPARE(translated(french, "Open"), QString("Ouvrir"));
    QCOMPARE(translated(french, "Save"), QString("Save it"));
}

void LimgenTests::translator_regionalFallsBackToBase() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Catalogs catalogs;
    QVERIFY(catalogs.add("en", {{"Save", "Save"}, {"Open", "Open"}, {"Quit", "Quit"}}));
    QVERIFY(catalogs.add("pt", {{"Save", "Guardar"}, {"Open", "Abrir"}}));
    QVERIFY(catalogs.add("pt-BR", {{"Save", "Salvar"}}));
    const QString languageFile = fixture.root.path() + "/config/omarchy/language";
    QVERIFY(writeFile(languageFile, "pt-BR\n"));
    const Translator translator(languageFile, catalogs.path());
    QCOMPARE(translator.language(), QString("pt-BR"));
    QCOMPARE(translated(translator, "Save"), QString("Salvar"));  // regional wins
    QCOMPARE(translated(translator, "Open"), QString("Abrir"));   // base language
    QCOMPARE(translated(translator, "Quit"), QString("Quit"));    // English
    QVERIFY(translated(translator, "Missing").isNull());
}

void LimgenTests::translator_pluralFormsRussian_data() {
    QTest::addColumn<int>("count");
    QTest::addColumn<QString>("expected");
    QTest::newRow("1") << 1 << "one";
    QTest::newRow("2") << 2 << "few";
    QTest::newRow("4") << 4 << "few";
    QTest::newRow("5") << 5 << "many";
    QTest::newRow("0") << 0 << "many";
    QTest::newRow("11") << 11 << "many";
    QTest::newRow("12") << 12 << "many";
    QTest::newRow("14") << 14 << "many";
    QTest::newRow("21") << 21 << "one";
    QTest::newRow("22") << 22 << "few";
    QTest::newRow("25") << 25 << "many";
    QTest::newRow("101") << 101 << "one";
    QTest::newRow("111") << 111 << "many";
}

void LimgenTests::translator_pluralFormsRussian() {
    QFETCH(int, count);
    QFETCH(QString, expected);
    QCOMPARE(Translator::pluralCategory("ru", count), expected);

    Fixture fixture;
    QVERIFY(fixture.ok());
    Catalogs catalogs;
    QVERIFY(catalogs.add("ru", {{"%n images", QJsonObject{{"one", "%n изображение"},
                                                           {"few", "%n изображения"},
                                                           {"many", "%n изображений"},
                                                           {"other", "%n изображения (прочее)"}}}}));
    const QString languageFile = fixture.root.path() + "/config/omarchy/language";
    QVERIFY(writeFile(languageFile, "ru\n"));
    const Translator translator(languageFile, catalogs.path());
    const QString form = QString::fromUtf8(expected == "one"   ? "%n изображение"
                                           : expected == "few" ? "%n изображения"
                                                               : "%n изображений");
    QCOMPARE(translated(translator, "%n images", count), form);
}

void LimgenTests::translator_pluralFallsBackToOther() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Catalogs catalogs;
    // A catalogue with no "few" form: Russian 2 falls back to "other".
    QVERIFY(catalogs.add("ru", {{"%n images", QJsonObject{{"one", "one-form"}, {"other", "other-form"}}}}));
    const QString languageFile = fixture.root.path() + "/config/omarchy/language";
    QVERIFY(writeFile(languageFile, "ru\n"));
    const Translator translator(languageFile, catalogs.path());
    QCOMPARE(translated(translator, "%n images", 2), QString("other-form"));
    QCOMPARE(translated(translator, "%n images", 1), QString("one-form"));
    // Without a count the "other" form is used.
    QCOMPARE(translated(translator, "%n images"), QString("other-form"));
    // English, French and Japanese rules for contrast.
    QCOMPARE(Translator::pluralCategory("en", 1), QString("one"));
    QCOMPARE(Translator::pluralCategory("en", 0), QString("other"));
    QCOMPARE(Translator::pluralCategory("fr", 0), QString("one"));
    QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    QCOMPARE(Translator::pluralCategory("pl", 5), QString("many"));
}

void LimgenTests::translator_readLanguageRejectsJunk() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString file = fixture.root.path() + "/language";
    QCOMPARE(Translator::readLanguage(file), QString("en")); // missing file
    const QList<QPair<QByteArray, QString>> cases{{"pt-BR\n", "pt-BR"}, {" da \n", "da"}, {"fil\n", "fil"},
                                                  {"../etc\n", "en"},   {"PT\n", "en"},   {"\n", "en"},
                                                  {"english\n", "en"},  {"pt_BR\n", "en"}, {"da\nfr\n", "da"}};
    for (const auto &entry : cases) {
        QVERIFY(writeFile(file, entry.first));
        QCOMPARE(Translator::readLanguage(file), entry.second);
    }
}

void LimgenTests::translator_followsLanguageFileChange() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Catalogs catalogs;
    QVERIFY(catalogs.add("en", {{"Save", "Save"}}));
    QVERIFY(catalogs.add("da", {{"Save", "Gem"}}));
    QVERIFY(catalogs.add("fr", {{"Save", "Enregistrer"}}));
    const QString languageFile = fixture.root.path() + "/config/omarchy/language";
    QVERIFY(writeFile(languageFile, "da\n"));
    Translator translator(languageFile, catalogs.path());
    QSignalSpy spy(&translator, &Translator::changed);
    QCOMPARE(translated(translator, "Save"), QString("Gem"));
    QVERIFY(replaceFile(languageFile, "fr\n"));
    QTRY_COMPARE_WITH_TIMEOUT(translator.language(), QString("fr"), 3000);
    QCOMPARE(translated(translator, "Save"), QString("Enregistrer"));
    QVERIFY(spy.count() >= 1);
    // Re-armed: a second replacement is followed.
    QVERIFY(replaceFile(languageFile, "da\n"));
    QTRY_COMPARE_WITH_TIMEOUT(translator.language(), QString("da"), 3000);
    QCOMPARE(translated(translator, "Save"), QString("Gem"));
}

// ============================== L2 helpers ================================

namespace {

const QString repoRoot = QStringLiteral(SOURCE_ROOT);
const QString fakeEngine = repoRoot + "/bin/fake-sd-cli";

QByteArray dataFile(const QString &name) { return readFile(repoRoot + "/tests/data/" + name); }

// A sparse file of the given size: models are only checked for existence and
// size, so nothing needs to be written.
bool makeSparse(const QString &path, qint64 bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.resize(bytes);
}

QString describeFile(const CatalogueFile &file) { return file.folder + "/" + file.name; }

// Installs every file of a model's first level, at the catalogue's size.
bool installModel(const Catalogue &catalogue, const QString &modelId, const QString &modelsDirectory) {
    const CatalogueModel *model = catalogue.model(modelId);
    if (!model)
        return false;
    for (const QString &key : model->levels.first().files) {
        const CatalogueFile *file = catalogue.file(key);
        if (!file || !makeSparse(Catalogue::path(*file, modelsDirectory), file->bytes))
            return false;
    }
    return true;
}

QJsonObject jsonObject(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

QString fakeNvidiaSmi(const Fixture &fixture, int total, int used, const QString &name = "Fake GPU",
                      const QString &argsLog = QString()) {
    const QString path = fixture.root.path() + "/nvidia-smi";
    QByteArray body;
    if (!argsLog.isEmpty())
        body += "echo \"$@\" >> '" + argsLog.toUtf8() + "'\n";
    body += "echo '" + name.toUtf8() + ", " + QByteArray::number(total) + ", " + QByteArray::number(used) + "'\n";
    writeScript(path, body);
    return path;
}

// The calls bin/fake-sd-cli logged, one argument list each.
QList<QStringList> fakeCalls(const QString &logPath) {
    QList<QStringList> calls;
    for (const QByteArray &line : readFile(logPath).split('\n')) {
        if (line.isEmpty())
            continue;
        QStringList arguments;
        for (const QJsonValue &value : QJsonDocument::fromJson(line).array())
            arguments << value.toString();
        calls << arguments;
    }
    return calls;
}

// Only the calls that make images, not --version and --list-devices.
QList<QStringList> imageCalls(const QString &logPath) {
    QList<QStringList> calls;
    for (const QStringList &call : fakeCalls(logPath))
        if (call.contains("--prompt"))
            calls << call;
    return calls;
}

QString valueAfter(const QStringList &arguments, const QString &option) {
    const int at = arguments.indexOf(option);
    return at < 0 ? QString() : arguments.value(at + 1);
}

QString eventText(const ProgressEvent &e) {
    return QString("%1 image=%2 step=%3/%4 s=%5 text=%6")
        .arg(int(e.kind)).arg(e.image).arg(e.step).arg(e.steps).arg(e.secondsPerStep, 0, 'f', 4).arg(e.text);
}

QList<ProgressEvent> parseInChunks(const QByteArray &data, int images, int steps, qsizetype chunk) {
    ProgressParser parser(images, steps);
    QList<ProgressEvent> events;
    for (qsizetype at = 0; at < data.size(); at += chunk)
        events += parser.feed(data.mid(at, chunk));
    events += parser.finish();
    return events;
}

QList<ProgressEvent> ofKind(const QList<ProgressEvent> &events, ProgressEvent::Kind kind) {
    QList<ProgressEvent> list;
    for (const ProgressEvent &e : events)
        if (e.kind == kind)
            list << e;
    return list;
}

QByteArray baseCatalogueJson() {
    QFile file(":/models.json");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A copy of the built-in catalogue with one change, for the validation tests.
QByteArray changedCatalogue(const std::function<void(QJsonObject &)> &change) {
    QJsonObject root = QJsonDocument::fromJson(baseCatalogueJson()).object();
    change(root);
    return QJsonDocument(root).toJson();
}

void setFileField(QJsonObject &root, const QString &key, const QString &field, const QJsonValue &value) {
    QJsonObject files = root["files"].toObject();
    QJsonObject file = files[key].toObject();
    file[field] = value;
    files[key] = file;
    root["files"] = files;
}

void setModelField(QJsonObject &root, int index, const QString &field, const QJsonValue &value) {
    QJsonArray models = root["models"].toArray();
    QJsonObject model = models[index].toObject();
    model[field] = value;
    models[index] = model;
    root["models"] = models;
}

void setLevelFiles(QJsonObject &root, const std::function<void(QJsonObject &)> &change) {
    QJsonArray models = root["models"].toArray();
    QJsonObject model = models[0].toObject();
    QJsonArray levels = model["levels"].toArray();
    QJsonObject level = levels[0].toObject();
    QJsonObject files = level["files"].toObject();
    change(files);
    level["files"] = files;
    levels[0] = level;
    model["levels"] = levels;
    models[0] = model;
    root["models"] = models;
}

} // namespace

// ============================== Catalogue ================================

void LimgenTests::catalogue_builtInIsValidAndPinned() {
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(catalogue.isValid());
    // The catalogue and the engine class must name the same release.
    QCOMPARE(catalogue.engine(), QString(SdCppEngine::pinnedVersion));
    QVERIFY(catalogue.models().size() >= 2);
    QVERIFY(catalogue.defaultModel() != nullptr);
    QCOMPARE(catalogue.defaultModel()->id, QString("z-image-turbo"));
    int defaults = 0;
    for (const CatalogueModel &model : catalogue.models())
        defaults += model.isDefault;
    QCOMPARE(defaults, 1);
    QVERIFY(catalogue.upscaler() != nullptr);
}

void LimgenTests::catalogue_builtInFilesAndLevelsAreConsistent() {
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(catalogue.isValid());
    for (const CatalogueModel &model : catalogue.models()) {
        QVERIFY2(!model.licenceId.isEmpty(), qPrintable(model.id)); // CLAUDE.md: licence recorded
        QVERIFY2(!model.licenceUrl.isEmpty(), qPrintable(model.id));
        QVERIFY2(model.width > 0 && model.height > 0 && model.steps > 0, qPrintable(model.id));
        QVERIFY(!model.levels.isEmpty());
        for (const ModelLevel &level : model.levels) {
            QVERIFY2(level.minFreeMiB > 0, qPrintable(model.id));
            for (auto it = level.files.begin(); it != level.files.end(); ++it) {
                QVERIFY2(Catalogue::roles.contains(it.key()), qPrintable(it.key()));
                const CatalogueFile *file = catalogue.file(it.value());
                QVERIFY2(file != nullptr, qPrintable(it.value()));
                QVERIFY(file->bytes > 0);
                QVERIFY2(QRegularExpression("^[0-9a-f]{64}$").match(file->sha256).hasMatch(), qPrintable(file->key));
                QVERIFY(file->url.startsWith("https://"));
                QVERIFY(!file->licence.isEmpty());
            }
        }
    }
    // No two files share a place on disk.
    QSet<QString> places;
    for (const CatalogueModel &model : catalogue.models())
        for (const QString &key : model.levels.first().files)
            places.insert(describeFile(*catalogue.file(key)));
    QVERIFY(places.size() >= 4);
}

void LimgenTests::catalogue_loadAcceptsTheUnchangedCatalogue() {
    // The baseline for the rejection rows: the same bytes load fine.
    QString error;
    const Catalogue catalogue = Catalogue::load(changedCatalogue([](QJsonObject &) {}), &error);
    QVERIFY2(catalogue.isValid(), qPrintable(error));
    QVERIFY(error.isEmpty());
}

void LimgenTests::catalogue_loadRejectsBrokenCatalogues_data() {
    QTest::addColumn<QByteArray>("json");
    const QString zFile = "z-image-turbo-q4_k";
    QTest::newRow("not json") << QByteArray("{nope");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("no models") << changedCatalogue([](QJsonObject &r) { r["models"] = QJsonArray(); });
    QTest::newRow("no default") << changedCatalogue([](QJsonObject &r) { setModelField(r, 0, "default", false); });
    QTest::newRow("two defaults") << changedCatalogue([](QJsonObject &r) { setModelField(r, 1, "default", true); });
    QTest::newRow("duplicate model id") << changedCatalogue([](QJsonObject &r) {
        setModelField(r, 1, "id", r["models"].toArray()[0].toObject()["id"]);
    });
    QTest::newRow("short sha256") << changedCatalogue([=](QJsonObject &r) { setFileField(r, zFile, "sha256", "abc"); });
    QTest::newRow("http url") << changedCatalogue([=](QJsonObject &r) { setFileField(r, zFile, "url", "http://x/y"); });
    QTest::newRow("zero bytes") << changedCatalogue([=](QJsonObject &r) { setFileField(r, zFile, "bytes", 0); });
    QTest::newRow("folder traversal") << changedCatalogue([=](QJsonObject &r) { setFileField(r, zFile, "folder", "../x"); });
    QTest::newRow("name with slash") << changedCatalogue([=](QJsonObject &r) { setFileField(r, zFile, "name", "a/b"); });
    QTest::newRow("absolute folder") << changedCatalogue([=](QJsonObject &r) { setFileField(r, zFile, "folder", "/etc"); });
    QTest::newRow("no file licence") << changedCatalogue([=](QJsonObject &r) { setFileField(r, zFile, "licence", ""); });
    QTest::newRow("unknown role") << changedCatalogue([=](QJsonObject &r) {
        setLevelFiles(r, [=](QJsonObject &f) { f["bogus"] = zFile; });
    });
    QTest::newRow("unknown file key") << changedCatalogue([](QJsonObject &r) {
        setLevelFiles(r, [](QJsonObject &f) { f["diffusion-model"] = "nope"; });
    });
    QTest::newRow("whole and split") << changedCatalogue([=](QJsonObject &r) {
        setLevelFiles(r, [=](QJsonObject &f) { f["model"] = zFile; });
    });
    QTest::newRow("split without vae") << changedCatalogue([](QJsonObject &r) {
        setLevelFiles(r, [](QJsonObject &f) { f.remove("vae"); });
    });
    QTest::newRow("no levels") << changedCatalogue([](QJsonObject &r) { setModelField(r, 0, "levels", QJsonArray()); });
    QTest::newRow("no licence id") << changedCatalogue([](QJsonObject &r) { setModelField(r, 0, "licence", QJsonObject()); });
    QTest::newRow("no native size") << changedCatalogue([](QJsonObject &r) { setModelField(r, 0, "native", QJsonArray{0, 0}); });
    QTest::newRow("zero steps") << changedCatalogue([](QJsonObject &r) { setModelField(r, 0, "steps", 0); });
    QTest::newRow("unknown upscaler") << changedCatalogue([](QJsonObject &r) { r["upscaler"] = "nope"; });
}

void LimgenTests::catalogue_loadRejectsBrokenCatalogues() {
    QFETCH(QByteArray, json);
    QString error;
    const Catalogue catalogue = Catalogue::load(json, &error);
    QVERIFY(!catalogue.isValid());
    QVERIFY(!error.isEmpty());
    // A null error pointer is accepted.
    QVERIFY(!Catalogue::load(json, nullptr).isValid());
}

void LimgenTests::catalogue_lookups() {
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(catalogue.model("z-image-turbo") != nullptr);
    QVERIFY(catalogue.model("flux2-klein-4b") != nullptr);
    QVERIFY(catalogue.model("nope") == nullptr);
    QVERIFY(catalogue.model("") == nullptr);
    QVERIFY(catalogue.file("nope") == nullptr);
    QVERIFY(catalogue.file("flux1-vae") != nullptr);
    const CatalogueModel *z = catalogue.model("z-image-turbo");
    QCOMPARE(z->width, 1344);
    QCOMPARE(z->height, 768);
    QCOMPARE(z->steps, 8);
    QCOMPARE(z->cfg, 1.0);
    QCOMPARE(z->levels.first().files.value("vae"), QString("flux1-vae"));
    QCOMPARE(catalogue.model("flux2-klein-4b")->steps, 4);
}

void LimgenTests::catalogue_defaultModelsDirectory() {
    EnvGuard guard({"LIMGEN_MODELS", "XDG_DATA_HOME"});
    qputenv("XDG_DATA_HOME", "/tmp/limgen-test/xdgdata");
    QCOMPARE(Catalogue::defaultModelsDirectory(), QString("/tmp/limgen-test/xdgdata/limgen/models"));
    qputenv("LIMGEN_MODELS", "/tmp/limgen-test/models");
    QCOMPARE(Catalogue::defaultModelsDirectory(), QString("/tmp/limgen-test/models"));
    qputenv("LIMGEN_MODELS", ""); // empty counts as unset
    QCOMPARE(Catalogue::defaultModelsDirectory(), QString("/tmp/limgen-test/xdgdata/limgen/models"));
}

void LimgenTests::catalogue_pathAndState() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    const CatalogueFile &file = *catalogue.file("taef1"); // 9.8 MB, but still sparse here
    QCOMPARE(Catalogue::path(file, fixture.models), fixture.models + "/" + file.folder + "/" + file.name);
    QCOMPARE(Catalogue::state(file, fixture.models), Catalogue::FileState::Missing);
    const QString path = Catalogue::path(file, fixture.models);
    QVERIFY(makeSparse(path, file.bytes - 1));
    QCOMPARE(Catalogue::state(file, fixture.models), Catalogue::FileState::WrongSize);
    QVERIFY(makeSparse(path, file.bytes + 1));
    QCOMPARE(Catalogue::state(file, fixture.models), Catalogue::FileState::WrongSize);
    QVERIFY(makeSparse(path, file.bytes));
    QCOMPARE(Catalogue::state(file, fixture.models), Catalogue::FileState::Ok);
    // A folder where the file should be is missing, not installed.
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir().mkpath(path));
    QCOMPARE(Catalogue::state(file, fixture.models), Catalogue::FileState::Missing);
}

void LimgenTests::catalogue_stateVerifiesSha256() {
    // A small catalogue, so hashing is quick; "hello" has a well-known digest.
    const QByteArray json = R"({
      "engine": "x",
      "files": {
        "a": {"folder": "f", "name": "a.bin", "bytes": 5,
              "sha256": "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
              "url": "https://example.com/a", "licence": "MIT"},
        "b": {"folder": "f", "name": "b.bin", "bytes": 5,
              "sha256": "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
              "url": "https://example.com/b", "licence": "MIT"},
        "c": {"folder": "f", "name": "c.bin", "bytes": 5,
              "sha256": "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
              "url": "https://example.com/c", "licence": "MIT"}
      },
      "models": [{"id": "m", "default": true, "licence": {"id": "MIT"}, "native": [64, 64], "steps": 1,
                  "levels": [{"id": "q", "minFreeMiB": 1,
                              "files": {"diffusion-model": "a", "vae": "b", "llm": "c"}}]}]
    })";
    QString error;
    const Catalogue catalogue = Catalogue::load(json, &error);
    QVERIFY2(catalogue.isValid(), qPrintable(error));
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeFile(fixture.models + "/f/a.bin", "hello"));
    QVERIFY(writeFile(fixture.models + "/f/b.bin", "jello")); // right size, wrong content
    // c is missing.
    QCOMPARE(Catalogue::state(*catalogue.file("a"), fixture.models), Catalogue::FileState::Ok);
    QCOMPARE(Catalogue::state(*catalogue.file("a"), fixture.models, true), Catalogue::FileState::Ok);
    // Without verify the wrong content is not seen; with it, it is.
    QCOMPARE(Catalogue::state(*catalogue.file("b"), fixture.models), Catalogue::FileState::Ok);
    QCOMPARE(Catalogue::state(*catalogue.file("b"), fixture.models, true), Catalogue::FileState::Corrupt);
    QCOMPARE(Catalogue::state(*catalogue.file("c"), fixture.models, true), Catalogue::FileState::Missing);
    const ModelLevel &level = catalogue.model("m")->levels.first();
    QCOMPARE(catalogue.missingFiles(level, fixture.models), QStringList{"c"});
    QStringList corrupt = catalogue.missingFiles(level, fixture.models, true);
    corrupt.sort(); // the files come in role order, not key order
    QCOMPARE(corrupt, (QStringList{"b", "c"}));
}

void LimgenTests::catalogue_missingFiles() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    const ModelLevel &zLevel = catalogue.model("z-image-turbo")->levels.first();
    const ModelLevel &kLevel = catalogue.model("flux2-klein-4b")->levels.first();
    QCOMPARE(QSet<QString>(zLevel.files.begin(), zLevel.files.end()).size(), zLevel.files.size());
    QStringList all = catalogue.missingFiles(zLevel, fixture.models);
    QCOMPARE(QSet<QString>(all.begin(), all.end()), QSet<QString>(zLevel.files.begin(), zLevel.files.end()));
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models));
    QCOMPARE(catalogue.missingFiles(zLevel, fixture.models), QStringList());
    // The Qwen3 text encoder is shared, so only the klein-specific files are missing.
    const QStringList kMissing = catalogue.missingFiles(kLevel, fixture.models);
    QVERIFY(!kMissing.isEmpty());
    QVERIFY(!kMissing.contains(kLevel.files.value("llm")));
    QVERIFY(kMissing.contains(kLevel.files.value("diffusion-model")));
    QCOMPARE(kMissing.size(), QSet<QString>(kMissing.begin(), kMissing.end()).size());
    // A truncated file counts as missing.
    const CatalogueFile &vae = *catalogue.file(zLevel.files.value("vae"));
    QVERIFY(makeSparse(Catalogue::path(vae, fixture.models), vae.bytes - 1));
    QCOMPARE(catalogue.missingFiles(zLevel, fixture.models), QStringList{vae.key});
}

// ============================== Hardware =================================

void LimgenTests::hardware_parseNvidiaSmi() {
    const QList<Gpu> gpus = Hardware::parseNvidiaSmi("NVIDIA GeForce RTX 3070 Ti, 8192, 1500\n"
                                                      "NVIDIA RTX A2000, 6144, 6144\r\n");
    QCOMPARE(gpus.size(), 2);
    QCOMPARE(gpus[0].vendor, QString("nvidia"));
    QCOMPARE(gpus[0].name, QString("NVIDIA GeForce RTX 3070 Ti"));
    QCOMPARE(gpus[0].totalMiB, qint64(8192));
    QCOMPARE(gpus[0].freeMiB, qint64(6692));
    QCOMPARE(gpus[1].name, QString("NVIDIA RTX A2000"));
    QCOMPARE(gpus[1].freeMiB, qint64(0));
    QVERIFY(Hardware::parseNvidiaSmi("").isEmpty());
}

void LimgenTests::hardware_parseNvidiaSmiNameWithCommas() {
    const QList<Gpu> gpus = Hardware::parseNvidiaSmi("NVIDIA GeForce RTX 4070, Laptop GPU, 8188, 1500\n");
    QCOMPARE(gpus.size(), 1);
    QCOMPARE(gpus[0].name, QString("NVIDIA GeForce RTX 4070, Laptop GPU"));
    QCOMPARE(gpus[0].totalMiB, qint64(8188));
    QCOMPARE(gpus[0].freeMiB, qint64(6688));
}

void LimgenTests::hardware_parseNvidiaSmiSkipsJunk_data() {
    QTest::addColumn<QByteArray>("csv");
    QTest::newRow("no devices message") << QByteArray("No devices were found\n");
    QTest::newRow("driver error") << QByteArray("NVIDIA-SMI has failed because it couldn't communicate with the driver.\n");
    QTest::newRow("two fields") << QByteArray("GPU, 8192\n");
    QTest::newRow("text numbers") << QByteArray("GPU, lots, some\n");
    QTest::newRow("zero total") << QByteArray("GPU, 0, 0\n");
    QTest::newRow("negative used") << QByteArray("GPU, 8192, -5\n");
    QTest::newRow("blank lines") << QByteArray("\n\n  \n");
    QTest::newRow("header line") << QByteArray("name, memory.total [MiB], memory.used [MiB]\n");
}

void LimgenTests::hardware_parseNvidiaSmiSkipsJunk() {
    QFETCH(QByteArray, csv);
    QVERIFY(Hardware::parseNvidiaSmi(csv).isEmpty());
    // Junk between good lines does not drop the good lines.
    const QList<Gpu> gpus = Hardware::parseNvidiaSmi("A, 100, 10\n" + csv + "B, 200, 50\n");
    QCOMPARE(gpus.size(), 2);
    QCOMPARE(gpus[0].name, QString("A"));
    QCOMPARE(gpus[1].freeMiB, qint64(150));
}

namespace {
bool makeCard(const QString &root, const QString &card, const QString &vendor,
              const QMap<QString, QString> &files = {}) {
    if (!writeFile(root + "/" + card + "/device/vendor", vendor.toUtf8() + "\n"))
        return false;
    for (auto it = files.begin(); it != files.end(); ++it)
        if (!writeFile(root + "/" + card + "/device/" + it.key(), it.value().toUtf8() + "\n"))
            return false;
    return true;
}
} // namespace

void LimgenTests::hardware_readDrmAmdIntelAndSkips() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString root = fixture.root.path() + "/sys-drm";
    QVERIFY(makeCard(root, "card0", "0x1002",
                     {{"mem_info_vram_total", "8589934592"}, {"mem_info_vram_used", "1073741824"},
                      {"product_name", "AMD Radeon RX 7600"}}));
    QVERIFY(makeCard(root, "card1", "0x8086"));
    QVERIFY(makeCard(root, "card2", "0x10de", {{"mem_info_vram_total", "8589934592"}})); // NVIDIA: skipped here
    QVERIFY(makeCard(root, "card0-DP-1", "0x1002"));    // a connector, not a card
    QVERIFY(makeCard(root, "renderD128", "0x1002"));    // a render node
    QVERIFY(makeCard(root, "cardX", "0x1002"));
    QVERIFY(writeFile(root + "/version", "x"));          // a plain file
    // In /sys/class/drm the cards are symlinks to the device folders.
    QVERIFY(makeCard(fixture.root.path() + "/devices", "pci3", "0x8086"));
    QVERIFY(QFile::link(fixture.root.path() + "/devices/pci3", root + "/card3"));

    const QList<Gpu> gpus = Hardware::readDrm(root);
    QCOMPARE(gpus.size(), 3);
    QCOMPARE(gpus[0].vendor, QString("amd"));
    QCOMPARE(gpus[0].name, QString("AMD Radeon RX 7600"));
    QCOMPARE(gpus[0].totalMiB, qint64(8192));
    QCOMPARE(gpus[0].freeMiB, qint64(7168));
    QCOMPARE(gpus[1].vendor, QString("intel"));
    QCOMPARE(gpus[1].totalMiB, qint64(-1));
    QCOMPARE(gpus[1].freeMiB, qint64(-1));
    QCOMPARE(gpus[2].vendor, QString("intel")); // the symlinked card3
}

void LimgenTests::hardware_readDrmAmdWithoutMemoryFiles() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString root = fixture.root.path() + "/sys-drm";
    QVERIFY(makeCard(root, "card0", "0x1002")); // no product name, no memory files
    QVERIFY(makeCard(root, "card1", "0x1002", {{"mem_info_vram_total", "0"}, {"mem_info_vram_used", "0"}}));
    QVERIFY(makeCard(root, "card2", "0x1002", {{"mem_info_vram_total", "4294967296"}, {"mem_info_vram_used", "8589934592"}}));
    // An APU's dedicated memory is a small carve-out, and the GPU also uses
    // shared memory, so the carve-out says nothing about what fits.
    QVERIFY(makeCard(root, "card3", "0x1002", {{"mem_info_vram_total", "536870912"}, {"mem_info_vram_used", "0"}}));
    const QList<Gpu> gpus = Hardware::readDrm(root);
    QCOMPARE(gpus.size(), 4);
    QVERIFY(!gpus[0].name.isEmpty());
    QCOMPARE(gpus[0].totalMiB, qint64(-1));
    QCOMPARE(gpus[0].freeMiB, qint64(-1));
    QCOMPARE(gpus[1].totalMiB, qint64(-1)); // a zero total is not a reading
    // More used than total: no negative free memory.
    QCOMPARE(gpus[2].totalMiB, qint64(4096));
    QCOMPARE(gpus[2].freeMiB, qint64(0));
    QCOMPARE(gpus[3].totalMiB, qint64(-1));
    QCOMPARE(gpus[3].freeMiB, qint64(-1));
}

void LimgenTests::hardware_readDrmMissingRoot() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(Hardware::readDrm(fixture.root.path() + "/nowhere").isEmpty());
    QVERIFY(Hardware::readDrm(fixture.drm).isEmpty());
}

void LimgenTests::hardware_detectWithFakes() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/smi.log";
    Hardware::Sources sources;
    sources.nvidiaSmi = fakeNvidiaSmi(fixture, 12000, 2000, "Fake GPU", log);
    sources.drmRoot = fixture.drm;
    QVERIFY(makeCard(fixture.drm, "card0", "0x1002",
                     {{"mem_info_vram_total", "4294967296"}, {"mem_info_vram_used", "0"}}));
    const QList<Gpu> gpus = Hardware::detect(sources);
    QCOMPARE(gpus.size(), 2);
    QCOMPARE(gpus[0].vendor, QString("nvidia")); // nvidia-smi first, then the DRM cards
    QCOMPARE(gpus[0].name, QString("Fake GPU"));
    QCOMPARE(gpus[0].freeMiB, qint64(10000));
    QCOMPARE(gpus[1].vendor, QString("amd"));
    QCOMPARE(gpus[1].freeMiB, qint64(4096));
    QCOMPARE(QString(readFile(log)).trimmed(),
             QString("--query-gpu=name,memory.total,memory.used --format=csv,noheader,nounits"));
}

void LimgenTests::hardware_detectIgnoresFailingOrMissingSmi() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Hardware::Sources sources;
    sources.drmRoot = fixture.drm;
    // Valid output with a non-zero exit is not used.
    QVERIFY(writeScript(fixture.root.path() + "/smi-fail", "echo 'Fake GPU, 12000, 2000'\nexit 9\n"));
    sources.nvidiaSmi = fixture.root.path() + "/smi-fail";
    QVERIFY(Hardware::detect(sources).isEmpty());
    sources.nvidiaSmi = fixture.root.path() + "/does-not-exist";
    QVERIFY(Hardware::detect(sources).isEmpty());
    sources.nvidiaSmi = QString();
    QVERIFY(Hardware::detect(sources).isEmpty());
}

void LimgenTests::hardware_detectGivesUpOnSlowSmi() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeScript(fixture.root.path() + "/smi-slow", "exec sleep 30\n"));
    Hardware::Sources sources;
    sources.nvidiaSmi = fixture.root.path() + "/smi-slow";
    sources.drmRoot = fixture.drm;
    QElapsedTimer timer;
    timer.start();
    QVERIFY(Hardware::detect(sources).isEmpty());
    QVERIFY2(timer.elapsed() < 8000, qPrintable(QString::number(timer.elapsed())));
}

void LimgenTests::hardware_sourcesFromEnvironment() {
    EnvGuard guard({"LIMGEN_NVIDIA_SMI", "LIMGEN_DRM_ROOT"});
    qputenv("LIMGEN_NVIDIA_SMI", "/tmp/limgen-test/smi");
    qputenv("LIMGEN_DRM_ROOT", "/tmp/limgen-test/drm");
    Hardware::Sources sources = Hardware::Sources::fromEnvironment();
    QCOMPARE(sources.nvidiaSmi, QString("/tmp/limgen-test/smi"));
    QCOMPARE(sources.drmRoot, QString("/tmp/limgen-test/drm"));
    qputenv("LIMGEN_DRM_ROOT", "");
    QCOMPARE(Hardware::Sources::fromEnvironment().drmRoot, QString("/sys/class/drm"));
}

void LimgenTests::hardware_matching() {
    const QList<Gpu> gpus{{"nvidia", "NVIDIA GeForce RTX 3070 Ti", 8192, 6000}, {"amd", "AMD Radeon RX 7600", 8192, 4000}};
    QCOMPARE(Hardware::matching(gpus, "NVIDIA GeForce RTX 3070 Ti (NVIDIA) | uma: 0").name,
             QString("NVIDIA GeForce RTX 3070 Ti"));
    QCOMPARE(Hardware::matching(gpus, "amd radeon rx 7600 graphics").name, QString("AMD Radeon RX 7600"));
    // Several GPUs and no match: which one the engine uses is unknown, and
    // so is its memory; another card's figure must not stand in for it.
    QVERIFY(Hardware::matching(gpus, "Something else").name.isEmpty());
    QCOMPARE(Hardware::matching(gpus, "Something else").freeMiB, qint64(-1));
    QVERIFY(Hardware::matching(gpus, QString()).vendor.isEmpty());
    // A single GPU is the engine's, whatever the engine calls it.
    QCOMPARE(Hardware::matching({gpus[1]}, "RADV NAVI33").name, QString("AMD Radeon RX 7600"));
    const Gpu none = Hardware::matching({}, "anything");
    QVERIFY(none.name.isEmpty());
    QVERIFY(none.vendor.isEmpty());
    QCOMPARE(none.freeMiB, qint64(-1));
    // A GPU with no name never matches by name.
    QCOMPARE(Hardware::matching({{"intel", "", -1, -1}, {"amd", "AMD Radeon", 1, 1}}, "AMD Radeon").vendor,
             QString("amd"));
}

// ============================== ProgressParser ============================

void LimgenTests::progress_zImageLogGivesAllEvents() {
    const QByteArray log = dataFile("sd-cli-z-image-batch4.log");
    QVERIFY(!log.isEmpty());
    const QList<ProgressEvent> events = parseInChunks(log, 4, 8, log.size());
    const auto steps = ofKind(events, ProgressEvent::Step);
    QCOMPARE(steps.size(), 32);
    for (int k = 0; k < steps.size(); ++k) {
        QCOMPARE(steps[k].image, k / 8);
        QCOMPARE(steps[k].step, k % 8 + 1);
        QCOMPARE(steps[k].steps, 8);
        QVERIFY(steps[k].secondsPerStep > 1.0 && steps[k].secondsPerStep < 7.0);
    }
    QCOMPARE(steps.first().secondsPerStep, 6.62);
    QCOMPARE(steps[7].secondsPerStep, 1.12);
    const auto sampled = ofKind(events, ProgressEvent::Sampled);
    QCOMPARE(sampled.size(), 4);
    const auto decoded = ofKind(events, ProgressEvent::Decoded);
    QCOMPARE(decoded.size(), 4);
    const auto saved = ofKind(events, ProgressEvent::Saved);
    QCOMPARE(saved.size(), 4);
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(sampled[i].image, i);
        QCOMPARE(decoded[i].image, i);
        QCOMPARE(saved[i].image, i);
        QVERIFY2(saved[i].text.startsWith("/out/") && saved[i].text.endsWith(QString("-%1.png").arg(i)),
                 qPrintable(saved[i].text));
    }
    QVERIFY(ofKind(events, ProgressEvent::Error).isEmpty());
    // All of a kind come in order, and every sample precedes the first decode.
    int lastSampled = -1, firstDecoded = -1;
    for (int k = 0; k < events.size(); ++k) {
        if (events[k].kind == ProgressEvent::Sampled)
            lastSampled = k;
        if (events[k].kind == ProgressEvent::Decoded && firstDecoded < 0)
            firstDecoded = k;
    }
    QVERIFY(lastSampled < firstDecoded);
}

void LimgenTests::progress_kleinLogGivesAllEvents() {
    const QByteArray log = dataFile("sd-cli-klein-vulkan-batch4.log");
    QVERIFY(!log.isEmpty());
    const QList<ProgressEvent> events = parseInChunks(log, 4, 4, log.size());
    const auto steps = ofKind(events, ProgressEvent::Step);
    QCOMPARE(steps.size(), 16);
    for (int k = 0; k < steps.size(); ++k) {
        QCOMPARE(steps[k].image, k / 4);
        QCOMPARE(steps[k].step, k % 4 + 1);
        QCOMPARE(steps[k].steps, 4);
        QVERIFY(steps[k].secondsPerStep > 0);
    }
    QCOMPARE(ofKind(events, ProgressEvent::Sampled).size(), 4);
    QCOMPARE(ofKind(events, ProgressEvent::Decoded).size(), 4);
    const auto saved = ofKind(events, ProgressEvent::Saved);
    QCOMPARE(saved.size(), 4);
    for (int i = 0; i < 4; ++i)
        QCOMPARE(saved[i].image, i);
    QVERIFY(ofKind(events, ProgressEvent::Error).isEmpty());
}

void LimgenTests::progress_chunkSizeDoesNotChangeEvents_data() {
    QTest::addColumn<QString>("log");
    QTest::addColumn<int>("steps");
    QTest::addColumn<int>("images");
    QTest::newRow("z-image") << "sd-cli-z-image-batch4.log" << 8 << 4;
    QTest::newRow("klein") << "sd-cli-klein-vulkan-batch4.log" << 4 << 4;
    QTest::newRow("esrgan oom") << "sd-cli-esrgan-oom-exit0.log" << 1 << 1;
}

void LimgenTests::progress_chunkSizeDoesNotChangeEvents() {
    QFETCH(QString, log);
    QFETCH(int, steps);
    QFETCH(int, images);
    const QByteArray data = dataFile(log);
    QVERIFY(!data.isEmpty());
    QStringList whole;
    for (const ProgressEvent &e : parseInChunks(data, images, steps, data.size()))
        whole << eventText(e);
    QVERIFY(!whole.isEmpty());
    // Chunks of one byte split lines, escape sequences and UTF-8 anywhere.
    for (const qsizetype chunk : {qsizetype(1), qsizetype(2), qsizetype(7), qsizetype(64), qsizetype(1000)}) {
        QStringList parts;
        for (const ProgressEvent &e : parseInChunks(data, images, steps, chunk))
            parts << eventText(e);
        QVERIFY2(parts == whole, qPrintable(QString("chunk size %1").arg(chunk)));
    }
}

void LimgenTests::progress_tileProgressAfterSamplingIsIgnored() {
    ProgressParser parser(1, 8);
    QList<ProgressEvent> events = parser.feed("  |===>   | 8/8 - 1.00s/it\n[I] sampling completed, taking 8s\n");
    QCOMPARE(events.size(), 2);
    QCOMPARE(events[0].kind, ProgressEvent::Step);
    QCOMPARE(events[1].kind, ProgressEvent::Sampled);
    QCOMPARE(parser.sampled(), 1);
    // The VAE tile counter prints in the same format, here with 8 tiles too.
    events = parser.feed("  |===>   | 3/8 - 0.50s/it\n  |======| 8/8 - 0.50s/it\n");
    QVERIFY(events.isEmpty());
    events = parser.feed("[I] latent 1 decoded\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].kind, ProgressEvent::Decoded);
    QCOMPARE(events[0].image, 0);
}

void LimgenTests::progress_ignoresOtherTotalsAndOutOfRangeSteps() {
    ProgressParser parser(2, 8);
    QVERIFY(parser.feed("  |==| 3/12 - 1.00s/it\n").isEmpty()); // a tile or model-loading counter
    QVERIFY(parser.feed("  |==| 0/8 - 1.00s/it\n").isEmpty());
    QVERIFY(parser.feed("  |==| 9/8 - 1.00s/it\n").isEmpty());
    QCOMPARE(parser.feed("  |==| 1/8 - 1.00s/it\n").size(), 1);
    QCOMPARE(parser.feed("some text with 3/8 - 1.00s/it but no bar\n").size(), 0);
}

void LimgenTests::progress_itPerSecondBecomesSeconds() {
    ProgressParser parser(1, 8);
    QList<ProgressEvent> events = parser.feed("  |==| 1/8 - 2.00it/s\n  |==| 2/8 -   1.25s/it - ETA 00:07\n");
    QCOMPARE(events.size(), 2);
    QCOMPARE(events[0].secondsPerStep, 0.5);
    QCOMPARE(events[1].secondsPerStep, 1.25);
    // A rate of zero must not divide by zero.
    events = parser.feed("  |==| 3/8 - 0.00it/s\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].secondsPerStep, 0.0);
}

void LimgenTests::progress_stripsAnsiAndCarriageReturns() {
    ProgressParser parser(1, 8);
    const QList<ProgressEvent> events =
        parser.feed("\r  |==>  | 3/8 - 1.50s/it\x1b[K\r  |===> | 4/8 - 1.50s/it\x1b[K\n");
    QCOMPARE(events.size(), 2);
    QCOMPARE(events[0].step, 3);
    QCOMPARE(events[1].step, 4);
    QVERIFY(parser.feed("\x1b[K\n\n\r\r").isEmpty());
}

void LimgenTests::progress_finishParsesUnterminatedLine() {
    ProgressParser parser(2, 8);
    QVERIFY(parser.feed("[I] sampling completed, taking 1s").isEmpty()); // no newline yet
    QCOMPARE(parser.sampled(), 0);
    const QList<ProgressEvent> events = parser.finish();
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].kind, ProgressEvent::Sampled);
    QCOMPARE(events[0].image, 0);
    QVERIFY(parser.finish().isEmpty()); // nothing left the second time
}

void LimgenTests::progress_extraSampledLinesAreIgnored() {
    ProgressParser parser(1, 8);
    const QList<ProgressEvent> events =
        parser.feed("[I] sampling completed, taking 1s\n[I] sampling completed, taking 1s\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(parser.sampled(), 1);
}

void LimgenTests::progress_esrganOomLogGivesErrors() {
    const QByteArray log = dataFile("sd-cli-esrgan-oom-exit0.log");
    QVERIFY(!log.isEmpty());
    const QList<ProgressEvent> events = parseInChunks(log, 1, 1, log.size());
    const auto errors = ofKind(events, ProgressEvent::Error);
    QCOMPARE(errors.size(), 3);
    QVERIFY(errors[0].text.startsWith("esrgan segment 27/28"));
    QVERIFY(errors[2].text.startsWith("upscale failed"));
    QVERIFY(ofKind(events, ProgressEvent::Step).isEmpty());
    // The engine saved the unscaled image all the same; the parser reports that too.
    QCOMPARE(ofKind(events, ProgressEvent::Saved).size(), 1);
}

// ============================== SdCppEngine, static ======================

namespace {

EngineRequest fullRequest() {
    EngineRequest request;
    request.files = {{"diffusion-model", "/m/dit.gguf"}, {"llm", "/m/llm.gguf"}, {"vae", "/m/vae.safetensors"},
                     {"tae", "/m/tae.safetensors"}};
    request.prompt = "a fox, cinematic";
    request.negative = "blurry";
    request.width = 1344;
    request.height = 768;
    request.steps = 8;
    request.cfg = 1.0;
    request.sampler = "euler";
    request.seed = 4000000000u;
    request.count = 4;
    request.outputDirectory = "/cache/batch-1";
    return request;
}

EngineRequest runnableRequest(const QString &directory, int count = 4, int steps = 8) {
    EngineRequest request;
    for (const QString &role : QStringList{"diffusion-model", "llm", "vae", "tae"}) {
        const QString path = directory + "/models/" + role + ".bin";
        writeFile(path, "x");
        request.files.insert(role, path);
    }
    request.prompt = "a fox";
    request.width = 128;
    request.height = 96;
    request.steps = steps;
    request.sampler = "euler";
    request.seed = 7;
    request.count = count;
    request.outputDirectory = directory + "/out";
    return request;
}

// What an engine run reported, collected from its signals.
struct EngineRun {
    struct Progress {
        int image, step, steps;
        double secondsPerStep;
    };
    QStringList drafts;
    bool finished = false, failed = false;
    EngineError error;
    QList<Progress> progress;
    QList<int> sampled;
    QList<QPair<int, QString>> previews;
    bool done() const { return finished || failed; }
};

void watch(SdCppEngine &engine, EngineRun &run) {
    QObject::connect(&engine, &Engine::progress, &engine, [&run](int image, int step, int steps, double seconds) {
        run.progress.append({image, step, steps, seconds});
    });
    QObject::connect(&engine, &Engine::sampled, &engine, [&run](int image) { run.sampled << image; });
    QObject::connect(&engine, &Engine::preview, &engine,
                     [&run](int image, const QString &path) { run.previews.append({image, path}); });
    QObject::connect(&engine, &Engine::finished, &engine, [&run](const QStringList &drafts) {
        run.finished = true;
        run.drafts = drafts;
    });
    QObject::connect(&engine, &Engine::failed, &engine, [&run](const EngineError &error) {
        run.failed = true;
        run.error = error;
    });
}

} // namespace

void LimgenTests::engine_argumentsFullRequest() {
    EngineRequest request = fullRequest();
    request.cfg = 4.5;
    request.previews = true;
    request.previewInterval = 3;
    request.vaeTiling = true;
    const QStringList arguments = SdCppEngine::arguments(request);
    const QStringList expected{"--diffusion-model", "/m/dit.gguf", "--llm", "/m/llm.gguf", "--vae", "/m/vae.safetensors",
                               "--prompt", "a fox, cinematic", "--negative-prompt", "blurry",
                               "--width", "1344", "--height", "768", "--steps", "8", "--cfg-scale", "4.5",
                               "--sampling-method", "euler", "--seed", "4000000000", "--batch-count", "4",
                               "--output", "/cache/batch-1/draft-%d.png",
                               "--preview", "tae", "--tae", "/m/tae.safetensors", "--taesd-preview-only",
                               "--preview-path", "/cache/batch-1/preview/%03d.png", "--preview-interval", "3",
                               "--vae-tiling"};
    QCOMPARE(arguments, expected);
}

void LimgenTests::engine_argumentsOptionsExistInPinnedHelp() {
    const QString help = QString::fromUtf8(dataFile("sd-cli-help.txt"));
    QVERIFY(help.contains("--vae-tiling"));
    EngineRequest request = fullRequest();
    request.cfg = 4.5;
    request.previews = true;
    request.vaeTiling = true;
    request.files.insert("model", "/m/whole.safetensors");
    QSet<QString> options;
    for (const QString &argument : SdCppEngine::arguments(request))
        if (argument.startsWith("--"))
            options.insert(argument);
    // Every option limgen can emit, so a missing one means the argument list
    // grew without this test seeing it.
    QVERIFY(options.size() >= 18);
    for (const QString &option : options) {
        const QRegularExpression word("(^|[\\s,])" + QRegularExpression::escape(option) + "(\\s|$)",
                                      QRegularExpression::MultilineOption);
        QVERIFY2(word.match(help).hasMatch(), qPrintable(option + " is not an option of the pinned sd-cli"));
    }
}

void LimgenTests::engine_argumentsNegativeOnlyWithGuidance() {
    EngineRequest request = fullRequest();
    request.negative = "blurry, text";
    for (const double cfg : {0.0, 1.0}) {
        request.cfg = cfg;
        QVERIFY2(!SdCppEngine::arguments(request).contains("--negative-prompt"), qPrintable(QString::number(cfg)));
    }
    request.cfg = 1.5;
    QStringList arguments = SdCppEngine::arguments(request);
    QCOMPARE(valueAfter(arguments, "--negative-prompt"), QString("blurry, text"));
    QCOMPARE(valueAfter(arguments, "--cfg-scale"), QString("1.5"));
    request.negative.clear(); // guidance but nothing to avoid
    QVERIFY(!SdCppEngine::arguments(request).contains("--negative-prompt"));
}

void LimgenTests::engine_argumentsPreviewNeedsFlagAndTaeFile() {
    EngineRequest request = fullRequest();
    // A tae file alone is not a request for previews.
    request.previews = false;
    QStringList arguments = SdCppEngine::arguments(request);
    for (const QString &option : QStringList{"--preview", "--tae", "--taesd-preview-only", "--preview-path", "--preview-interval"})
        QVERIFY2(!arguments.contains(option), qPrintable(option));
    // Previews without a tae file cannot be drawn.
    request.previews = true;
    request.files.remove("tae");
    arguments = SdCppEngine::arguments(request);
    QVERIFY(!arguments.contains("--preview"));
    QVERIFY(!arguments.contains("--taesd-preview-only"));
    // Both: the tiny decoder is for previews only, never for the drafts.
    request.files.insert("tae", "/m/tae.safetensors");
    request.previewInterval = 5;
    arguments = SdCppEngine::arguments(request);
    QCOMPARE(valueAfter(arguments, "--preview"), QString("tae"));
    QCOMPARE(valueAfter(arguments, "--tae"), QString("/m/tae.safetensors"));
    QVERIFY(arguments.contains("--taesd-preview-only"));
    QCOMPARE(valueAfter(arguments, "--preview-path"), QString("/cache/batch-1/preview/%03d.png"));
    QCOMPARE(valueAfter(arguments, "--preview-interval"), QString("5"));
    // The tae file is never passed as the VAE.
    QCOMPARE(valueAfter(arguments, "--vae"), QString("/m/vae.safetensors"));
}

void LimgenTests::engine_argumentsTilingAndSampler() {
    EngineRequest request = fullRequest();
    QStringList arguments = SdCppEngine::arguments(request);
    QVERIFY(!arguments.contains("--vae-tiling"));
    request.vaeTiling = true;
    QVERIFY(SdCppEngine::arguments(request).contains("--vae-tiling"));
    request.sampler.clear();
    QVERIFY(!SdCppEngine::arguments(request).contains("--sampling-method"));
}

void LimgenTests::engine_argumentsRolesMapToOptions() {
    EngineRequest request = fullRequest();
    request.files = {{"model", "/m/all.safetensors"}};
    QStringList arguments = SdCppEngine::arguments(request);
    QCOMPARE(valueAfter(arguments, "--model"), QString("/m/all.safetensors"));
    QVERIFY(!arguments.contains("--diffusion-model"));
    QVERIFY(!arguments.contains("--llm"));
    QVERIFY(!arguments.contains("--vae"));
    request = fullRequest();
    arguments = SdCppEngine::arguments(request);
    QCOMPARE(valueAfter(arguments, "--diffusion-model"), QString("/m/dit.gguf"));
    QCOMPARE(valueAfter(arguments, "--llm"), QString("/m/llm.gguf"));
    QCOMPARE(valueAfter(arguments, "--vae"), QString("/m/vae.safetensors"));
    QVERIFY(!arguments.contains("--model"));
    // A prompt is one argument, whatever it contains.
    request.prompt = "--steps 99 \"quoted\" a, b";
    arguments = SdCppEngine::arguments(request);
    QCOMPARE(valueAfter(arguments, "--prompt"), request.prompt);
    QCOMPARE(arguments.count("--steps"), 1);
    QCOMPARE(valueAfter(arguments, "--output"), QString("/cache/batch-1/draft-%d.png"));
}

void LimgenTests::engine_parseVersion_data() {
    QTest::addColumn<QByteArray>("output");
    QTest::addColumn<QString>("version");
    QTest::newRow("release") << QByteArray("stable-diffusion.cpp version master-1106-532d2c1, commit 532d2c1\n")
                             << "master-1106-532d2c1";
    QTest::newRow("verbose prefix") << QByteArray("[V] version: stable-diffusion.cpp version master-1106-532d2c1, commit 532d2c1 --- main.cpp:760")
                                    << "master-1106-532d2c1";
    QTest::newRow("preceded by noise") << QByteArray("ggml_cuda_init: found 1 CUDA devices\nstable-diffusion.cpp version master-9-abc, commit abc\n")
                                       << "master-9-abc";
    QTest::newRow("no comma") << QByteArray("stable-diffusion.cpp version master-1-aaa commit aaa") << "master-1-aaa";
    QTest::newRow("garbage") << QByteArray("hello") << "";
    QTest::newRow("empty") << QByteArray() << "";
}

void LimgenTests::engine_parseVersion() {
    QFETCH(QByteArray, output);
    QFETCH(QString, version);
    QCOMPARE(SdCppEngine::parseVersion(output), version);
}

void LimgenTests::engine_parseDevices() {
    const QList<EngineDevice> devices = SdCppEngine::parseDevices(
        "Vulkan0\tNVIDIA GeForce RTX 3070 Ti\nCPU\tAMD Ryzen 7\r\nno tab here\n\ttab first\n\nCUDA1\t\n");
    QCOMPARE(devices.size(), 3);
    QCOMPARE(devices[0].name, QString("Vulkan0"));
    QCOMPARE(devices[0].description, QString("NVIDIA GeForce RTX 3070 Ti"));
    QVERIFY(devices[0].isGpu());
    QCOMPARE(devices[1].name, QString("CPU"));
    QCOMPARE(devices[1].description, QString("AMD Ryzen 7"));
    QVERIFY(!devices[1].isGpu());
    QCOMPARE(devices[2].name, QString("CUDA1"));
    QVERIFY(SdCppEngine::parseDevices("").isEmpty());
}

void LimgenTests::engine_deviceBackend_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("backend");
    QTest::newRow("vulkan") << "Vulkan0" << "vulkan";
    QTest::newRow("cuda, second card") << "CUDA12" << "cuda";
    QTest::newRow("rocm") << "ROCm1" << "rocm";
    QTest::newRow("cpu") << "CPU" << "cpu";
    QTest::newRow("empty") << "" << "";
}

void LimgenTests::engine_deviceBackend() {
    QFETCH(QString, name);
    QFETCH(QString, backend);
    const EngineDevice device{name, "x"};
    QCOMPARE(device.backend(), backend);
}

void LimgenTests::engine_probeGpuSkipsCpu() {
    EngineProbe probe;
    QVERIFY(probe.gpu().name.isEmpty());
    probe.devices = {{"CPU", "cpu"}, {"CUDA0", "first"}, {"Vulkan0", "second"}};
    QCOMPARE(probe.gpu().name, QString("CUDA0"));
    probe.devices = {{"CPU", "cpu"}};
    QVERIFY(probe.gpu().name.isEmpty());
}

void LimgenTests::engine_classify_data() {
    QTest::addColumn<int>("exitCode");
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("output");
    QTest::addColumn<int>("kind");
    const int normal = QProcess::NormalExit, crash = QProcess::CrashExit;
    using K = EngineError;
    QTest::newRow("success") << 0 << normal << "[I] 4/4 images saved\n" << int(K::None);
    QTest::newRow("success with warnings") << 0 << normal << "[W] something odd\n[I] done\n" << int(K::None);
    QTest::newRow("empty success") << 0 << normal << "" << int(K::None);
    // Auto-fit can warn about memory and then manage: a run that exits 0 with
    // no error line succeeded, whatever its warnings say.
    QTest::newRow("memory warning, success")
        << 0 << normal
        << "[W] execution engine cannot make enough memory available on Vulkan0\n[I] 4/4 images saved\n"
        << int(K::None);
    QTest::newRow("open warning, success") << 0 << normal << "[W] failed to open optional file\n[I] done\n"
                                           << int(K::None);
    QTest::newRow("oom text") << 1 << normal << "[E] out of memory\n" << int(K::OutOfMemory);
    QTest::newRow("oom upper case") << 1 << normal << "CUDA error: Out Of Memory" << int(K::OutOfMemory);
    QTest::newRow("cudaMalloc") << 1 << normal << "ggml_cuda: cudaMalloc failed: out" << int(K::OutOfMemory);
    QTest::newRow("vulkan oom") << 1 << normal << "vk::Device::allocateMemory: ErrorOutOfDeviceMemory" << int(K::OutOfMemory);
    QTest::newRow("engine words") << 1 << normal << "[E] execution engine cannot make enough memory available on Vulkan0"
                                  << int(K::OutOfMemory);
    QTest::newRow("failed to allocate") << 1 << normal << "ggml_backend_alloc_ctx_tensors: failed to allocate" << int(K::OutOfMemory);
    QTest::newRow("oom with exit 0") << 0 << normal << "[E] out of memory" << int(K::OutOfMemory);
    QTest::newRow("oom after a crash") << 0 << crash << "cudaMalloc failed" << int(K::OutOfMemory);
    QTest::newRow("missing file") << 1 << normal << "[E] cannot inspect model source '/x': No such file" << int(K::MissingFile);
    QTest::newRow("failed to open") << 1 << normal << "failed to open '/x'" << int(K::MissingFile);
    QTest::newRow("unsupported") << 1 << normal << "[E] get sd version from file failed: '/x'" << int(K::UnsupportedModel);
    QTest::newRow("crash") << 0 << crash << "[I] loading\n" << int(K::Failed);
    QTest::newRow("crash, exit code 139") << 139 << crash << "" << int(K::Failed);
    QTest::newRow("exit 1, no message") << 1 << normal << "[I] working\n" << int(K::Failed);
    QTest::newRow("exit 2") << 2 << normal << "" << int(K::Failed);
    QTest::newRow("error line, exit 0") << 0 << normal << "[I] saved\n[E] upscale failed --- main.cpp:1\n" << int(K::Failed);
    QTest::newRow("error line, exit 1") << 1 << normal << "[E] something broke\n" << int(K::Failed);
}

void LimgenTests::engine_classify() {
    QFETCH(int, exitCode);
    QFETCH(int, status);
    QFETCH(QString, output);
    QFETCH(int, kind);
    const EngineError error = SdCppEngine::classify(exitCode, QProcess::ExitStatus(status), output);
    QCOMPARE(int(error.kind), kind);
    if (kind != EngineError::None)
        QVERIFY(!error.message.isEmpty());
}

void LimgenTests::engine_classifyRealLogs() {
    // Successful batches of the pinned engine must not look like failures.
    for (const QString &name : QStringList{"sd-cli-z-image-batch4.log", "sd-cli-klein-vulkan-batch4.log"}) {
        const QString log = QString::fromUtf8(dataFile(name));
        QVERIFY(!log.isEmpty());
        QCOMPARE(int(SdCppEngine::classify(0, QProcess::NormalExit, log).kind), int(EngineError::None));
    }
    // The ESRGAN run that ran out of video memory and still exited 0.
    const QString oom = QString::fromUtf8(dataFile("sd-cli-esrgan-oom-exit0.log"));
    QCOMPARE(int(SdCppEngine::classify(0, QProcess::NormalExit, oom).kind), int(EngineError::OutOfMemory));
    // Without the memory words, the [E] lines alone are a failure.
    QString errorsOnly;
    for (const QString &line : oom.split('\n'))
        if (line.contains("[E] upscale") || line.contains("[I] save"))
            errorsOnly += line + "\n";
    const EngineError error = SdCppEngine::classify(0, QProcess::NormalExit, errorsOnly);
    QCOMPARE(int(error.kind), int(EngineError::Failed));
}

void LimgenTests::engine_classifyDetailHasErrorLines() {
    const QString output = "[I] loading\n[E] first problem --- a.cpp:1\r[W] warn\n[E] second problem\n[I] end\n";
    const EngineError error = SdCppEngine::classify(1, QProcess::NormalExit, output);
    QCOMPARE(int(error.kind), int(EngineError::Failed));
    QVERIFY2(error.detail.contains("first problem") && error.detail.contains("second problem"), qPrintable(error.detail));
    QVERIFY(!error.detail.contains("[I] loading"));
    QVERIFY(!error.detail.contains("warn"));
    // With no [E] line the end of the output is the detail.
    const EngineError plain = SdCppEngine::classify(1, QProcess::NormalExit, "[I] one\n[I] two\n");
    QVERIFY(plain.detail.contains("two"));
}

void LimgenTests::engine_draftPaths() {
    EngineRequest request;
    request.outputDirectory = "/cache/batch-9";
    request.count = 3;
    QCOMPARE(SdCppEngine::draftPaths(request),
             (QStringList{"/cache/batch-9/draft-0.png", "/cache/batch-9/draft-1.png", "/cache/batch-9/draft-2.png"}));
    request.count = 0;
    QVERIFY(SdCppEngine::draftPaths(request).isEmpty());
}

void LimgenTests::engine_defaultProgram() {
    EnvGuard guard({"LIMGEN_SD_CLI"});
    QCOMPARE(SdCppEngine::defaultProgram(), QString("/usr/lib/limgen/sd-cli"));
    qputenv("LIMGEN_SD_CLI", "/tmp/limgen-test/sd-cli");
    QCOMPARE(SdCppEngine::defaultProgram(), QString("/tmp/limgen-test/sd-cli"));
    QCOMPARE(SdCppEngine("/x/y").program(), QString("/x/y"));
}

void LimgenTests::engine_probeMissingProgram() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    QCOMPARE(int(SdCppEngine(fixture.root.path() + "/nope").probe().error.kind), int(EngineError::EngineMissing));
    // Present but not executable, and a folder.
    QVERIFY(writeFile(fixture.root.path() + "/plain", "#!/bin/sh\n"));
    QCOMPARE(int(SdCppEngine(fixture.root.path() + "/plain").probe().error.kind), int(EngineError::EngineMissing));
    QCOMPARE(int(SdCppEngine(fixture.root.path()).probe().error.kind), int(EngineError::EngineMissing));
    const EngineProbe probe = SdCppEngine(fixture.root.path() + "/nope").probe();
    QVERIFY(probe.error.message.contains(fixture.root.path() + "/nope"));
    QVERIFY(probe.version.isEmpty());
    QVERIFY(probe.devices.isEmpty());
}

void LimgenTests::engine_probeWrongVersion() {
    EnvGuard guard({"FAKE_SD_CLI_MODE"});
    qputenv("FAKE_SD_CLI_MODE", "old-version");
    const EngineProbe probe = SdCppEngine(fakeEngine).probe();
    QCOMPARE(int(probe.error.kind), int(EngineError::WrongVersion));
    QCOMPARE(probe.version, QString("master-1000-0000000"));
    QVERIFY(probe.error.message.contains("master-1000-0000000"));
    QVERIFY(probe.error.message.contains(SdCppEngine::pinnedVersion));
    QVERIFY(probe.devices.isEmpty()); // not asked once the version is wrong
    // An engine that does not print a version at all.
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeScript(fixture.root.path() + "/odd", "echo hello\n"));
    QCOMPARE(int(SdCppEngine(fixture.root.path() + "/odd").probe().error.kind), int(EngineError::WrongVersion));
}

void LimgenTests::engine_probeNoGpu() {
    EnvGuard guard({"FAKE_SD_CLI_MODE"});
    qputenv("FAKE_SD_CLI_MODE", "no-gpu");
    const EngineProbe probe = SdCppEngine(fakeEngine).probe();
    QCOMPARE(int(probe.error.kind), int(EngineError::NoGpu));
    QCOMPARE(probe.version, QString(SdCppEngine::pinnedVersion));
    QCOMPARE(probe.devices.size(), 1);
    QVERIFY(probe.gpu().name.isEmpty());
}

void LimgenTests::engine_probeOk() {
    EnvGuard guard({"FAKE_SD_CLI_MODE"});
    const EngineProbe probe = SdCppEngine(fakeEngine).probe();
    QCOMPARE(int(probe.error.kind), int(EngineError::None));
    QVERIFY(probe.error.message.isEmpty());
    QCOMPARE(probe.version, QString(SdCppEngine::pinnedVersion));
    QCOMPARE(probe.devices.size(), 2);
    QCOMPARE(probe.gpu().name, QString("Vulkan0"));
    QCOMPARE(probe.gpu().description, QString("Fake GPU"));
    QCOMPARE(probe.gpu().backend(), QString("vulkan"));
}

// ============================== SdCppEngine, async =======================

#define RUN_ENGINE(engine, run, request, timeout) \
    watch(engine, run);                           \
    engine.start(request);                        \
    QTRY_VERIFY_WITH_TIMEOUT((run).done(), timeout)

void LimgenTests::engine_runFinishesWithDrafts() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/fake.log";
    qputenv("FAKE_SD_CLI_LOG", log.toUtf8());
    const EngineRequest request = runnableRequest(fixture.root.path());
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY2(run.finished, qPrintable(run.error.message + run.error.detail));
    QCOMPARE(run.drafts, SdCppEngine::draftPaths(request));
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(QImageReader(run.drafts[i]).size(), QSize(128, 96));
        // The fake paints a colour from seed + index: the seeds counted up.
        const quint64 value = (quint64(request.seed) + i) * 2654435761ULL % 16777216ULL;
        const QColor expected(int(value >> 16), int((value >> 8) & 255), int(value & 255));
        QCOMPARE(QImage(run.drafts[i]).pixelColor(3, 3), expected);
    }
    QVERIFY(!engine.isRunning());
    QCOMPARE(run.progress.size(), 32);
    for (int k = 0; k < run.progress.size(); ++k) {
        QCOMPARE(run.progress[k].image, k / 8);
        QCOMPARE(run.progress[k].step, k % 8 + 1);
        QCOMPARE(run.progress[k].steps, 8);
        QCOMPARE(run.progress[k].secondsPerStep, 0.01);
    }
    QCOMPARE(run.sampled, (QList<int>{0, 1, 2, 3}));
    QVERIFY(run.previews.isEmpty());
    QVERIFY(!QFileInfo::exists(request.outputDirectory + "/preview"));
    // The engine ran with exactly the arguments arguments() builds.
    const QList<QStringList> calls = imageCalls(log);
    QCOMPARE(calls.size(), 1);
    QCOMPARE(calls[0], SdCppEngine::arguments(request));
}

void LimgenTests::engine_runReportsPreviewsWithImageIndices() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/fake.log";
    qputenv("FAKE_SD_CLI_LOG", log.toUtf8());
    EngineRequest request = runnableRequest(fixture.root.path());
    request.previews = true;
    request.previewInterval = 2;
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY2(run.finished, qPrintable(run.error.message + run.error.detail));
    // 8 steps with an interval of 2 give 4 previews per image.
    QCOMPARE(run.previews.size(), 16);
    QMap<int, int> perImage;
    int previous = 0;
    for (const auto &preview : run.previews) {
        perImage[preview.first]++;
        QVERIFY2(preview.first >= previous, "previews come in image order");
        previous = preview.first;
        QVERIFY2(QFileInfo(preview.second).isFile(), qPrintable(preview.second));
        QVERIFY(preview.second.startsWith(request.outputDirectory + "/preview/"));
        QVERIFY(QImageReader(preview.second).size().isValid()); // complete on disk
    }
    QCOMPARE(perImage, (QMap<int, int>{{0, 4}, {1, 4}, {2, 4}, {3, 4}}));
    QCOMPARE(QSet<QString>(run.drafts.begin(), run.drafts.end()).size(), 4);
    const QStringList arguments = imageCalls(log).first();
    QVERIFY(arguments.contains("--taesd-preview-only"));
    QCOMPARE(valueAfter(arguments, "--preview-interval"), QString("2"));
}

// Previews left in the batch folder by an earlier attempt (the run before an
// out-of-memory retry) must not be reported as this run's.
void LimgenTests::engine_previewsFromAnEarlierAttemptAreNotReported() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    EngineRequest request = runnableRequest(fixture.root.path());
    request.previews = true;
    request.previewInterval = 2;
    QVERIFY(writeFile(request.outputDirectory + "/preview/900.png", "stale"));
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY2(run.finished, qPrintable(run.error.message + run.error.detail));
    QCOMPARE(run.previews.size(), 16);
    for (const auto &preview : run.previews)
        QVERIFY2(!preview.second.endsWith("/900.png"), qPrintable(preview.second));
}

void LimgenTests::engine_runSecondStartWhileRunningIsIgnored() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/fake.log";
    qputenv("FAKE_SD_CLI_LOG", log.toUtf8());
    qputenv("FAKE_SD_CLI_STEP_SECONDS", "0.05");
    const EngineRequest request = runnableRequest(fixture.root.path(), 1, 4);
    EngineRequest other = request;
    other.prompt = "a different prompt";
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    watch(engine, run);
    engine.start(request);
    QVERIFY(engine.isRunning());
    engine.start(other);
    QTRY_VERIFY_WITH_TIMEOUT(run.done(), 20000);
    QVERIFY(run.finished);
    const QList<QStringList> calls = imageCalls(log);
    QCOMPARE(calls.size(), 1);
    QCOMPARE(valueAfter(calls[0], "--prompt"), QString("a fox"));
}

void LimgenTests::engine_oomOnceRetriesWithVaeTiling() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/fake.log";
    qputenv("FAKE_SD_CLI_LOG", log.toUtf8());
    qputenv("FAKE_SD_CLI_MODE", "oom-once");
    const EngineRequest request = runnableRequest(fixture.root.path(), 2, 4);
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY2(run.finished, qPrintable(run.error.message + run.error.detail));
    QVERIFY(!run.failed);
    QCOMPARE(run.drafts.size(), 2);
    for (const QString &draft : run.drafts)
        QCOMPARE(QImageReader(draft).size(), QSize(128, 96));
    const QList<QStringList> calls = imageCalls(log);
    QCOMPARE(calls.size(), 2);
    QVERIFY(!calls[0].contains("--vae-tiling"));
    QVERIFY(calls[1].contains("--vae-tiling"));
    // Same batch otherwise.
    QStringList withoutTiling = calls[1];
    withoutTiling.removeAll("--vae-tiling");
    QCOMPARE(withoutTiling, calls[0]);
}

void LimgenTests::engine_oomFailsAfterOneRetry() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/fake.log";
    qputenv("FAKE_SD_CLI_LOG", log.toUtf8());
    qputenv("FAKE_SD_CLI_MODE", "oom");
    const EngineRequest request = runnableRequest(fixture.root.path(), 2, 4);
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY(run.failed);
    QVERIFY(!run.finished);
    QCOMPARE(int(run.error.kind), int(EngineError::OutOfMemory));
    QVERIFY(run.error.detail.contains("memory"));
    const QList<QStringList> calls = imageCalls(log);
    QCOMPARE(calls.size(), 2); // one retry, not more
    QVERIFY(calls[1].contains("--vae-tiling"));
    QVERIFY(!QFileInfo::exists(request.outputDirectory + "/draft-0.png"));
}

void LimgenTests::engine_oomWithTilingAlreadyOnIsNotRetried() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString log = fixture.root.path() + "/fake.log";
    qputenv("FAKE_SD_CLI_LOG", log.toUtf8());
    qputenv("FAKE_SD_CLI_MODE", "oom");
    EngineRequest request = runnableRequest(fixture.root.path(), 1, 4);
    request.vaeTiling = true;
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QCOMPARE(int(run.error.kind), int(EngineError::OutOfMemory));
    QCOMPARE(imageCalls(log).size(), 1);
}

void LimgenTests::engine_errorExit0Fails() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    qputenv("FAKE_SD_CLI_MODE", "error-exit0");
    const EngineRequest request = runnableRequest(fixture.root.path(), 2, 4);
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    // The drafts exist and have the right size; the [E] line still makes it a failure.
    QVERIFY(QFileInfo::exists(request.outputDirectory + "/draft-0.png"));
    QVERIFY(run.failed);
    QVERIFY(!run.finished);
    QCOMPARE(int(run.error.kind), int(EngineError::Failed));
    QVERIFY2(run.error.detail.contains("fake error after saving"), qPrintable(run.error.detail));
}

void LimgenTests::engine_wrongSizeFails() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    qputenv("FAKE_SD_CLI_MODE", "wrong-size");
    const EngineRequest request = runnableRequest(fixture.root.path(), 2, 4);
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY(run.failed);
    QVERIFY(!run.finished);
    QVERIFY(run.drafts.isEmpty());
    QCOMPARE(int(run.error.kind), int(EngineError::Failed));
    QVERIFY(!run.error.message.isEmpty());
}

void LimgenTests::engine_failModeFails() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    qputenv("FAKE_SD_CLI_MODE", "fail");
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, runnableRequest(fixture.root.path(), 1, 4), 20000);
    QVERIFY(run.failed);
    QVERIFY(!run.finished);
    QCOMPARE(int(run.error.kind), int(EngineError::Failed));
    QVERIFY(run.error.detail.contains("fake engine failure"));
}

void LimgenTests::engine_cancelDuringHangLeavesNoFiles() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    qputenv("FAKE_SD_CLI_MODE", "hang");
    EngineRequest request = runnableRequest(fixture.root.path(), 2, 4);
    request.previews = true;
    request.previewInterval = 1;
    // A partial draft such as a killed engine leaves behind.
    QVERIFY(writeFile(request.outputDirectory + "/draft-0.png", "partial"));
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    watch(engine, run);
    engine.start(request);
    // The fake writes the first preview, then hangs.
    QTRY_VERIFY_WITH_TIMEOUT(!QDir(request.outputDirectory + "/preview").entryList({"*.png"}).isEmpty(), 10000);
    QVERIFY(engine.isRunning());
    QElapsedTimer timer;
    timer.start();
    engine.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(run.done(), 10000);
    QVERIFY(run.failed);
    QVERIFY(!run.finished);
    QCOMPARE(int(run.error.kind), int(EngineError::Cancelled));
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!engine.isRunning());
    QVERIFY(!QFileInfo::exists(request.outputDirectory + "/draft-0.png"));
    QVERIFY(!QFileInfo::exists(request.outputDirectory + "/draft-1.png"));
    QVERIFY(!QFileInfo::exists(request.outputDirectory + "/preview"));
    // Cancelling an engine that is not running does nothing.
    run = EngineRun();
    engine.cancel();
    QTest::qWait(100);
    QVERIFY(!run.done());
}

void LimgenTests::engine_missingProgramFails() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    SdCppEngine engine(fixture.root.path() + "/no-such-engine");
    EngineRun run;
    RUN_ENGINE(engine, run, runnableRequest(fixture.root.path(), 1, 4), 10000);
    QVERIFY(run.failed);
    QVERIFY(!run.finished);
    QCOMPARE(int(run.error.kind), int(EngineError::EngineMissing));
    QVERIFY(run.error.message.contains("no-such-engine"));
    QVERIFY(!engine.isRunning());
}

// ============================== Library ==================================

namespace {

QString makePng(const QString &path, int width = 4, int height = 3) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(10, 120, 200));
    image.save(path, "PNG");
    return path;
}

DraftRecord fullRecord() {
    DraftRecord record;
    record.themeSlug = "night-city";
    record.themeMode = "dark";
    record.palette = QJsonObject{{"accent", "#7aa2f7"}, {"background", "#1a1b26"}};
    record.subject = "oil rig at dusk";
    record.prompt = "oil rig at dusk, cinematic wide shot";
    record.negative = "blurry";
    record.style = "cinematic";
    record.modelId = "z-image-turbo";
    record.level = "q4_k";
    record.filesSha256 = {"aa11", "bb22"};
    record.engineVersion = "master-1106-532d2c1";
    record.engineBackend = "vulkan";
    record.seed = 4000000000u;
    record.steps = 8;
    record.cfg = 1.0;
    record.sampler = "euler";
    record.width = 1344;
    record.height = 768;
    return record;
}

const QDateTime fixedTime(QDate(2026, 11, 20), QTime(10, 30, 15), QTimeZone::UTC);

} // namespace

void LimgenTests::library_isValidId_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<bool>("valid");
    QTest::newRow("typical") << "2026-11-20-a3f9c1" << true;
    QTest::newRow("all digits") << "2026-01-02-000000" << true;
    QTest::newRow("upper-case hex") << "2026-11-20-A3F9C1" << false;
    QTest::newRow("short suffix") << "2026-11-20-a3f9c" << false;
    QTest::newRow("long suffix") << "2026-11-20-a3f9c12" << false;
    QTest::newRow("non-hex") << "2026-11-20-a3f9cz" << false;
    QTest::newRow("one digit month") << "2026-1-20-a3f9c1" << false;
    QTest::newRow("empty") << "" << false;
    QTest::newRow("trailing newline") << "2026-11-20-a3f9c1\n" << false;
    QTest::newRow("extension") << "2026-11-20-a3f9c1.png" << false;
    QTest::newRow("traversal") << "../2026-11-20-a3f9c1" << false;
    QTest::newRow("slash prefix") << "x/2026-11-20-a3f9c1" << false;
    QTest::newRow("space") << " 2026-11-20-a3f9c1" << false;
}

void LimgenTests::library_isValidId() {
    QFETCH(QString, id);
    QFETCH(bool, valid);
    QCOMPARE(Library::isValidId(id), valid);
}

void LimgenTests::library_newIdFormatAndUtcDate() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Library library(fixture.data + "/library");
    const QString id = library.newId(fixedTime);
    QVERIFY2(Library::isValidId(id), qPrintable(id));
    QVERIFY(id.startsWith("2026-11-20-"));
    // 05:00 at UTC+10 on the 21st is still the 20th in UTC.
    const QDateTime ahead(QDate(2026, 11, 21), QTime(5, 0), QTimeZone::fromSecondsAheadOfUtc(10 * 3600));
    QVERIFY2(library.newId(ahead).startsWith("2026-11-20-"), qPrintable(library.newId(ahead)));
    // Nothing is created by asking for an id.
    QVERIFY(!QFileInfo::exists(fixture.data + "/library"));
}

void LimgenTests::library_defaultDirectory() {
    EnvGuard guard({"XDG_DATA_HOME"});
    qputenv("XDG_DATA_HOME", "/tmp/limgen-test/xdgdata");
    QCOMPARE(Library::defaultDirectory(), QString("/tmp/limgen-test/xdgdata/limgen/library"));
    QCOMPARE(Library().directory(), QString("/tmp/limgen-test/xdgdata/limgen/library"));
    QCOMPARE(Library("/x/y").directory(), QString("/x/y"));
}

void LimgenTests::library_addDraftWritesImageAndSidecar() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString source = makePng(fixture.root.path() + "/src/draft-0.png");
    Library library(fixture.data + "/library");
    QString error;
    const LibraryEntry entry = library.addDraft(source, fullRecord(), fixedTime, &error);
    QVERIFY2(entry.isValid(), qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(Library::isValidId(entry.id));
    QVERIFY(entry.id.startsWith("2026-11-20-"));
    QCOMPARE(entry.image, fixture.data + "/library/2026-11/" + entry.id + ".png");
    QCOMPARE(entry.sidecar, fixture.data + "/library/2026-11/" + entry.id + ".json");
    // The image is a copy of the source, which stays where it was.
    QVERIFY(QFileInfo::exists(source));
    QCOMPARE(readFile(entry.image), readFile(source));
    // Nothing else is left beside the entry, in particular no .part file.
    QCOMPARE(QDir(fixture.data + "/library/2026-11").entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name),
             (QStringList{entry.id + ".json", entry.id + ".png"}));

    const QJsonObject json = QJsonDocument::fromJson(readFile(entry.sidecar)).object();
    QCOMPARE(json, entry.metadata);
    QCOMPARE(json["id"].toString(), entry.id);
    QCOMPARE(json["kind"].toString(), QString("draft"));
    QCOMPARE(json["created"].toString(), QString("2026-11-20T10:30:15Z"));
    const QJsonObject theme = json["theme"].toObject();
    QCOMPARE(theme["slug"].toString(), QString("night-city"));
    QCOMPARE(theme["mode"].toString(), QString("dark"));
    QCOMPARE(theme["palette"].toObject()["accent"].toString(), QString("#7aa2f7"));
    QCOMPARE(json["subject"].toString(), QString("oil rig at dusk"));
    QCOMPARE(json["prompt"].toString(), QString("oil rig at dusk, cinematic wide shot"));
    QCOMPARE(json["negative"].toString(), QString("blurry"));
    QCOMPARE(json["style"].toString(), QString("cinematic"));
    const QJsonObject model = json["model"].toObject();
    QCOMPARE(model["id"].toString(), QString("z-image-turbo"));
    QCOMPARE(model["level"].toString(), QString("q4_k"));
    QCOMPARE(model["files_sha256"].toArray().size(), 2);
    QCOMPARE(model["files_sha256"].toArray()[1].toString(), QString("bb22"));
    const QJsonObject engine = json["engine"].toObject();
    QCOMPARE(engine["name"].toString(), QString("sd-cli"));
    QCOMPARE(engine["version"].toString(), QString("master-1106-532d2c1"));
    QCOMPARE(engine["backend"].toString(), QString("vulkan"));
    // The seed is a number, and one above 2^31 survives.
    QVERIFY(json["seed"].isDouble());
    QCOMPARE(json["seed"].toDouble(), 4000000000.0);
    QCOMPARE(json["steps"].toInt(), 8);
    QCOMPARE(json["cfg"].toDouble(), 1.0);
    QCOMPARE(json["sampler"].toString(), QString("euler"));
    QVERIFY(json["size"].isArray());
    QCOMPARE(json["size"].toArray().size(), 2);
    QCOMPARE(json["size"].toArray()[0].toInt(), 1344);
    QCOMPARE(json["size"].toArray()[1].toInt(), 768);
    QVERIFY(json.contains("source") && json["source"].isNull());
    QVERIFY(json.contains("finish") && json["finish"].isNull());
}

void LimgenTests::library_addDraftWithoutThemeHasNullTheme() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString source = makePng(fixture.root.path() + "/src/draft-0.png");
    Library library(fixture.data + "/library");
    DraftRecord record = fullRecord();
    record.themeSlug.clear();
    record.themeMode.clear();
    record.palette = QJsonObject();
    const LibraryEntry entry = library.addDraft(source, record, fixedTime);
    QVERIFY(entry.isValid());
    const QJsonObject json = QJsonDocument::fromJson(readFile(entry.sidecar)).object();
    QVERIFY(json.contains("theme"));
    QVERIFY(json["theme"].isNull());
}

void LimgenTests::library_addDraftNeverOverwritesAndIdsDiffer() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString first = makePng(fixture.root.path() + "/src/a.png", 4, 3);
    const QString second = makePng(fixture.root.path() + "/src/b.png", 8, 6);
    Library library(fixture.data + "/library");
    // The same moment for all of them, so only the random part tells them apart.
    QSet<QString> ids;
    QList<LibraryEntry> entries;
    for (int i = 0; i < 20; ++i) {
        const LibraryEntry entry = library.addDraft(i == 0 ? first : second, fullRecord(), fixedTime);
        QVERIFY(entry.isValid());
        ids.insert(entry.id);
        entries << entry;
    }
    QCOMPARE(ids.size(), 20);
    // The first entry is still what it was after the others were added.
    QCOMPARE(readFile(entries[0].image), readFile(first));
    QCOMPARE(QJsonDocument::fromJson(readFile(entries[0].sidecar)).object()["id"].toString(), entries[0].id);
    QCOMPARE(QDir(fixture.data + "/library/2026-11").entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 40);
    for (const LibraryEntry &entry : entries)
        QVERIFY(!QFileInfo::exists(entry.image + ".part"));
}

void LimgenTests::library_addDraftFromMissingSourceFails() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    QString error;
    const LibraryEntry entry = library.addDraft(fixture.root.path() + "/src/nope.png", fullRecord(), fixedTime, &error);
    QVERIFY(!entry.isValid());
    QVERIFY(!error.isEmpty());
    QVERIFY2(error.contains("nope.png"), qPrintable(error));
    // No image, sidecar or .part file is left behind.
    QCOMPARE(QDir(fixture.data + "/library/2026-11").entryList(QDir::Files | QDir::NoDotAndDotDot), QStringList());
    // A null error pointer is accepted.
    QVERIFY(!library.addDraft(fixture.root.path() + "/src/nope.png", fullRecord(), fixedTime).isValid());
}

void LimgenTests::library_addDraftIntoUnwritableFolderFails() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString source = makePng(fixture.root.path() + "/src/a.png");
    // The library folder would have to be created below a plain file.
    QVERIFY(writeFile(fixture.root.path() + "/blocker", "x"));
    Library library(fixture.root.path() + "/blocker/library");
    QString error;
    QVERIFY(!library.addDraft(source, fullRecord(), fixedTime, &error).isValid());
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo(fixture.root.path() + "/blocker").isFile());
}

void LimgenTests::library_findRoundTrip() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString source = makePng(fixture.root.path() + "/src/a.png");
    Library library(fixture.data + "/library");
    const LibraryEntry added = library.addDraft(source, fullRecord(), fixedTime);
    QVERIFY(added.isValid());
    // A second library object on the same folder finds it.
    const LibraryEntry found = Library(fixture.data + "/library").find(added.id);
    QVERIFY(found.isValid());
    QCOMPARE(found.id, added.id);
    QCOMPARE(found.image, added.image);
    QCOMPARE(found.sidecar, added.sidecar);
    QCOMPARE(found.metadata, added.metadata);
    QCOMPARE(found.metadata["model"].toObject()["id"].toString(), QString("z-image-turbo"));
}

void LimgenTests::library_findRejectsBadEntries() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString source = makePng(fixture.root.path() + "/src/a.png");
    Library library(fixture.data + "/library");
    const LibraryEntry added = library.addDraft(source, fullRecord(), fixedTime);
    QVERIFY(added.isValid());

    QVERIFY(!library.find("").isValid());
    QVERIFY(!library.find("not-an-id").isValid());
    QVERIFY(!library.find("2026-11-20-000000").isValid()); // well formed, not there
    // A path in place of an id never reaches the disk, even if the file exists.
    QVERIFY(!library.find("../library/2026-11/" + added.id).isValid());
    QVERIFY(!library.find(added.id + ".png").isValid());

    // A sidecar naming another id.
    const QByteArray original = readFile(added.sidecar);
    QJsonObject other = QJsonDocument::fromJson(original).object();
    other["id"] = "2026-11-20-ffffff";
    QVERIFY(writeFile(added.sidecar, QJsonDocument(other).toJson()));
    QVERIFY(!library.find(added.id).isValid());
    // A sidecar that is not JSON.
    QVERIFY(writeFile(added.sidecar, "not json"));
    QVERIFY(!library.find(added.id).isValid());
    // Restored, it is found again; then without the sidecar and without the image.
    QVERIFY(writeFile(added.sidecar, original));
    QVERIFY(library.find(added.id).isValid());
    QVERIFY(QFile::remove(added.sidecar));
    QVERIFY(!library.find(added.id).isValid());
    QVERIFY(writeFile(added.sidecar, original));
    QVERIFY(QFile::remove(added.image));
    QVERIFY(!library.find(added.id).isValid());
}

// ============================== CLI: doctor and generate =================

namespace {

// Environment for the headless commands that run the fake engine against a
// temp models folder, a fake nvidia-smi and an empty DRM root.
QHash<QString, QString> engineEnvironment(const Fixture &fixture, int usedMiB = 2000, int totalMiB = 12000) {
    return {{"LIMGEN_SD_CLI", fakeEngine},
            {"LIMGEN_MODELS", fixture.models},
            {"LIMGEN_NVIDIA_SMI", fakeNvidiaSmi(fixture, totalMiB, usedMiB)},
            {"LIMGEN_DRM_ROOT", fixture.drm},
            {"FAKE_SD_CLI_LOG", fixture.root.path() + "/fake.log"},
            {"LIMGEN_DOWNLOAD_BASE", "http://127.0.0.1:1"}}; // nothing listens there
}

QStringList filesUnder(const QString &directory) {
    QStringList files;
    QDirIterator it(directory, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    files.sort();
    return files;
}

} // namespace

void LimgenTests::cli_doctorJson() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"doctor", "--json"}, engineEnvironment(fixture));
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.err, QString());
    const QJsonObject json = jsonObject(result.out);
    QVERIFY2(!json.isEmpty(), qPrintable(result.out));
    QCOMPARE(json["ok"].toBool(), true);
    QCOMPARE(json["problems"].toArray().size(), 0);
    QCOMPARE(json["modelsDirectory"].toString(), fixture.models);
    const QJsonObject engine = json["engine"].toObject();
    QCOMPARE(engine["path"].toString(), fakeEngine);
    QCOMPARE(engine["version"].toString(), QString(SdCppEngine::pinnedVersion));
    QCOMPARE(engine["pinned"].toString(), QString(SdCppEngine::pinnedVersion));
    QCOMPARE(engine["backend"].toString(), QString("vulkan"));
    QCOMPARE(engine["devices"].toArray().size(), 2);
    const QJsonArray gpus = json["gpus"].toArray();
    QCOMPARE(gpus.size(), 1);
    QCOMPARE(gpus[0].toObject()["name"].toString(), QString("Fake GPU"));
    QCOMPARE(gpus[0].toObject()["totalMiB"].toInt(), 12000);
    QCOMPARE(gpus[0].toObject()["freeMiB"].toInt(), 10000);

    QJsonObject zImage, klein;
    for (const QJsonValue &value : json["models"].toArray()) {
        if (value.toObject()["id"].toString() == "z-image-turbo")
            zImage = value.toObject();
        if (value.toObject()["id"].toString() == "flux2-klein-4b")
            klein = value.toObject();
    }
    QCOMPARE(zImage["default"].toBool(), true);
    QCOMPARE(zImage["installed"].toBool(), true);
    QCOMPARE(zImage["runnable"].toBool(), true);
    QCOMPARE(zImage["missing"].toArray().size(), 0);
    QCOMPARE(klein["default"].toBool(), false);
    QCOMPARE(klein["installed"].toBool(), false);
    QCOMPARE(klein["runnable"].toBool(), false);
    // What klein lacks is what is not shared with the installed model.
    QSet<QString> expected;
    const ModelLevel &zLevel = catalogue.model("z-image-turbo")->levels.first();
    for (const QString &key : catalogue.model("flux2-klein-4b")->levels.first().files)
        if (!zLevel.files.values().contains(key))
            expected.insert(describeFile(*catalogue.file(key)));
    QSet<QString> missing;
    for (const QJsonValue &value : klein["missing"].toArray())
        missing.insert(value.toString());
    QVERIFY(!expected.isEmpty());
    QCOMPARE(missing, expected);
    QCOMPARE(json["upscaler"].toObject()["installed"].toBool(), false);
}

void LimgenTests::cli_doctorText() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"doctor"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.err, QString());
    QVERIFY2(result.out.contains(fakeEngine), qPrintable(result.out));
    QVERIFY(result.out.contains(SdCppEngine::pinnedVersion));
    QVERIFY(result.out.contains("Fake GPU"));
    QVERIFY(result.out.contains("10000"));
    QVERIFY(result.out.contains(fixture.models));
    QVERIFY(result.out.contains("z-image-turbo"));
    QVERIFY(result.out.contains("flux2-klein-4b"));
    // The other model's missing files are named.
    QVERIFY(result.out.contains("flux2-vae"));
}

void LimgenTests::cli_doctorDefaultModelMissing() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    // Only the second model is installed.
    QVERIFY(installModel(catalogue, "flux2-klein-4b", fixture.models));
    const CliResult json = runCli(fixture, {"doctor", "--json"}, engineEnvironment(fixture));
    QCOMPARE(json.exitCode, 3);
    const QJsonObject object = jsonObject(json.out);
    QCOMPARE(object["ok"].toBool(), true == false);
    const QJsonArray problems = object["problems"].toArray();
    QCOMPARE(problems.size(), 1);
    QVERIFY2(problems[0].toString().contains(catalogue.defaultModel()->name), qPrintable(problems[0].toString()));
    for (const QJsonValue &value : object["models"].toArray())
        if (value.toObject()["id"].toString() == "z-image-turbo") {
            QCOMPARE(value.toObject()["installed"].toBool(), false);
            QCOMPARE(value.toObject()["runnable"].toBool(), false);
            QVERIFY(value.toObject()["missing"].toArray().size() >= 3);
        }
    const CliResult text = runCli(fixture, {"doctor"}, engineEnvironment(fixture));
    QCOMPARE(text.exitCode, 3);
    QVERIFY(text.out.contains(catalogue.defaultModel()->name));
    QCOMPARE(text.err, QString());
}

void LimgenTests::cli_doctorTooLittleFreeMemory() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models));
    const int need = catalogue.defaultModel()->levels.first().minFreeMiB;
    // 12000 total, 9000 used: 3000 free, below the model's minimum.
    const CliResult result = runCli(fixture, {"doctor", "--json"}, engineEnvironment(fixture, 9000, 12000));
    QCOMPARE(result.exitCode, 3);
    const QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["ok"].toBool(), false);
    const QJsonArray problems = json["problems"].toArray();
    QCOMPARE(problems.size(), 1);
    const QString problem = problems[0].toString();
    QVERIFY2(problem.contains(QString::number(need)) && problem.contains("3000") && problem.contains("12000"),
             qPrintable(problem));
    for (const QJsonValue &value : json["models"].toArray())
        if (value.toObject()["id"].toString() == "z-image-turbo") {
            QCOMPARE(value.toObject()["installed"].toBool(), true);
            QCOMPARE(value.toObject()["runnable"].toBool(), false);
        }
    // Exactly the minimum is enough.
    const CliResult enough = runCli(fixture, {"doctor", "--json"}, engineEnvironment(fixture, 12000 - need, 12000));
    QCOMPARE(enough.exitCode, 0);
    QCOMPARE(jsonObject(enough.out)["ok"].toBool(), true);
}

void LimgenTests::cli_doctorUnknownMemoryIsNotAProblem() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    // An Intel GPU shares system memory: free memory is unknown, not zero.
    QVERIFY(makeCard(fixture.drm, "card0", "0x8086"));
    QHash<QString, QString> environment = engineEnvironment(fixture);
    environment["LIMGEN_NVIDIA_SMI"] = fixture.root.path() + "/no-nvidia-smi";
    const CliResult result = runCli(fixture, {"doctor", "--json"}, environment);
    QCOMPARE(result.exitCode, 0);
    const QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["ok"].toBool(), true);
    QCOMPARE(json["gpus"].toArray().size(), 1);
    QCOMPARE(json["gpus"].toArray()[0].toObject()["vendor"].toString(), QString("intel"));
    QCOMPARE(json["gpus"].toArray()[0].toObject()["freeMiB"].toInt(), -1);
}

void LimgenTests::cli_doctorNoGpuWrongVersionOrMissingEngine() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    for (const QString &mode : QStringList{"no-gpu", "old-version"}) {
        QHash<QString, QString> environment = engineEnvironment(fixture);
        environment["FAKE_SD_CLI_MODE"] = mode;
        const CliResult result = runCli(fixture, {"doctor", "--json"}, environment);
        QVERIFY2(result.exitCode == 3, qPrintable(mode));
        const QJsonObject json = jsonObject(result.out);
        QCOMPARE(json["ok"].toBool(), false);
        QCOMPARE(json["problems"].toArray().size(), 1);
        QVERIFY(!json["problems"].toArray()[0].toString().isEmpty());
    }
    QHash<QString, QString> environment = engineEnvironment(fixture);
    environment["LIMGEN_SD_CLI"] = fixture.root.path() + "/no-engine";
    const CliResult result = runCli(fixture, {"doctor", "--json"}, environment);
    QCOMPARE(result.exitCode, 3);
    QVERIFY(jsonObject(result.out)["problems"].toArray()[0].toString().contains("no-engine"));
}

namespace {
// Common checks of a successful generate --json run.
struct Generated {
    QJsonObject json;
    QStringList ids, paths;
    QList<qint64> seeds;
};
} // namespace

void LimgenTests::cli_generateJsonWithTheme() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"generate", "oil rig at dusk", "--count", "3", "--seed", "100", "--json"},
                                    engineEnvironment(fixture));
    QVERIFY(result.started);
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
    QCOMPARE(result.err, QString());
    const QJsonObject json = jsonObject(result.out);
    QVERIFY2(!json.isEmpty(), qPrintable(result.out));
    QCOMPARE(json["model"].toString(), QString("z-image-turbo"));
    QCOMPARE(json["theme"].toString(), QString("night-city"));
    QCOMPARE(json["style"].toString(), QString("cinematic"));
    QCOMPARE(json["seed"].toInt(), 100);
    const Palette palette = Palette::fromToml(stockToml);
    const QString prompt = json["prompt"].toString();
    QVERIFY2(prompt.startsWith("oil rig at dusk, cinematic wide shot, " + palette.words.join(", ") + " colour palette, "),
             qPrintable(prompt));
    QVERIFY(prompt.contains(palette.mood));

    const QJsonArray drafts = json["drafts"].toArray();
    QCOMPARE(drafts.size(), 3);
    const QString library = fixture.data + "/limgen/library";
    for (int i = 0; i < 3; ++i) {
        const QJsonObject draft = drafts[i].toObject();
        const QString id = draft["id"].toString();
        QVERIFY2(Library::isValidId(id), qPrintable(id));
        QCOMPARE(draft["seed"].toInt(), 100 + i);
        QCOMPARE(draft["path"].toString(), library + "/" + id.left(7) + "/" + id + ".png");
        QCOMPARE(QImageReader(draft["path"].toString()).size(), QSize(1344, 768));
        // The sidecar carries the theme, the palette and this draft's own seed.
        const LibraryEntry entry = Library(library).find(id);
        QVERIFY2(entry.isValid(), qPrintable(id));
        const QJsonObject sidecar = entry.metadata;
        QCOMPARE(sidecar["seed"].toDouble(), double(100 + i));
        QCOMPARE(sidecar["theme"].toObject()["slug"].toString(), QString("night-city"));
        QCOMPARE(sidecar["theme"].toObject()["mode"].toString(), QString("dark"));
        QCOMPARE(sidecar["theme"].toObject()["palette"].toObject()["accent"].toString(), QString("#7aa2f7"));
        QCOMPARE(sidecar["theme"].toObject()["palette"].toObject()["background"].toString(), QString("#1a1b26"));
        QCOMPARE(sidecar["prompt"].toString(), prompt);
        QCOMPARE(sidecar["subject"].toString(), QString("oil rig at dusk"));
        QCOMPARE(sidecar["model"].toObject()["id"].toString(), QString("z-image-turbo"));
        QCOMPARE(sidecar["engine"].toObject()["version"].toString(), QString(SdCppEngine::pinnedVersion));
        QCOMPARE(sidecar["engine"].toObject()["backend"].toString(), QString("vulkan"));
        QCOMPARE(sidecar["size"].toArray()[0].toInt(), 1344);
        QCOMPARE(sidecar["steps"].toInt(), 8);
        QCOMPARE(sidecar["model"].toObject()["files_sha256"].toArray().size(), 3); // dit, llm, vae; not the tae
        QVERIFY(sidecar["negative"].toString().isEmpty()); // no guidance, no negative prompt
    }
    // Only the library holds files: three PNGs and their sidecars, nothing in the
    // cache (the batch folder is removed) and nothing near the Omarchy folders.
    QCOMPARE(filesUnder(library).size(), 6);
    QCOMPARE(filesUnder(fixture.cache), QStringList());
    QCOMPARE(filesUnder(fixture.backgrounds), QStringList());
    // The engine got the same prompt and the seed, and the models from the temp folder.
    const QList<QStringList> calls = imageCalls(fixture.root.path() + "/fake.log");
    QCOMPARE(calls.size(), 1);
    QCOMPARE(valueAfter(calls[0], "--prompt"), prompt);
    QCOMPARE(valueAfter(calls[0], "--seed"), QString("100"));
    QCOMPARE(valueAfter(calls[0], "--batch-count"), QString("3"));
    QCOMPARE(valueAfter(calls[0], "--width"), QString("1344"));
    QVERIFY(valueAfter(calls[0], "--diffusion-model").startsWith(fixture.models + "/"));
    QVERIFY(valueAfter(calls[0], "--output").startsWith(fixture.cache + "/limgen/batch-"));
    QVERIFY(!calls[0].contains("--negative-prompt"));
}

void LimgenTests::cli_generateTextOutput() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"generate", "a quiet harbour", "--count", "2"}, engineEnvironment(fixture));
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
    const QStringList lines = result.out.split('\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 2);
    for (const QString &line : lines) {
        const QStringList parts = line.split("  ");
        QCOMPARE(parts.size(), 2);
        QVERIFY2(Library::isValidId(parts[0]), qPrintable(line));
        QVERIFY(QFileInfo(parts[1]).isFile());
        QVERIFY(parts[1].startsWith(fixture.data + "/limgen/library/"));
    }
}

void LimgenTests::cli_generateRandomSeedCountsUp() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"generate", "a fox", "--count", "3", "--json"}, engineEnvironment(fixture));
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
    const QJsonObject json = jsonObject(result.out);
    const qint64 seed = qint64(json["seed"].toDouble());
    QVERIFY(seed >= 0 && seed < (qint64(1) << 31));
    const QJsonArray drafts = json["drafts"].toArray();
    QCOMPARE(drafts.size(), 3);
    for (int i = 0; i < 3; ++i)
        QCOMPARE(qint64(drafts[i].toObject()["seed"].toDouble()), seed + i);
    QCOMPARE(valueAfter(imageCalls(fixture.root.path() + "/fake.log").first(), "--seed"), QString::number(seed));
    // Two runs without --seed do not repeat each other.
    const CliResult again = runCli(fixture, {"generate", "a fox", "--count", "1", "--json"}, engineEnvironment(fixture));
    QCOMPARE(again.exitCode, 0);
    QVERIFY(jsonObject(again.out)["seed"].toDouble() != json["seed"].toDouble());
}

void LimgenTests::cli_generatePromptOverridesBuiltPrompt() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"generate", "--prompt", "a plain red square", "--count", "1", "--json"},
                                    engineEnvironment(fixture));
    QVERIFY2(result.exitCode == 0, qPrintable(result.err)); // no subject needed with --prompt
    const QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["prompt"].toString(), QString("a plain red square"));
    QCOMPARE(valueAfter(imageCalls(fixture.root.path() + "/fake.log").first(), "--prompt"),
             QString("a plain red square"));
    // A subject beside --prompt does not change the prompt either.
    const CliResult both = runCli(fixture, {"generate", "a subject", "--prompt", "only this", "--count", "1", "--json"},
                                  engineEnvironment(fixture));
    QCOMPARE(both.exitCode, 0);
    QCOMPARE(jsonObject(both.out)["prompt"].toString(), QString("only this"));
}

void LimgenTests::cli_generateStyleOptionIsHonoured() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"generate", "a fox", "--style", "photo", "--count", "1", "--json"},
                                    engineEnvironment(fixture));
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
    const QJsonObject json = jsonObject(result.out);
    // QGuiApplication takes "-style <name>" (and "--style <name>") for itself and
    // removes it from argv; main() must hand commands the arguments as typed.
    QCOMPARE(json["style"].toString(), QString("photo"));
    QVERIFY(json["prompt"].toString().contains("photograph, wide angle"));
    QVERIFY(!json["prompt"].toString().contains("cinematic"));
    const QString id = json["drafts"].toArray()[0].toObject()["id"].toString();
    QCOMPARE(Library(fixture.data + "/limgen/library").find(id).metadata["style"].toString(), QString("photo"));
}

void LimgenTests::cli_generateSelectsModelById() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models));
    QVERIFY(installModel(catalogue, "flux2-klein-4b", fixture.models));
    const CliResult result = runCli(fixture, {"generate", "a fox", "--model", "flux2-klein-4b", "--count", "1", "--json"},
                                    engineEnvironment(fixture));
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
    const QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["model"].toString(), QString("flux2-klein-4b"));
    const CatalogueModel &klein = *catalogue.model("flux2-klein-4b");
    const QStringList call = imageCalls(fixture.root.path() + "/fake.log").first();
    QCOMPARE(valueAfter(call, "--steps"), QString::number(klein.steps));
    QCOMPARE(valueAfter(call, "--diffusion-model"),
             Catalogue::path(*catalogue.file(klein.levels.first().files.value("diffusion-model")), fixture.models));
    const QString id = json["drafts"].toArray()[0].toObject()["id"].toString();
    QCOMPARE(Library(fixture.data + "/limgen/library").find(id).metadata["model"].toObject()["id"].toString(),
             QString("flux2-klein-4b"));
}

void LimgenTests::cli_generateWithoutThemeStillWorks() {
    REQUIRE_BINARY();
    Fixture fixture; // no theme.name
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"generate", "a fox", "--count", "1", "--seed", "5", "--json"},
                                    engineEnvironment(fixture));
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
    const QJsonObject json = jsonObject(result.out);
    QVERIFY(json.contains("theme"));
    QVERIFY(json["theme"].isNull());
    QCOMPARE(json["prompt"].toString(), QString("a fox, cinematic wide shot, atmospheric, highly detailed, desktop wallpaper"));
    const QString id = json["drafts"].toArray()[0].toObject()["id"].toString();
    const LibraryEntry entry = Library(fixture.data + "/limgen/library").find(id);
    QVERIFY(entry.isValid());
    QVERIFY(entry.metadata["theme"].isNull());
}

void LimgenTests::cli_generateUsageErrors_data() {
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("no subject, no prompt") << QStringList{"generate"};
    QTest::newRow("blank subject") << QStringList{"generate", "   "};
    QTest::newRow("count 0") << QStringList{"generate", "a fox", "--count", "0"};
    QTest::newRow("count 9") << QStringList{"generate", "a fox", "--count", "9"};
    QTest::newRow("count x") << QStringList{"generate", "a fox", "--count", "x"};
    QTest::newRow("count negative") << QStringList{"generate", "a fox", "--count", "-1"};
    QTest::newRow("style bogus") << QStringList{"generate", "a fox", "--style", "bogus"};
    QTest::newRow("model bogus") << QStringList{"generate", "a fox", "--model", "bogus"};
    QTest::newRow("seed x") << QStringList{"generate", "a fox", "--seed", "x"};
    QTest::newRow("seed negative") << QStringList{"generate", "a fox", "--seed", "-1"};
    QTest::newRow("seed too large") << QStringList{"generate", "a fox", "--seed", "4294967296"};
    // The last draft's seed (seed + count - 1) must fit as well.
    QTest::newRow("seeds past the top")
        << QStringList{"generate", "a fox", "--seed", "4294967295", "--count", "2"};
    QTest::newRow("extra argument") << QStringList{"generate", "a fox", "another"};
    QTest::newRow("unknown option") << QStringList{"generate", "a fox", "--bogus"};
}

void LimgenTests::cli_generateUsageErrors() {
    REQUIRE_BINARY();
    QFETCH(QStringList, arguments);
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, arguments, engineEnvironment(fixture));
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 2);
    QCOMPARE(result.out, QString());
    QVERIFY(!result.err.isEmpty());
    // A usage error must not start the engine or touch the library.
    QCOMPARE(imageCalls(fixture.root.path() + "/fake.log").size(), 0);
    QCOMPARE(filesUnder(fixture.data), QStringList());
    QCOMPARE(filesUnder(fixture.cache), QStringList());
}

void LimgenTests::cli_generateModelNotInstalled() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    // Nothing installed: the default model's files are named.
    CliResult result = runCli(fixture, {"generate", "a fox", "--json"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 3);
    QCOMPARE(result.out, QString());
    // Every file the engine needs is named; the preview decoder is not
    // needed by a command, which draws no previews.
    const ModelLevel &level = catalogue.defaultModel()->levels.first();
    for (auto it = level.files.begin(); it != level.files.end(); ++it) {
        const QString name = describeFile(*catalogue.file(it.value()));
        if (it.key() == "tae")
            QVERIFY2(!result.err.contains(name), qPrintable(result.err));
        else
            QVERIFY2(result.err.contains(name), qPrintable(result.err));
    }
    QVERIFY(result.err.contains(fixture.models));
    // The default is installed, the requested model is not: only its own files are named.
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models));
    result = runCli(fixture, {"generate", "a fox", "--model", "flux2-klein-4b"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 3);
    QCOMPARE(result.out, QString());
    QVERIFY(result.err.contains("flux2-vae"));
    QVERIFY(!result.err.contains("z-image-turbo/ae.safetensors")); // the installed model's own file
    QCOMPARE(imageCalls(fixture.root.path() + "/fake.log").size(), 0);
    QCOMPARE(filesUnder(fixture.data), QStringList());
}

// generate draws no previews, so it must not refuse to run because the tiny
// preview decoder is missing.
void LimgenTests::cli_generateNeedsNoPreviewDecoder() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models));
    QVERIFY(QFile::remove(Catalogue::path(*catalogue.file("taef1"), fixture.models)));
    const CliResult result = runCli(fixture, {"generate", "a fox", "--count", "1"}, engineEnvironment(fixture));
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
}

void LimgenTests::cli_generateTooLittleFreeMemory() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const CliResult result = runCli(fixture, {"generate", "a fox"}, engineEnvironment(fixture, 9000, 12000));
    QCOMPARE(result.exitCode, 3);
    QCOMPARE(result.out, QString());
    QVERIFY2(result.err.contains("3000") && result.err.contains("12000") && result.err.contains("5000"),
             qPrintable(result.err));
    QCOMPARE(imageCalls(fixture.root.path() + "/fake.log").size(), 0);
    QCOMPARE(filesUnder(fixture.data), QStringList());
}

void LimgenTests::cli_generateEngineProblems_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("exitCode");
    QTest::addColumn<int>("imageRuns");
    QTest::newRow("no gpu") << "no-gpu" << 3 << 0;
    QTest::newRow("old engine") << "old-version" << 3 << 0;
    QTest::newRow("out of memory, after one retry") << "oom" << 3 << 2;
    QTest::newRow("engine fails") << "fail" << 1 << 1;
    QTest::newRow("error with exit 0") << "error-exit0" << 1 << 1;
    QTest::newRow("wrong size") << "wrong-size" << 1 << 1;
}

void LimgenTests::cli_generateEngineProblems() {
    REQUIRE_BINARY();
    QFETCH(QString, mode);
    QFETCH(int, exitCode);
    QFETCH(int, imageRuns);
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    QHash<QString, QString> environment = engineEnvironment(fixture);
    environment["FAKE_SD_CLI_MODE"] = mode;
    const CliResult result = runCli(fixture, {"generate", "a fox", "--count", "2", "--json"}, environment);
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, exitCode);
    QCOMPARE(result.out, QString());
    QVERIFY(!result.err.isEmpty());
    QCOMPARE(imageCalls(fixture.root.path() + "/fake.log").size(), imageRuns);
    // Nothing was added to the library, and no batch folder is left in the cache.
    QCOMPARE(filesUnder(fixture.data), QStringList());
    QCOMPARE(filesUnder(fixture.cache), QStringList());
    QCOMPARE(filesUnder(fixture.backgrounds), QStringList());
}

void LimgenTests::cli_generateOomOnceSucceeds() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    QHash<QString, QString> environment = engineEnvironment(fixture);
    environment["FAKE_SD_CLI_MODE"] = "oom-once";
    const CliResult result = runCli(fixture, {"generate", "a fox", "--count", "2", "--json"}, environment);
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
    QCOMPARE(jsonObject(result.out)["drafts"].toArray().size(), 2);
    const QList<QStringList> calls = imageCalls(fixture.root.path() + "/fake.log");
    QCOMPARE(calls.size(), 2);
    QVERIFY(!calls[0].contains("--vae-tiling"));
    QVERIFY(calls[1].contains("--vae-tiling"));
    QCOMPARE(filesUnder(fixture.data + "/limgen/library").size(), 4);
    QCOMPARE(filesUnder(fixture.cache), QStringList());
}

void LimgenTests::cli_generateInterruptedExits130() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    QHash<QString, QString> extra = engineEnvironment(fixture);
    extra["FAKE_SD_CLI_MODE"] = "hang";

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char *name : {"WAYLAND_DISPLAY", "DISPLAY", "QT_QPA_PLATFORM", "QT_QPA_PLATFORMTHEME",
                             "HYPRLAND_INSTANCE_SIGNATURE", "LIMGEN_HYPRCTL", "LIMGEN_OMARCHY_BIN"})
        environment.remove(name);
    environment.insert("HOME", fixture.root.path() + "/home");
    environment.insert("LIMGEN_OMARCHY_STATE", fixture.state);
    environment.insert("LIMGEN_OMARCHY_BACKGROUNDS", fixture.backgrounds);
    environment.insert("XDG_CONFIG_HOME", fixture.root.path() + "/config");
    environment.insert("XDG_STATE_HOME", fixture.root.path() + "/state");
    environment.insert("XDG_DATA_HOME", fixture.data);
    environment.insert("XDG_CACHE_HOME", fixture.cache);
    for (auto it = extra.begin(); it != extra.end(); ++it)
        environment.insert(it.key(), it.value());

    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(qEnvironmentVariable("LIMGEN_BINARY"), {"generate", "a fox", "--count", "2", "--json"});
    QVERIFY(process.waitForStarted(5000));
    // Wait until the fake engine has been asked to make images, and has had
    // time to write its first preview-less step and hang.
    const QString log = fixture.root.path() + "/fake.log";
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(log).isEmpty(), 15000);
    QTest::qWait(500);
    QCOMPARE(process.state(), QProcess::Running);
    QVERIFY(::kill(pid_t(process.processId()), SIGINT) == 0);
    QVERIFY(process.waitForFinished(15000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 130);
    QCOMPARE(QString::fromUtf8(process.readAllStandardOutput()), QString());
    QVERIFY(!QString::fromUtf8(process.readAllStandardError()).isEmpty());
    QCOMPARE(filesUnder(fixture.data), QStringList()); // no drafts
    QCOMPARE(filesUnder(fixture.cache), QStringList());
    // The cache folder holds no batch folder at all.
    QCOMPARE(QDir(fixture.cache + "/limgen").entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList());
}

namespace {
// The environment runCli gives a command, for tests that drive the process
// themselves while it runs.
QProcessEnvironment commandEnvironment(Fixture &fixture, const QHash<QString, QString> &extra) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char *name : {"WAYLAND_DISPLAY", "DISPLAY", "QT_QPA_PLATFORM", "QT_QPA_PLATFORMTHEME",
                             "HYPRLAND_INSTANCE_SIGNATURE", "LIMGEN_HYPRCTL", "LIMGEN_OMARCHY_BIN"})
        environment.remove(name);
    environment.insert("HOME", fixture.root.path() + "/home");
    environment.insert("LIMGEN_OMARCHY_STATE", fixture.state);
    environment.insert("LIMGEN_OMARCHY_BACKGROUNDS", fixture.backgrounds);
    environment.insert("XDG_CONFIG_HOME", fixture.root.path() + "/config");
    environment.insert("XDG_STATE_HOME", fixture.root.path() + "/state");
    environment.insert("XDG_DATA_HOME", fixture.data);
    environment.insert("XDG_CACHE_HOME", fixture.cache);
    for (auto it = extra.begin(); it != extra.end(); ++it)
        environment.insert(it.key(), it.value());
    return environment;
}
} // namespace

// A batch is made for the theme that was current when it started; switching
// themes while it runs must not file it under the new theme's name.
void LimgenTests::cli_generateKeepsTheThemeItStartedWith() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    writeStockTheme(fixture);
    QVERIFY(fixture.addTheme("other-theme", stockToml));
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    QHash<QString, QString> extra = engineEnvironment(fixture);
    extra["FAKE_SD_CLI_STEP_SECONDS"] = "0.3";

    QProcess process;
    process.setProcessEnvironment(commandEnvironment(fixture, extra));
    process.start(qEnvironmentVariable("LIMGEN_BINARY"), {"generate", "a fox", "--count", "1", "--json"});
    QVERIFY(process.waitForStarted(5000));
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(fixture.root.path() + "/fake.log").isEmpty(), 15000);
    QVERIFY(fixture.switchTo("other-theme"));
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitCode(), 0);
    const QJsonObject json = jsonObject(QString::fromUtf8(process.readAllStandardOutput()));
    QCOMPARE(json["theme"].toString(), QString("night-city"));
    const QString id = json["drafts"].toArray()[0].toObject()["id"].toString();
    const LibraryEntry entry = Library(fixture.data + "/limgen/library").find(id);
    QVERIFY(entry.isValid());
    QCOMPARE(entry.metadata["theme"].toObject()["slug"].toString(), QString("night-city"));
}

// Ctrl+C in a terminal signals the whole foreground process group, so the
// engine dies on its own, often before limgen gets to cancel it. That is
// still an interruption: exit 130, nothing kept.
void LimgenTests::cli_generateInterruptedByTerminalExits130() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    QHash<QString, QString> extra = engineEnvironment(fixture);
    extra["FAKE_SD_CLI_MODE"] = "hang";

    QProcess process;
    process.setProcessEnvironment(commandEnvironment(fixture, extra));
    // Its own process group, so the signal below reaches limgen and its
    // engine but not this test.
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
    process.start(qEnvironmentVariable("LIMGEN_BINARY"), {"generate", "a fox", "--count", "2", "--json"});
    QVERIFY(process.waitForStarted(5000));
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(fixture.root.path() + "/fake.log").isEmpty(), 15000);
    QTest::qWait(500);
    QVERIFY(::kill(-pid_t(process.processId()), SIGINT) == 0);
    QVERIFY(process.waitForFinished(15000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 130);
    QCOMPARE(QString::fromUtf8(process.readAllStandardOutput()), QString());
    QCOMPARE(filesUnder(fixture.data), QStringList());
    QCOMPARE(QDir(fixture.cache + "/limgen").entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList());
}

void LimgenTests::cli_helpListsDoctorAndGenerate() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult summary = runCli(fixture, {"help"});
    QCOMPARE(summary.exitCode, 0);
    QVERIFY(summary.out.contains("doctor"));
    QVERIFY(summary.out.contains("generate"));
    const CliResult generate = runCli(fixture, {"help", "generate"});
    QCOMPARE(generate.exitCode, 0);
    for (const QString &option : QStringList{"--count", "--style", "--model", "--seed", "--prompt", "--json"})
        QVERIFY2(generate.out.contains(option), qPrintable(option));
    const CliResult doctor = runCli(fixture, {"help", "doctor"});
    QCOMPARE(doctor.exitCode, 0);
    QVERIFY(doctor.out.contains("--json"));
}

// ============================== L3 helpers ================================

namespace {

// A small HTTP server that serves files from memory, with a switch for each
// way a real download goes wrong. It lives in the test's own event loop.
class HttpServer : public QObject {
public:
    struct Route {
        QByteArray body;
        bool honourRange = true;
        int rangeStartOverride = -1;  // answer a range request from this offset instead
        int status = 0;               // answer with this status and an error page
        QString redirectTo;           // answer 302 with this absolute URL
        int dropAfterBytes = -1;      // close after this many body bytes, on the first request only
        int shortBytes = -1;          // declare the full length, send this many body bytes, every request
        int chunk = 0, chunkDelayMs = 0; // a slow body
        bool dropped = false;
    };
    struct Request {
        QString path;
        QByteArray range;
    };

    HttpServer() {
        connect(&m_server, &QTcpServer::newConnection, this, [this] { accept(); });
        listening = m_server.listen(QHostAddress::LocalHost, 0);
    }
    quint16 port() const { return m_server.serverPort(); }
    Route &serve(const QString &path, const QByteArray &body) {
        Route &route = routes[path];
        route = Route();
        route.body = body;
        return route;
    }
    QStringList paths() const {
        QStringList list;
        for (const Request &request : requests)
            list << request.path;
        return list;
    }
    QList<QByteArray> rangesOf(const QString &path) const {
        QList<QByteArray> list;
        for (const Request &request : requests)
            if (request.path == path)
                list << request.range;
        return list;
    }
    int count(const QString &path) const { return paths().count(path); }

    bool listening = false;
    QMap<QString, Route> routes;
    QList<Request> requests;

private:
    void accept() {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                *buffer += socket->readAll();
                if (buffer->contains("\r\n\r\n")) {
                    const QByteArray request = *buffer;
                    buffer->clear();
                    handle(socket, request);
                }
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    static void send(QTcpSocket *socket, int status, const QByteArray &reason, const QByteArray &headers,
                     const QByteArray &body, qint64 declared = -1) {
        QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n" + headers +
                          "Connection: close\r\nContent-Length: " +
                          QByteArray::number(declared < 0 ? body.size() : declared) + "\r\n\r\n";
        socket->write(head + body);
    }

    void handle(QTcpSocket *socket, const QByteArray &request) {
        const QList<QByteArray> lines = request.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QString path = QString::fromLatin1(first.value(1));
        QByteArray range;
        for (const QByteArray &line : lines)
            if (line.toLower().startsWith("range:"))
                range = line.mid(6).trimmed();
        requests.append({path, range});

        auto it = routes.find(path);
        if (it == routes.end() || it->status != 0) {
            const int status = it == routes.end() ? 404 : it->status;
            send(socket, status, "Error", status == 416 ? "Content-Range: bytes */1\r\n" : "",
                 "<html>error page</html>");
            socket->disconnectFromHost();
            return;
        }
        Route &route = *it;
        if (!route.redirectTo.isEmpty()) {
            send(socket, 302, "Found", "Location: " + route.redirectTo.toLatin1() + "\r\n", "");
            socket->disconnectFromHost();
            return;
        }
        qint64 start = 0;
        bool partial = false;
        if (route.honourRange && range.startsWith("bytes=")) {
            start = range.mid(6).split('-').value(0).toLongLong();
            if (start >= route.body.size()) {
                send(socket, 416, "Range Not Satisfiable",
                     "Content-Range: bytes */" + QByteArray::number(route.body.size()) + "\r\n", "");
                socket->disconnectFromHost();
                return;
            }
            partial = true;
        }
        const qint64 from = partial && route.rangeStartOverride >= 0 ? route.rangeStartOverride : start;
        const QByteArray payload = route.body.mid(from);
        QByteArray headers;
        int status = 200;
        QByteArray reason = "OK";
        if (partial) {
            status = 206;
            reason = "Partial Content";
            headers = "Content-Range: bytes " + QByteArray::number(from) + "-" +
                      QByteArray::number(route.body.size() - 1) + "/" + QByteArray::number(route.body.size()) + "\r\n";
        }
        qint64 limit = payload.size();
        if (route.dropAfterBytes >= 0 && !route.dropped) {
            route.dropped = true;
            limit = qMin<qint64>(limit, route.dropAfterBytes);
        } else if (route.shortBytes >= 0) {
            limit = qMin<qint64>(limit, route.shortBytes);
        }
        if (route.chunk > 0) {
            send(socket, status, reason, headers, "", payload.size());
            auto sent = std::make_shared<qint64>(0);
            auto *timer = new QTimer(socket);
            const int chunk = route.chunk;
            connect(timer, &QTimer::timeout, socket, [socket, timer, sent, payload, chunk] {
                socket->write(payload.mid(*sent, chunk));
                *sent += chunk;
                if (*sent >= payload.size()) {
                    timer->stop();
                    socket->disconnectFromHost();
                }
            });
            timer->start(route.chunkDelayMs);
            return;
        }
        send(socket, status, reason, headers, payload.left(limit), payload.size());
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
};

// Sends every https request to the local server and refuses to send anything
// anywhere else, so no test can reach the internet.
class RewritingNetwork : public QNetworkAccessManager {
public:
    explicit RewritingNetwork(quint16 port) : m_port(port) {}
    QStringList seen;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoing = nullptr) override {
        QNetworkRequest local(request);
        const QUrl url = request.url();
        seen << url.toString();
        QUrl target;
        target.setScheme("http");
        target.setHost("127.0.0.1");
        if (url.scheme() == "https" && url.host() == "models.example.test") {
            target.setPort(m_port);
            target.setPath(url.path());
        } else if (url.scheme() == "http" && url.host() == "127.0.0.1") {
            target = url;
        } else {
            target.setPort(1); // nothing listens here: any stray request fails
            target.setPath("/refused");
        }
        local.setUrl(target);
        return QNetworkAccessManager::createRequest(operation, local, outgoing);
    }

private:
    quint16 m_port;
};

struct Blob {
    QString key, folder, name;
    QByteArray data;
    QString path() const { return "/files/" + name; }
    QByteArray sha256() const { return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(); }
};

QByteArray pseudoRandom(int size, quint32 seed) {
    QRandomGenerator rng(seed);
    QByteArray data;
    data.reserve(size);
    while (data.size() < size) {
        const quint32 value = rng.generate();
        data.append(reinterpret_cast<const char *>(&value), 4);
    }
    data.truncate(size);
    return data;
}

// A catalogue of small files with real checksums. Model m is the default
// (files a, c, b); m2 shares the encoder c with it (d, c, e); m3 lists one
// file in two roles; u is the upscaler.
struct World {
    QMap<QString, Blob> blobs;
    Catalogue catalogue;
    QByteArray json; // the catalogue as a file, for LIMGEN_CATALOGUE
    const Blob &blob(const QString &key) const { return blobs.constFind(key).value(); }
    CatalogueFile file(const QString &key) const { return *catalogue.file(key); }
    QList<CatalogueFile> files(const QStringList &keys) const {
        QList<CatalogueFile> list;
        for (const QString &key : keys)
            list << file(key);
        return list;
    }
    const ModelLevel &level(const QString &model) const { return catalogue.model(model)->levels.first(); }
};

World makeWorld() {
    World world;
    const struct {
        const char *key, *folder, *name;
        int size;
    } specs[] = {{"a", "fa", "a.bin", 150000}, {"b", "fa", "b.bin", 70000},     {"c", "shared", "c.bin", 100000},
                 {"d", "fd", "d.bin", 60000},  {"e", "fd", "e.bin", 30000},     {"u", "up", "u.bin", 40000}};
    QJsonObject files;
    quint32 seed = 1;
    for (const auto &spec : specs) {
        Blob blob{spec.key, spec.folder, spec.name, pseudoRandom(spec.size, seed++)};
        files[spec.key] = QJsonObject{{"folder", spec.folder},
                                      {"name", spec.name},
                                      {"bytes", spec.size},
                                      {"sha256", QString::fromLatin1(blob.sha256())},
                                      {"url", QString("https://models.example.test/files/%1").arg(spec.name)},
                                      {"licence", "MIT"}};
        world.blobs.insert(spec.key, blob);
    }
    auto model = [](const char *id, bool isDefault, const QJsonObject &levelFiles) {
        return QJsonObject{{"id", id},
                           {"name", id},
                           {"publisher", "Test"},
                           {"default", isDefault},
                           {"licence", QJsonObject{{"id", "MIT"}, {"summary", "s"}, {"url", "https://x.test/l"}}},
                           {"native", QJsonArray{64, 64}},
                           {"steps", 1},
                           {"levels", QJsonArray{QJsonObject{{"id", "q"}, {"minFreeMiB", 1}, {"files", levelFiles}}}}};
    };
    QJsonObject root{{"engine", "x"},
                     {"upscaler", "u"},
                     {"files", files},
                     {"models", QJsonArray{model("m", true, {{"diffusion-model", "a"}, {"llm", "c"}, {"vae", "b"}}),
                                           model("m2", false, {{"diffusion-model", "d"}, {"llm", "c"}, {"vae", "e"}}),
                                           model("m3", false, {{"diffusion-model", "a"}, {"vae", "a"}})}}};
    QString error;
    world.json = QJsonDocument(root).toJson();
    world.catalogue = Catalogue::load(world.json, &error);
    if (!world.catalogue.isValid())
        qFatal("test catalogue: %s", qPrintable(error));
    return world;
}

// What a ModelStore reported.
struct StoreRun {
    bool finished = false, failed = false, cancelled = false;
    QString message;
    QStringList installed;
    QList<qint64> done;
    qint64 total = -1;
    bool over() const { return finished || failed || cancelled; }
};

void watchStore(ModelStore &store, StoreRun &run) {
    QObject::connect(&store, &ModelStore::progress, &store, [&run](qint64 done, qint64 total, const QString &) {
        run.done << done;
        run.total = total;
    });
    QObject::connect(&store, &ModelStore::fileInstalled, &store, [&run](const QString &key) { run.installed << key; });
    QObject::connect(&store, &ModelStore::finished, &store, [&run] { run.finished = true; });
    QObject::connect(&store, &ModelStore::failed, &store, [&run](const QString &message) {
        run.failed = true;
        run.message = message;
    });
    QObject::connect(&store, &ModelStore::cancelled, &store, [&run] { run.cancelled = true; });
}

// Everything one download test needs: a temp models folder, the local server
// serving every file of the world, and a store pointed at it.
struct Rig {
    Fixture fixture;
    HttpServer server;
    RewritingNetwork network;
    World world;
    ModelStore store;
    StoreRun run;

    Rig() : network(server.port()), world(makeWorld()), store(fixture.models, &network) {
        for (const Blob &blob : world.blobs)
            server.serve(blob.path(), blob.data);
        watchStore(store, run);
    }
    QString target(const QString &key) const { return Catalogue::path(world.file(key), fixture.models); }
    QString part(const QString &key) const { return target(key) + ".part"; }
    HttpServer::Route &route(const QString &key) { return server.routes[world.blob(key).path()]; }
    QString route_path(const QString &key) const { return world.blob(key).path(); }
};

#define WAIT_FOR_STORE(rig) QTRY_VERIFY_WITH_TIMEOUT((rig).run.over(), 25000)

} // namespace

// ============================== ModelStore ================================

void LimgenTests::store_freshDownloadInstalls() {
    Rig rig;
    QVERIFY(rig.server.listening);
    rig.store.start(rig.world.files({"a"}));
    QVERIFY(rig.store.isRunning());
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QVERIFY(!rig.store.isRunning());
    QCOMPARE(readFile(rig.target("a")), rig.world.blob("a").data);
    // Only the installed file is left: no .part, nothing else.
    QCOMPARE(filesUnder(rig.fixture.models), QStringList{rig.target("a")});
    QCOMPARE(rig.run.installed, QStringList{"a"});
    QCOMPARE(rig.server.count("/files/a.bin"), 1);
    QCOMPARE(rig.server.rangesOf("/files/a.bin"), QList<QByteArray>{QByteArray()});
    QVERIFY(!rig.run.done.isEmpty());
    QCOMPARE(rig.run.total, qint64(150000));
    QCOMPARE(rig.run.done.last(), qint64(150000));
    for (int i = 1; i < rig.run.done.size(); ++i)
        QVERIFY2(rig.run.done[i] >= rig.run.done[i - 1], "progress never goes backwards");
}

void LimgenTests::store_severalFilesInstallInOrder() {
    Rig rig;
    const ModelStore::Plan plan = rig.store.plan(rig.world.catalogue, rig.world.level("m"));
    QStringList keys;
    for (const CatalogueFile &file : plan.files)
        keys << file.key;
    QCOMPARE(keys, (QStringList{"a", "c", "b", "u"}));
    rig.store.start(plan.files);
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.run.installed, keys);
    QCOMPARE(rig.server.paths(), (QStringList{"/files/a.bin", "/files/c.bin", "/files/b.bin", "/files/u.bin"}));
    for (const QString &key : keys) {
        QCOMPARE(readFile(rig.target(key)), rig.world.blob(key).data);
        QVERIFY(!QFileInfo::exists(rig.part(key)));
    }
    QCOMPARE(rig.run.total, plan.totalBytes);
    QCOMPARE(rig.run.done.last(), plan.totalBytes);
    QCOMPARE(filesUnder(rig.fixture.models).size(), 4);
    // After the install there is nothing left to plan.
    QVERIFY(rig.store.plan(rig.world.catalogue, rig.world.level("m")).files.isEmpty());
}

void LimgenTests::store_resumeSendsRangeAndEndsCorrect() {
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    QVERIFY(writeFile(rig.part("a"), data.left(50000)));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.server.rangesOf("/files/a.bin"), QList<QByteArray>{"bytes=50000-"});
    QCOMPARE(readFile(rig.target("a")), data);
    QVERIFY(!QFileInfo::exists(rig.part("a")));
    QCOMPARE(rig.run.done.first(), qint64(50000)); // progress starts from what is on disk
}

void LimgenTests::store_serverIgnoringRangeStartsOver() {
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    rig.route("a").honourRange = false; // answers 200 with the whole file
    QVERIFY(writeFile(rig.part("a"), data.left(50000)));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.server.rangesOf("/files/a.bin"), QList<QByteArray>{"bytes=50000-"});
    // No duplicated bytes: the file is exactly the served one.
    QCOMPARE(QFileInfo(rig.target("a")).size(), qint64(data.size()));
    QCOMPARE(readFile(rig.target("a")), data);
}

void LimgenTests::store_wrongContentRangeStartsOver() {
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    // The server answers 206, but from offset 0 and says so in Content-Range.
    rig.route("a").rangeStartOverride = 0;
    QVERIFY(writeFile(rig.part("a"), data.left(50000)));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(readFile(rig.target("a")), data);
    QVERIFY(!QFileInfo::exists(rig.part("a")));
}

void LimgenTests::store_completePartIsVerifiedWithoutRequest() {
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    QVERIFY(writeFile(rig.part("a"), data));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.server.requests.size(), 0); // nothing left to ask for
    QCOMPARE(readFile(rig.target("a")), data);
    QVERIFY(!QFileInfo::exists(rig.part("a")));
}

void LimgenTests::store_corruptCompletePartIsRemoved() {
    Rig rig;
    QByteArray bad = rig.world.blob("a").data;
    bad[100] = char(bad[100] ^ 0xff);
    QVERIFY(writeFile(rig.part("a"), bad));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY(rig.run.failed);
    QVERIFY(!rig.run.finished);
    QVERIFY(rig.run.installed.isEmpty());
    QVERIFY(!QFileInfo::exists(rig.part("a")));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QCOMPARE(rig.server.requests.size(), 0);
}

void LimgenTests::store_status416OnShortPartDoesNotInstall() {
    Rig rig;
    rig.route("a").status = 416; // "the range starts at the end"
    QVERIFY(writeFile(rig.part("a"), rig.world.blob("a").data.left(50000)));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    // 416 is taken as "complete" and verified; a short file fails the checksum.
    QCOMPARE(rig.server.count("/files/a.bin"), 1);
    QVERIFY(rig.run.failed);
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QVERIFY(!QFileInfo::exists(rig.part("a")));
    QVERIFY(rig.run.installed.isEmpty());
}

void LimgenTests::store_oversizedPartStartsAgain() {
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    QVERIFY(writeFile(rig.part("a"), pseudoRandom(data.size() + 100, 99)));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.server.rangesOf("/files/a.bin"), QList<QByteArray>{QByteArray()}); // from zero
    QCOMPARE(readFile(rig.target("a")), data);
}

void LimgenTests::store_droppedConnectionIsRetriedAndResumed() {
    Rig rig;
    rig.route("a").dropAfterBytes = 40000; // the first request only
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.server.count("/files/a.bin"), 2);
    const QList<QByteArray> ranges = rig.server.rangesOf("/files/a.bin");
    QCOMPARE(ranges[0], QByteArray());
    QVERIFY2(ranges[1].startsWith("bytes=") && ranges[1].endsWith("-"), ranges[1].constData());
    const qint64 resumedAt = ranges[1].mid(6, ranges[1].size() - 7).toLongLong();
    QVERIFY2(resumedAt > 0 && resumedAt <= 40000, qPrintable(QString::number(resumedAt)));
    QCOMPARE(readFile(rig.target("a")), rig.world.blob("a").data);
    QVERIFY(!QFileInfo::exists(rig.part("a")));
}

void LimgenTests::store_retriesExhaustedFailsAndKeepsPart() {
    Rig rig;
    // Every request fails without delivering a byte, so the retries are not
    // reset: the first try and three retries, then the end.
    rig.route("a").status = 500;
    const QByteArray &data = rig.world.blob("a").data;
    QVERIFY(writeFile(rig.part("a"), data.left(20000)));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY(rig.run.failed);
    QVERIFY(!rig.run.message.isEmpty());
    QVERIFY(!rig.store.isRunning());
    QCOMPARE(rig.server.count("/files/a.bin"), 1 + ModelStore::retries);
    // Each retry resumes from the same place; the .part is exactly as it was.
    QCOMPARE(rig.server.rangesOf("/files/a.bin"),
             (QList<QByteArray>{"bytes=20000-", "bytes=20000-", "bytes=20000-", "bytes=20000-"}));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QCOMPARE(readFile(rig.part("a")), data.left(20000));
}

void LimgenTests::store_finalErrorsFailAtOnce_data() {
    QTest::addColumn<int>("status");
    QTest::newRow("not found") << 404;
    QTest::newRow("forbidden") << 403;
    QTest::newRow("unauthorised") << 401;
    QTest::newRow("gone") << 410;
}

void LimgenTests::store_finalErrorsFailAtOnce() {
    QFETCH(int, status);
    Rig rig;
    rig.route("a").status = status;
    QElapsedTimer timer;
    timer.start();
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY(rig.run.failed);
    QVERIFY(!rig.run.message.isEmpty());
    QCOMPARE(rig.server.count("/files/a.bin"), 1); // no retries
    QVERIFY2(timer.elapsed() < 1900, qPrintable(QString::number(timer.elapsed()))); // and no 2 s wait
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    // The error page was never written anywhere.
    QVERIFY(!QFileInfo::exists(rig.part("a")) || QFileInfo(rig.part("a")).size() == 0);
    for (const QString &path : filesUnder(rig.fixture.models))
        QVERIFY2(!readFile(path).contains("error page"), qPrintable(path));
}

void LimgenTests::store_checksumMismatchRemovesPart() {
    Rig rig;
    QByteArray bad = rig.world.blob("a").data;
    bad[1000] = char(bad[1000] ^ 1); // same size, one flipped bit
    rig.route("a").body = bad;
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY(rig.run.failed);
    QVERIFY(rig.run.message.contains("a.bin"));
    QVERIFY(!QFileInfo::exists(rig.part("a")));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QVERIFY(rig.run.installed.isEmpty());
    QCOMPARE(rig.server.count("/files/a.bin"), 1); // not retried
}

void LimgenTests::store_oversizeBodyIsRemoved() {
    Rig rig;
    rig.route("a").body = rig.world.blob("a").data + QByteArray(5000, 'x');
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY(rig.run.failed);
    QVERIFY(!rig.store.isRunning());
    QVERIFY(!QFileInfo::exists(rig.part("a")));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QCOMPARE(rig.server.count("/files/a.bin"), 1);
}

void LimgenTests::store_redirectIsFollowed() {
    Rig rig;
    rig.server.serve("/real/a.bin", rig.world.blob("a").data);
    rig.route("a").redirectTo = QString("http://127.0.0.1:%1/real/a.bin").arg(rig.server.port());
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.server.paths(), (QStringList{"/files/a.bin", "/real/a.bin"}));
    QCOMPARE(readFile(rig.target("a")), rig.world.blob("a").data);
    QVERIFY(!QFileInfo::exists(rig.part("a")));
}

void LimgenTests::store_redirectKeepsRange() {
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    rig.server.serve("/real/a.bin", data);
    rig.route("a").redirectTo = QString("http://127.0.0.1:%1/real/a.bin").arg(rig.server.port());
    QVERIFY(writeFile(rig.part("a"), data.left(50000)));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    // The resume survives the redirect: the final server was asked for the rest only.
    QCOMPARE(rig.server.rangesOf("/real/a.bin"), QList<QByteArray>{"bytes=50000-"});
    QCOMPARE(readFile(rig.target("a")), data);
}

void LimgenTests::store_wrongSizedTargetIsReplaced() {
    Rig rig;
    QVERIFY(writeFile(rig.target("a"), "short and wrong"));
    const ModelStore::Plan plan = rig.store.plan(rig.world.catalogue, rig.world.level("m3"), false);
    QCOMPARE(plan.files.size(), 1); // a wrong-sized file counts as not installed
    rig.store.start(plan.files);
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(readFile(rig.target("a")), rig.world.blob("a").data);
    QCOMPARE(filesUnder(rig.fixture.models), QStringList{rig.target("a")});
}

void LimgenTests::store_cancelKeepsPartAndNextStartResumes() {
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    HttpServer::Route &route = rig.route("a");
    route.chunk = 10000; // 1.5 s for the whole file
    route.chunkDelayMs = 100;
    rig.store.start(rig.world.files({"a"}));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(rig.part("a")).size() > 0, 10000);
    rig.store.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(rig.run.over(), 10000);
    QVERIFY(rig.run.cancelled);
    QVERIFY(!rig.run.failed);
    QVERIFY(!rig.run.finished);
    QVERIFY(!rig.store.isRunning());
    const qint64 kept = QFileInfo(rig.part("a")).size();
    QVERIFY2(kept > 0 && kept < data.size(), qPrintable(QString::number(kept)));
    QCOMPARE(readFile(rig.part("a")), data.left(kept));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QTest::qWait(300); // nothing more is written after the cancel
    QCOMPARE(QFileInfo(rig.part("a")).size(), kept);

    // The next start resumes from there, at full speed this time.
    route.chunk = 0;
    rig.run = StoreRun();
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.server.rangesOf("/files/a.bin"),
             (QList<QByteArray>{"", "bytes=" + QByteArray::number(kept) + "-"}));
    QCOMPARE(readFile(rig.target("a")), data);
    QVERIFY(!QFileInfo::exists(rig.part("a")));
}

void LimgenTests::store_cancelBeforeStartDoesNothing() {
    Rig rig;
    rig.store.cancel();
    QTest::qWait(100);
    QVERIFY(!rig.run.over());
    QVERIFY(!rig.store.isRunning());
    // And it does not leave a cancel pending for the next start.
    rig.store.start(rig.world.files({"b"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QVERIFY(!rig.run.cancelled);
    QCOMPARE(readFile(rig.target("b")), rig.world.blob("b").data);
}

void LimgenTests::store_startWhileRunningIsIgnored() {
    Rig rig;
    rig.store.start(rig.world.files({"a"}));
    rig.store.start(rig.world.files({"b"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.run.installed, QStringList{"a"});
    QCOMPARE(rig.server.count("/files/b.bin"), 0);
    QVERIFY(!QFileInfo::exists(rig.target("b")));
}

void LimgenTests::store_neverReachesBeyondLocalhost() {
    Rig rig;
    rig.store.start(rig.world.files({"a", "b"}));
    WAIT_FOR_STORE(rig);
    QVERIFY(rig.run.finished);
    // Every request the store made was for the catalogue's host, which the
    // test network turned into the local server.
    QVERIFY(!rig.network.seen.isEmpty());
    for (const QString &url : rig.network.seen)
        QVERIFY2(url.startsWith("https://models.example.test/files/"), qPrintable(url));
}

// ============================== ModelStore::plan ==========================

void LimgenTests::plan_freshListsLevelFilesThenUpscaler() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const World world = makeWorld();
    const ModelStore store(fixture.models);
    const ModelStore::Plan plan = store.plan(world.catalogue, world.level("m"));
    QStringList keys;
    for (const CatalogueFile &file : plan.files)
        keys << file.key;
    QCOMPARE(keys, (QStringList{"a", "c", "b", "u"})); // level files in role order, the upscaler last
    QCOMPARE(plan.totalBytes, qint64(150000 + 100000 + 70000 + 40000));
    QCOMPARE(plan.partBytes, qint64(0));
    QCOMPARE(plan.downloadBytes(), plan.totalBytes);
    QVERIFY(plan.availableBytes > 0);
    QVERIFY(!QFileInfo::exists(fixture.models)); // planning creates nothing
}

void LimgenTests::plan_upscalerOptional() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const World world = makeWorld();
    const ModelStore store(fixture.models);
    ModelStore::Plan plan = store.plan(world.catalogue, world.level("m"), false);
    QCOMPARE(plan.files.size(), 3);
    for (const CatalogueFile &file : plan.files)
        QVERIFY(file.key != "u");
    QCOMPARE(plan.totalBytes, qint64(320000));
    // An installed upscaler is not planned either.
    QVERIFY(makeSparse(Catalogue::path(world.file("u"), fixture.models), 40000));
    plan = store.plan(world.catalogue, world.level("m"), true);
    QCOMPARE(plan.files.size(), 3);
}

void LimgenTests::plan_installedFilesAreSkipped() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const World world = makeWorld();
    const ModelStore store(fixture.models);
    QVERIFY(makeSparse(Catalogue::path(world.file("a"), fixture.models), 150000));
    QVERIFY(makeSparse(Catalogue::path(world.file("b"), fixture.models), 70000 - 1)); // wrong size: still planned
    const ModelStore::Plan plan = store.plan(world.catalogue, world.level("m"), false);
    QStringList keys;
    for (const CatalogueFile &file : plan.files)
        keys << file.key;
    QCOMPARE(keys, (QStringList{"c", "b"}));
    QCOMPARE(plan.totalBytes, qint64(170000));
}

void LimgenTests::plan_sharedFileOfInstalledModelIsSkipped() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const World world = makeWorld();
    const ModelStore store(fixture.models);
    for (const QString &key : world.level("m").files)
        QVERIFY(makeSparse(Catalogue::path(world.file(key), fixture.models), world.file(key).bytes));
    const ModelStore::Plan plan = store.plan(world.catalogue, world.level("m2"), false);
    QStringList keys;
    for (const CatalogueFile &file : plan.files)
        keys << file.key;
    QCOMPARE(keys, (QStringList{"d", "e"})); // the encoder c is already there
    QCOMPARE(plan.totalBytes, qint64(90000));
}

void LimgenTests::plan_sameFileTwiceInLevelIsPlannedOnce() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const World world = makeWorld();
    const ModelStore store(fixture.models);
    const ModelStore::Plan plan = store.plan(world.catalogue, world.level("m3"), false);
    QCOMPARE(plan.files.size(), 1);
    QCOMPARE(plan.totalBytes, qint64(150000));
}

void LimgenTests::plan_partBytes() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const World world = makeWorld();
    const ModelStore store(fixture.models);
    const QString partA = Catalogue::path(world.file("a"), fixture.models) + ".part";
    const QString partC = Catalogue::path(world.file("c"), fixture.models) + ".part";
    const QString partB = Catalogue::path(world.file("b"), fixture.models) + ".part";
    QVERIFY(writeFile(partA, QByteArray(50000, 'x')));
    QVERIFY(writeFile(partC, QByteArray(100000, 'x'))); // complete: counts in full
    QVERIFY(writeFile(partB, QByteArray(70001, 'x')));  // larger than the file: wrong, so not counted
    ModelStore::Plan plan = store.plan(world.catalogue, world.level("m"), false);
    QCOMPARE(plan.partBytes, qint64(150000));
    QCOMPARE(plan.totalBytes, qint64(320000));
    QCOMPARE(plan.downloadBytes(), qint64(170000));
    // A .part beside an installed file is not counted.
    QVERIFY(makeSparse(Catalogue::path(world.file("a"), fixture.models), 150000));
    plan = store.plan(world.catalogue, world.level("m"), false);
    QCOMPARE(plan.partBytes, qint64(100000));
}

void LimgenTests::plan_availableBytes() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const qint64 here = ModelStore::availableBytes(fixture.root.path());
    QVERIFY(here > 0);
    // A folder that does not exist yet is measured at its nearest existing parent.
    const qint64 below = ModelStore::availableBytes(fixture.root.path() + "/not/yet/there");
    QVERIFY(below > 0);
    QVERIFY(qAbs(below - here) < qint64(64) * 1024 * 1024);
    QVERIFY(!QFileInfo::exists(fixture.root.path() + "/not"));
}

void LimgenTests::plan_fitsOnDisk() {
    QCOMPARE(ModelStore::headroomBytes, qint64(256) * 1024 * 1024);
    ModelStore::Plan plan;
    plan.totalBytes = 1000;
    plan.partBytes = 200; // 800 left to download
    plan.availableBytes = -1;
    QVERIFY(plan.fitsOnDisk()); // unknown: do not refuse
    plan.availableBytes = 0;
    QVERIFY(!plan.fitsOnDisk());
    plan.availableBytes = 800; // enough for the file, but no headroom
    QVERIFY(!plan.fitsOnDisk());
    plan.availableBytes = 800 + ModelStore::headroomBytes - 1;
    QVERIFY(!plan.fitsOnDisk());
    plan.availableBytes = 800 + ModelStore::headroomBytes;
    QVERIFY(plan.fitsOnDisk());
    plan.partBytes = 1000; // nothing left: the headroom alone is still asked for
    plan.availableBytes = ModelStore::headroomBytes - 1;
    QVERIFY(!plan.fitsOnDisk());
    plan.availableBytes = ModelStore::headroomBytes;
    QVERIFY(plan.fitsOnDisk());
}

// ============================== CLI: models ===============================

namespace {

qint64 sizeOfMissing(const Catalogue &catalogue, const QString &model, const QString &installedModel) {
    qint64 bytes = 0;
    const ModelLevel &installed = catalogue.model(installedModel)->levels.first();
    for (const QString &key : catalogue.model(model)->levels.first().files)
        if (!installed.files.values().contains(key))
            bytes += catalogue.file(key)->bytes;
    return bytes;
}

} // namespace

void LimgenTests::cli_modelsListText() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    for (const QStringList &arguments : {QStringList{"models"}, QStringList{"models", "list"}}) {
        const CliResult result = runCli(fixture, arguments, engineEnvironment(fixture));
        QCOMPARE(result.exitCode, 0);
        QCOMPARE(result.err, QString());
        QVERIFY2(result.out.contains(fixture.models), qPrintable(result.out));
        for (const CatalogueModel &model : catalogue.models()) {
            QVERIFY2(result.out.contains(model.id), qPrintable(model.id));
            QVERIFY(result.out.contains(model.licenceId));
        }
    }
    QCOMPARE(filesUnder(fixture.models), QStringList()); // listing writes nothing
}

void LimgenTests::cli_modelsListJson() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models)); // without the upscaler
    const CliResult result = runCli(fixture, {"models", "--json"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.err, QString());
    const QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["modelsDirectory"].toString(), fixture.models);
    const QJsonArray models = json["models"].toArray();
    QCOMPARE(models.size(), catalogue.models().size());
    QJsonObject zImage, klein;
    for (const QJsonValue &value : models) {
        const QJsonObject model = value.toObject();
        if (model["id"].toString() == "z-image-turbo")
            zImage = model;
        if (model["id"].toString() == "flux2-klein-4b")
            klein = model;
    }
    QCOMPARE(zImage["default"].toBool(), true);
    QCOMPARE(klein["default"].toBool(), false);
    QCOMPARE(zImage["licence"].toObject()["id"].toString(), catalogue.model("z-image-turbo")->licenceId);
    QVERIFY(!zImage["name"].toString().isEmpty());
    QVERIFY(!zImage["publisher"].toString().isEmpty());
    const QJsonObject zLevel = zImage["levels"].toArray()[0].toObject();
    const QJsonObject kLevel = klein["levels"].toArray()[0].toObject();
    QCOMPARE(zLevel["id"].toString(), catalogue.model("z-image-turbo")->levels.first().id);
    QCOMPARE(zLevel["installed"].toBool(), true);
    QCOMPARE(zLevel["downloadBytes"].toDouble(), 0.0);
    QCOMPARE(kLevel["installed"].toBool(), false);
    // The shared text encoder is installed, so only klein's own files are left.
    QCOMPARE(qint64(kLevel["downloadBytes"].toDouble()), sizeOfMissing(catalogue, "flux2-klein-4b", "z-image-turbo"));
    QCOMPARE(kLevel["minFreeMiB"].toInt(), catalogue.model("flux2-klein-4b")->levels.first().minFreeMiB);
    QCOMPARE(kLevel["runsHere"].toBool(), true); // 10000 MiB free in the fake GPU
    // The same in a run with nothing installed: everything is to download.
    Fixture empty;
    QVERIFY(empty.ok());
    const QJsonObject all = jsonObject(runCli(empty, {"models", "--json"}, engineEnvironment(empty)).out);
    qint64 zTotal = 0;
    for (const QString &key : catalogue.model("z-image-turbo")->levels.first().files.values())
        zTotal += catalogue.file(key)->bytes;
    for (const QJsonValue &value : all["models"].toArray())
        if (value.toObject()["id"].toString() == "z-image-turbo") {
            const QJsonObject level = value.toObject()["levels"].toArray()[0].toObject();
            QCOMPARE(level["installed"].toBool(), false);
            QCOMPARE(qint64(level["downloadBytes"].toDouble()), zTotal);
        }
}

void LimgenTests::cli_modelsRunsHereFollowsFreeMemory() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    auto runsHere = [&](const QHash<QString, QString> &environment) -> QJsonValue {
        const CliResult result = runCli(fixture, {"models", "--json"}, environment);
        return jsonObject(result.out)["models"].toArray()[0].toObject()["levels"].toArray()[0].toObject()["runsHere"];
    };
    QVERIFY(runsHere(engineEnvironment(fixture, 2000, 12000)).isBool());
    QCOMPARE(runsHere(engineEnvironment(fixture, 2000, 12000)).toBool(), true);
    // 3000 MiB free is below the 5000 the model needs.
    const QJsonValue low = runsHere(engineEnvironment(fixture, 9000, 12000));
    QVERIFY(low.isBool());
    QCOMPARE(low.toBool(), false);
    // No GPU reading at all: unknown, not false.
    QHash<QString, QString> none = engineEnvironment(fixture);
    none["LIMGEN_NVIDIA_SMI"] = fixture.root.path() + "/no-nvidia-smi";
    QVERIFY(runsHere(none).isNull());
}

void LimgenTests::cli_modelsUsageErrors_data() {
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("list with a model") << QStringList{"models", "list", "extra"};
    QTest::newRow("list with --yes") << QStringList{"models", "list", "--yes"};
    QTest::newRow("list with --level") << QStringList{"models", "list", "--level", "q4_k"};
    QTest::newRow("unknown action") << QStringList{"models", "bogus"};
    QTest::newRow("too many arguments") << QStringList{"models", "pull", "z-image-turbo", "extra"};
    QTest::newRow("pull without id") << QStringList{"models", "pull"};
    QTest::newRow("pull unknown model") << QStringList{"models", "pull", "nope", "--yes"};
    QTest::newRow("pull unknown level") << QStringList{"models", "pull", "z-image-turbo", "--level", "nope", "--yes"};
    QTest::newRow("unknown option") << QStringList{"models", "--bogus"};
}

void LimgenTests::cli_modelsUsageErrors() {
    REQUIRE_BINARY();
    QFETCH(QStringList, arguments);
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult result = runCli(fixture, arguments, engineEnvironment(fixture));
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 2);
    QCOMPARE(result.out, QString());
    QVERIFY(!result.err.isEmpty());
    QVERIFY(!QFileInfo::exists(fixture.models + "/.download.lock"));
    QCOMPARE(filesUnder(fixture.models), QStringList());
}

void LimgenTests::cli_modelsPullAlreadyInstalled() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    QVERIFY(installModel(catalogue, "z-image-turbo", fixture.models));
    QVERIFY(makeSparse(Catalogue::path(*catalogue.upscaler(), fixture.models), catalogue.upscaler()->bytes));
    const QStringList before = filesUnder(fixture.models);
    // Without --yes too: nothing to download means nothing to ask.
    CliResult result = runCli(fixture, {"models", "pull", "z-image-turbo"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.out, QString());
    result = runCli(fixture, {"models", "pull", "z-image-turbo", "--yes", "--json"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 0);
    const QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["model"].toString(), QString("z-image-turbo"));
    QCOMPARE(json["level"].toString(), catalogue.model("z-image-turbo")->levels.first().id);
    QVERIFY(json["installed"].isArray());
    QCOMPARE(json["installed"].toArray().size(), 0);
    QCOMPARE(filesUnder(fixture.models), before);
    QVERIFY(!QFileInfo::exists(fixture.models + "/.download.lock"));
}

void LimgenTests::cli_modelsPullWithoutYesAsksNothingAndWritesNothing() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Catalogue catalogue = Catalogue::builtIn();
    const CatalogueModel &model = *catalogue.model("z-image-turbo");
    // The shared text encoder is already installed.
    const CatalogueFile &encoder = *catalogue.file(model.levels.first().files.value("llm"));
    QVERIFY(makeSparse(Catalogue::path(encoder, fixture.models), encoder.bytes));
    const QStringList before = filesUnder(fixture.models);
    const CliResult result = runCli(fixture, {"models", "pull", "z-image-turbo"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 2); // stdin is not a terminal and there is no --yes
    QCOMPARE(result.out, QString());
    // What the user would have agreed to is shown: the licence and each file still needed.
    QVERIFY2(result.err.contains(model.licenceId), qPrintable(result.err));
    for (const QString &key : model.levels.first().files)
        if (key != encoder.key)
            QVERIFY2(result.err.contains(describeFile(*catalogue.file(key))), qPrintable(key));
    QVERIFY2(result.err.contains(describeFile(*catalogue.upscaler())), "the upscaler is part of the pull");
    QVERIFY2(!result.err.contains(describeFile(encoder)), "an installed file is not listed");
    // Nothing was written, no lock either.
    QCOMPARE(filesUnder(fixture.models), before);
    QVERIFY(!QFileInfo::exists(fixture.models + "/.download.lock"));
    // The same with --json.
    const CliResult json = runCli(fixture, {"models", "pull", "z-image-turbo", "--json"}, engineEnvironment(fixture));
    QCOMPARE(json.exitCode, 2);
    QCOMPARE(json.out, QString());
    QCOMPARE(filesUnder(fixture.models), before);
}

void LimgenTests::cli_modelsPullHeldLock() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(QDir().mkpath(fixture.models));
    QLockFile lock(fixture.models + "/.download.lock");
    QVERIFY(lock.tryLock(0));
    const CliResult result = runCli(fixture, {"models", "pull", "z-image-turbo", "--yes"}, engineEnvironment(fixture));
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 1);
    QCOMPARE(result.out, QString());
    QVERIFY2(result.err.contains(fixture.models), qPrintable(result.err));
    // The lock stayed ours, and no download was started: no .part, no folder.
    // Were the lock check gone, the pull would try to download from the port
    // LIMGEN_DOWNLOAD_BASE names, where nothing listens, so even then it could
    // not reach the internet; it would still leave a .part file behind.
    QVERIFY(QFileInfo::exists(fixture.models + "/.download.lock"));
    QCOMPARE(filesUnder(fixture.models), QStringList());
    QCOMPARE(QDir(fixture.models).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList());
    QVERIFY(lock.isLocked());
}

void LimgenTests::cli_notInstalledMessagesNameThePullCommand() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult doctor = runCli(fixture, {"doctor", "--json"}, engineEnvironment(fixture));
    QCOMPARE(doctor.exitCode, 3);
    const QJsonArray problems = jsonObject(doctor.out)["problems"].toArray();
    QCOMPARE(problems.size(), 1);
    QVERIFY2(problems[0].toString().contains("limgen models pull z-image-turbo"), qPrintable(problems[0].toString()));
    const CliResult generate = runCli(fixture, {"generate", "a fox"}, engineEnvironment(fixture));
    QCOMPARE(generate.exitCode, 3);
    QVERIFY2(generate.err.contains("limgen models pull z-image-turbo"), qPrintable(generate.err));
    const CliResult klein = runCli(fixture, {"generate", "a fox", "--model", "flux2-klein-4b"}, engineEnvironment(fixture));
    QCOMPARE(klein.exitCode, 3);
    QVERIFY2(klein.err.contains("limgen models pull flux2-klein-4b"), qPrintable(klein.err));
}

void LimgenTests::cli_helpModels() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult summary = runCli(fixture, {"help"});
    QCOMPARE(summary.exitCode, 0);
    QVERIFY(summary.out.contains("models"));
    const CliResult models = runCli(fixture, {"help", "models"});
    QCOMPARE(models.exitCode, 0);
    for (const QString &option : QStringList{"--level", "--yes", "--json"})
        QVERIFY2(models.out.contains(option), qPrintable(option));
}

// ===================== L3 review: behaviour changes, end to end ==========

namespace {

// Runs the built limgen without blocking the test's event loop, which also
// serves the local HTTP server the child downloads from.
std::unique_ptr<QProcess> startProcess(const QProcessEnvironment &environment, const QString &program,
                                       const QStringList &arguments) {
    auto process = std::make_unique<QProcess>();
    process->setProcessEnvironment(environment);
    process->start(program, arguments);
    return process;
}

QString shellQuote(const QString &text) { return "'" + QString(text).replace("'", "'\\''") + "'"; }

// A temp models folder, a local server holding the test catalogue's files,
// and the environment that points a limgen process at both.
struct PullRig {
    Fixture fixture;
    HttpServer server;
    World world;
    QString cataloguePath;

    PullRig() : world(makeWorld()) {
        for (const Blob &blob : world.blobs)
            server.serve(blob.path(), blob.data);
        cataloguePath = fixture.root.path() + "/catalogue.json";
        writeFile(cataloguePath, world.json);
    }
    QHash<QString, QString> environment() const {
        QHash<QString, QString> env = engineEnvironment(fixture);
        env["LIMGEN_CATALOGUE"] = cataloguePath;
        env["LIMGEN_DOWNLOAD_BASE"] = QString("http://127.0.0.1:%1").arg(server.port());
        return env;
    }
    QString target(const QString &key) const { return Catalogue::path(world.file(key), fixture.models); }
    QString part(const QString &key) const { return target(key) + ".part"; }
    HttpServer::Route &route(const QString &key) { return server.routes[world.blob(key).path()]; }
    QStringList planned() const { return {target("a"), target("c"), target("b"), target("u")}; }
};

#define WAIT_FOR_PROCESS(process) QTRY_VERIFY_WITH_TIMEOUT((process)->state() == QProcess::NotRunning, 40000)

// A text pull driven through a pseudo-terminal made by script(1), so the
// y/N prompt is real: isatty(stdin) is true. Everything the child prints
// arrives on one stream.
struct PtyPull {
    std::unique_ptr<QProcess> process;
    QByteArray output;
    void start(const QProcessEnvironment &environment, const QString &model) {
        const QString command = shellQuote(qEnvironmentVariable("LIMGEN_BINARY")) + " models pull " + model;
        process = startProcess(environment, "script", {"-qec", command, "/dev/null"});
        QObject::connect(process.get(), &QProcess::readyReadStandardOutput, process.get(),
                         [this] { output += process->readAllStandardOutput(); });
    }
    bool waitForPrompt() {
        for (int i = 0; i < 400 && !output.contains("[y/N]"); ++i)
            QTest::qWait(50);
        return output.contains("[y/N]");
    }
};

} // namespace

void LimgenTests::store_dropEveryRequestStillFinishes() {
    Rig rig;
    // Every request ends after 30000 bytes, so the file takes 5 requests and 4 drops: one
    // more than the retries. Bytes arrived each time, so the retries start afresh.
    rig.route("a").shortBytes = 30000;
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.server.rangesOf("/files/a.bin"),
             (QList<QByteArray>{"", "bytes=30000-", "bytes=60000-", "bytes=90000-", "bytes=120000-"}));
    QCOMPARE(readFile(rig.target("a")), rig.world.blob("a").data);
    QVERIFY(!QFileInfo::exists(rig.part("a")));
}

void LimgenTests::store_redirectLoopFailsWithoutRetries() {
    Rig rig;
    rig.route("a").redirectTo = QString("http://127.0.0.1:%1/files/a.bin").arg(rig.server.port());
    QElapsedTimer timer;
    timer.start();
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QVERIFY(rig.run.failed);
    QVERIFY(!rig.run.message.isEmpty());
    // One round of redirects, then stop; a retry would wait two seconds first.
    QVERIFY2(timer.elapsed() < 1900, qPrintable(QString::number(timer.elapsed())));
    QVERIFY2(rig.server.requests.size() <= 60, qPrintable(QString::number(rig.server.requests.size())));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QVERIFY(!QFileInfo::exists(rig.part("a")) || QFileInfo(rig.part("a")).size() == 0);
}

void LimgenTests::store_unreadableCompletePartFailsAndIsKept() {
    if (geteuid() == 0)
        QSKIP("root can read any file, so there is nothing to refuse");
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    QVERIFY(writeFile(rig.part("a"), data));
    QVERIFY(QFile::setPermissions(rig.part("a"), QFileDevice::Permissions()));
    rig.store.start(rig.world.files({"a"}));
    WAIT_FOR_STORE(rig);
    QFile::setPermissions(rig.part("a"), QFile::ReadOwner | QFile::WriteOwner);
    QVERIFY(rig.run.failed);
    QVERIFY2(rig.run.message.contains("Cannot read"), qPrintable(rig.run.message));
    // A file that cannot be read is not known to be bad, so it stays.
    QVERIFY(QFileInfo::exists(rig.part("a")));
    QCOMPARE(readFile(rig.part("a")), data);
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QVERIFY(rig.run.installed.isEmpty());
    QCOMPARE(rig.server.requests.size(), 0);
}

void LimgenTests::store_cancelBetweenHashAndVerifiedEmitsOnce() {
    Rig rig;
    const QByteArray &data = rig.world.blob("a").data;
    QVERIFY(writeFile(rig.part("a"), data)); // complete: start() goes straight to hashing
    QSignalSpy cancelled(&rig.store, &ModelStore::cancelled);
    rig.store.start(rig.world.files({"a"}));
    // The hash of 150 KB is done long before this returns, but the event loop
    // has not run, so verified() has not: the window the cancel lands in.
    QThread::msleep(300);
    rig.store.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(cancelled.count() >= 1, 5000);
    QTest::qWait(300); // a second emission would arrive by now
    QCOMPARE(cancelled.count(), 1);
    QVERIFY(!rig.store.isRunning());
    // A complete, checked file is installed even though a cancel came in.
    QCOMPARE(rig.run.installed, QStringList{"a"});
    QCOMPARE(readFile(rig.target("a")), data);
    QVERIFY(!rig.run.finished);
    QVERIFY(!rig.run.failed);
}

void LimgenTests::store_cancelFromProgressDuringVerifyEmitsOnce() {
    Rig rig;
    QVERIFY(writeFile(rig.part("a"), rig.world.blob("a").data));
    QSignalSpy cancelled(&rig.store, &ModelStore::cancelled);
    // The progress signal of verify() runs inside start(), before hashing.
    QObject::connect(&rig.store, &ModelStore::progress, &rig.store, [&rig](qint64, qint64, const QString &) {
        if (rig.run.done.size() == 1) // the first report, seen by the recorder before this slot
            rig.store.cancel();
    });
    rig.store.start(rig.world.files({"a"}));
    QTRY_VERIFY_WITH_TIMEOUT(cancelled.count() >= 1, 5000);
    QTest::qWait(300);
    QCOMPARE(cancelled.count(), 1);
    QVERIFY(!rig.store.isRunning());
}

void LimgenTests::cli_pullEndToEnd() {
    REQUIRE_BINARY();
    PullRig rig;
    QVERIFY(rig.server.listening);
    const QProcessEnvironment environment = cliEnvironment(rig.fixture, rig.environment());
    auto process = startProcess(environment, qEnvironmentVariable("LIMGEN_BINARY"), {"models", "pull", "m", "--yes"});
    WAIT_FOR_PROCESS(process);
    const QString out = QString::fromUtf8(process->readAllStandardOutput());
    const QString err = QString::fromUtf8(process->readAllStandardError());
    QCOMPARE(process->exitCode(), 0);
    // stdout lists what was installed, in plan order, including the upscaler.
    QCOMPARE(out.split('\n', Qt::SkipEmptyParts), rig.planned());
    QVERIFY2(err.contains("MIT"), qPrintable(err)); // the licence was shown
    for (const QString &key : QStringList{"a", "c", "b", "u"}) {
        QCOMPARE(readFile(rig.target(key)), rig.world.blob(key).data);
        QVERIFY(!QFileInfo::exists(rig.part(key)));
    }
    QVERIFY(!QFileInfo::exists(rig.fixture.models + "/.download.lock"));
    QCOMPARE(rig.server.paths(), (QStringList{"/files/a.bin", "/files/c.bin", "/files/b.bin", "/files/u.bin"}));
    QCOMPARE(filesUnder(rig.fixture.models).size(), 4);
}

void LimgenTests::cli_pullEndToEndJson() {
    REQUIRE_BINARY();
    PullRig rig;
    const QProcessEnvironment environment = cliEnvironment(rig.fixture, rig.environment());
    auto process = startProcess(environment, qEnvironmentVariable("LIMGEN_BINARY"),
                                {"models", "pull", "m2", "--yes", "--json"});
    WAIT_FOR_PROCESS(process);
    QCOMPARE(process->exitCode(), 0);
    const QJsonObject json = jsonObject(QString::fromUtf8(process->readAllStandardOutput()));
    QCOMPARE(json["model"].toString(), QString("m2"));
    QCOMPARE(json["level"].toString(), QString("q"));
    QStringList installed;
    for (const QJsonValue &value : json["installed"].toArray())
        installed << value.toString();
    // m2 is d, c, e plus the upscaler, in the level's role order.
    QCOMPARE(installed, (QStringList{rig.target("d"), rig.target("c"), rig.target("e"), rig.target("u")}));
    for (const QString &key : QStringList{"d", "c", "e", "u"})
        QCOMPARE(readFile(rig.target(key)), rig.world.blob(key).data);
    QVERIFY(!QFileInfo::exists(rig.fixture.models + "/.download.lock"));
}

void LimgenTests::cli_pullSecondTimeDownloadsNothing() {
    REQUIRE_BINARY();
    PullRig rig;
    const QProcessEnvironment environment = cliEnvironment(rig.fixture, rig.environment());
    const QString binary = qEnvironmentVariable("LIMGEN_BINARY");
    auto first = startProcess(environment, binary, {"models", "pull", "m", "--yes"});
    WAIT_FOR_PROCESS(first);
    QCOMPARE(first->exitCode(), 0);
    const int requests = rig.server.requests.size();
    QCOMPARE(requests, 4);
    // Again: nothing to download, nothing to ask, nothing requested.
    auto second = startProcess(environment, binary, {"models", "pull", "m"});
    WAIT_FOR_PROCESS(second);
    QCOMPARE(second->exitCode(), 0);
    QCOMPARE(QString::fromUtf8(second->readAllStandardOutput()), QString());
    QVERIFY(!QString::fromUtf8(second->readAllStandardError()).isEmpty()); // says it is installed
    auto third = startProcess(environment, binary, {"models", "pull", "m", "--json"});
    WAIT_FOR_PROCESS(third);
    QCOMPARE(third->exitCode(), 0);
    QCOMPARE(jsonObject(QString::fromUtf8(third->readAllStandardOutput()))["installed"].toArray().size(), 0);
    QCOMPARE(rig.server.requests.size(), requests);
}

void LimgenTests::cli_pullInterruptedResumes() {
    REQUIRE_BINARY();
    PullRig rig;
    const QByteArray &data = rig.world.blob("a").data;
    HttpServer::Route &route = rig.route("a");
    route.chunk = 10000; // 1.5 s for the file
    route.chunkDelayMs = 100;
    const QProcessEnvironment environment = cliEnvironment(rig.fixture, rig.environment());
    const QString binary = qEnvironmentVariable("LIMGEN_BINARY");
    auto first = startProcess(environment, binary, {"models", "pull", "m", "--yes"});
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(rig.part("a")).size() > 0, 20000);
    QVERIFY(::kill(pid_t(first->processId()), SIGINT) == 0);
    WAIT_FOR_PROCESS(first);
    QCOMPARE(first->exitStatus(), QProcess::NormalExit);
    QCOMPARE(first->exitCode(), 130);
    QCOMPARE(QString::fromUtf8(first->readAllStandardOutput()), QString());
    QVERIFY2(QString::fromUtf8(first->readAllStandardError()).contains("Paused"), "paused is said on stderr");
    const qint64 kept = QFileInfo(rig.part("a")).size();
    QVERIFY2(kept > 0 && kept < data.size(), qPrintable(QString::number(kept)));
    QCOMPARE(readFile(rig.part("a")), data.left(kept));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QVERIFY(!QFileInfo::exists(rig.fixture.models + "/.download.lock")); // released on the way out

    // The rerun, at full speed, asks for the rest and finishes.
    route.chunk = 0;
    auto second = startProcess(environment, binary, {"models", "pull", "m", "--yes"});
    WAIT_FOR_PROCESS(second);
    QCOMPARE(second->exitCode(), 0);
    QCOMPARE(rig.server.rangesOf("/files/a.bin"), (QList<QByteArray>{"", "bytes=" + QByteArray::number(kept) + "-"}));
    for (const QString &key : QStringList{"a", "c", "b", "u"}) {
        QCOMPARE(readFile(rig.target(key)), rig.world.blob(key).data);
        QVERIFY(!QFileInfo::exists(rig.part(key)));
    }
}

void LimgenTests::cli_pullNotFoundFails() {
    REQUIRE_BINARY();
    PullRig rig;
    rig.route("a").status = 404;
    const QProcessEnvironment environment = cliEnvironment(rig.fixture, rig.environment());
    auto process = startProcess(environment, qEnvironmentVariable("LIMGEN_BINARY"), {"models", "pull", "m", "--yes"});
    WAIT_FOR_PROCESS(process);
    QCOMPARE(process->exitCode(), 1);
    QCOMPARE(QString::fromUtf8(process->readAllStandardOutput()), QString());
    QVERIFY2(QString::fromUtf8(process->readAllStandardError()).contains("a.bin"), "the message names the file");
    QCOMPARE(rig.server.paths(), QStringList{"/files/a.bin"}); // stopped at once, nothing else asked for
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QVERIFY(!QFileInfo::exists(rig.fixture.models + "/.download.lock"));
    for (const QString &path : filesUnder(rig.fixture.models))
        QVERIFY2(!readFile(path).contains("error page"), qPrintable(path));
}

void LimgenTests::cli_pullChecksumMismatchFails() {
    REQUIRE_BINARY();
    PullRig rig;
    QByteArray bad = rig.world.blob("a").data;
    bad[500] = char(bad[500] ^ 1);
    rig.route("a").body = bad;
    const QProcessEnvironment environment = cliEnvironment(rig.fixture, rig.environment());
    auto process = startProcess(environment, qEnvironmentVariable("LIMGEN_BINARY"), {"models", "pull", "m", "--yes"});
    WAIT_FOR_PROCESS(process);
    QCOMPARE(process->exitCode(), 1);
    QCOMPARE(QString::fromUtf8(process->readAllStandardOutput()), QString());
    QVERIFY(QString::fromUtf8(process->readAllStandardError()).contains("a.bin"));
    QVERIFY(!QFileInfo::exists(rig.part("a")));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QCOMPARE(filesUnder(rig.fixture.models), QStringList());
}

void LimgenTests::cli_modelsListWithCustomCatalogue() {
    REQUIRE_BINARY();
    PullRig rig;
    auto levelOf = [](const QJsonObject &json, const QString &id) {
        for (const QJsonValue &value : json["models"].toArray())
            if (value.toObject()["id"].toString() == id)
                return value.toObject()["levels"].toArray()[0].toObject();
        return QJsonObject();
    };
    CliResult result = runCli(rig.fixture, {"models", "--json"}, rig.environment());
    QCOMPARE(result.exitCode, 0);
    QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["models"].toArray().size(), 3); // the test catalogue, not the built-in one
    QCOMPARE(json["models"].toArray()[0].toObject()["id"].toString(), QString("m"));
    QCOMPARE(json["models"].toArray()[0].toObject()["default"].toBool(), true);
    QCOMPARE(levelOf(json, "m")["installed"].toBool(), false);
    QCOMPARE(qint64(levelOf(json, "m")["downloadBytes"].toDouble()), qint64(150000 + 100000 + 70000));
    QCOMPARE(qint64(levelOf(json, "m2")["downloadBytes"].toDouble()), qint64(60000 + 100000 + 30000));
    QCOMPARE(qint64(levelOf(json, "m3")["downloadBytes"].toDouble()), qint64(150000));
    // Install m's files: m is installed, and m2 needs only what it does not share.
    for (const QString &key : QStringList{"a", "b", "c"})
        QVERIFY(makeSparse(rig.target(key), rig.world.file(key).bytes));
    result = runCli(rig.fixture, {"models", "--json"}, rig.environment());
    json = jsonObject(result.out);
    QCOMPARE(levelOf(json, "m")["installed"].toBool(), true);
    QCOMPARE(qint64(levelOf(json, "m2")["downloadBytes"].toDouble()), qint64(90000));
    // The text list names the custom models too, and nothing from the built-in catalogue.
    result = runCli(rig.fixture, {"models"}, rig.environment());
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.out.contains("m2") && result.out.contains("m3"));
    QVERIFY(!result.out.contains("z-image-turbo"));
    QCOMPARE(rig.server.requests.size(), 0); // listing never touches the network
}

void LimgenTests::cli_pullPromptYes() {
    REQUIRE_BINARY();
    if (QStandardPaths::findExecutable("script").isEmpty())
        QSKIP("script(1) is not installed, so there is no pseudo-terminal to drive the prompt with");
    PullRig rig;
    PtyPull pull;
    pull.start(cliEnvironment(rig.fixture, rig.environment()), "m");
    QVERIFY2(pull.waitForPrompt(), pull.output.constData());
    QCOMPARE(rig.server.requests.size(), 0); // nothing is downloaded before the answer
    pull.process->write("y\n");
    WAIT_FOR_PROCESS(pull.process);
    pull.output += pull.process->readAllStandardOutput();
    QCOMPARE(pull.process->exitCode(), 0);
    for (const QString &key : QStringList{"a", "c", "b", "u"})
        QCOMPARE(readFile(rig.target(key)), rig.world.blob(key).data);
    QVERIFY(pull.output.contains(rig.target("a").toUtf8()));
    QVERIFY(!QFileInfo::exists(rig.fixture.models + "/.download.lock"));
}

void LimgenTests::cli_pullPromptDecline() {
    REQUIRE_BINARY();
    if (QStandardPaths::findExecutable("script").isEmpty())
        QSKIP("script(1) is not installed, so there is no pseudo-terminal to drive the prompt with");
    PullRig rig;
    PtyPull pull;
    pull.start(cliEnvironment(rig.fixture, rig.environment()), "m");
    QVERIFY2(pull.waitForPrompt(), pull.output.constData());
    pull.process->write("n\n");
    WAIT_FOR_PROCESS(pull.process);
    QCOMPARE(pull.process->exitCode(), 1);
    QCOMPARE(rig.server.requests.size(), 0);
    QCOMPARE(filesUnder(rig.fixture.models), QStringList());
    QVERIFY(!QFileInfo::exists(rig.fixture.models + "/.download.lock"));
}

void LimgenTests::cli_pullReplansAfterTheLock() {
    REQUIRE_BINARY();
    if (QStandardPaths::findExecutable("script").isEmpty())
        QSKIP("script(1) is not installed, so there is no pseudo-terminal to drive the prompt with");
    PullRig rig;
    PtyPull pull;
    pull.start(cliEnvironment(rig.fixture, rig.environment()), "m");
    QVERIFY2(pull.waitForPrompt(), pull.output.constData());
    // Another limgen installs a file while this one waits at the prompt. Its
    // contents are zeros of the right size: only a download would replace them.
    QVERIFY(makeSparse(rig.target("a"), rig.world.file("a").bytes));
    pull.process->write("y\n");
    WAIT_FOR_PROCESS(pull.process);
    QCOMPARE(pull.process->exitCode(), 0);
    QCOMPARE(rig.server.count("/files/a.bin"), 0);
    QCOMPARE(QFileInfo(rig.target("a")).size(), qint64(150000));
    QVERIFY(readFile(rig.target("a")) != rig.world.blob("a").data);
    for (const QString &key : QStringList{"c", "b", "u"})
        QCOMPARE(readFile(rig.target(key)), rig.world.blob(key).data);
}

void LimgenTests::cli_brokenCatalogueFileIsReported() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(writeFile(fixture.root.path() + "/bad.json", "{ not a catalogue"));
    for (const QString &path : {fixture.root.path() + "/bad.json", fixture.root.path() + "/missing.json"}) {
        QHash<QString, QString> environment = engineEnvironment(fixture);
        environment["LIMGEN_CATALOGUE"] = path;
        // Every command that reads the catalogue stops and says why, naming
        // the override, instead of blaming the built-in catalogue or listing
        // nothing.
        for (const QStringList &arguments : {QStringList{"generate", "a fox"}, QStringList{"models"},
                                             QStringList{"models", "pull", "m", "--yes"}, QStringList{"doctor"}}) {
            const CliResult result = runCli(fixture, arguments, environment);
            QVERIFY(result.started);
            QVERIFY2(result.exitCode == 1, qPrintable(arguments.join(' ') + ": " + result.err));
            QCOMPARE(result.out, QString());
            QVERIFY2(result.err.contains("LIMGEN_CATALOGUE"), qPrintable(result.err));
        }
        QCOMPARE(imageCalls(fixture.root.path() + "/fake.log").size(), 0);
    }
}

QTEST_MAIN(LimgenTests)
#include "limgen_tests.moc"
