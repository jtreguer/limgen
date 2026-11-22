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
#include <QPainter>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
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
#include "colorpass.h"
#include "keeper.h"
#include "resample.h"
#include "batch.h"
#include "config.h"
#include "timings.h"
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
        // A stand-in for `omarchy`: it logs its arguments, `theme bg set <file>`
        // moves the `background` link in the state folder as Omarchy does, and
        // FAKE_OMARCHY_FAIL makes it fail.
        writeScript(omarchyBin(), "echo \"$@\" >> '" + omarchyLog().toUtf8() + "'\n"
                                  "if [ -n \"$FAKE_OMARCHY_FAIL\" ]; then echo boom >&2; exit 1; fi\n"
                                  "if [ \"$1 $2 $3\" = \"theme bg set\" ]; then ln -sfn \"$4\" '" + state.toUtf8() +
                                  "/background'; fi\n");
    }
    QString omarchyBin() const { return root.path() + "/omarchy"; }
    QString omarchyLog() const { return root.path() + "/omarchy.log"; }
    QString pictures() const { return root.path() + "/pictures"; }
    // A hyprctl that reports one small monitor, so kept images are made small.
    QString hyprctl(int width = 320, int height = 180) const {
        const QString path = root.path() + "/hyprctl";
        writeScript(path, "echo '[{\"name\":\"T-1\",\"width\":" + QByteArray::number(width) + ",\"height\":" +
                              QByteArray::number(height) + ",\"transform\":0}]'\n");
        return path;
    }
    // Every call the fake omarchy got, one argument line each.
    QStringList omarchyCalls() const {
        QStringList calls;
        for (const QByteArray &line : readFile(omarchyLog()).split('\n'))
            if (!line.isEmpty())
                calls << QString::fromUtf8(line);
        return calls;
    }
    bool ok() const { return root.isValid(); }

    OmarchyPaths paths() const {
        OmarchyPaths paths;
        paths.state = state;
        paths.backgrounds = backgrounds;
        paths.language = root.path() + "/config/omarchy/language";
        paths.themes = QStringList{themes};
        paths.pictures = pictures();
        paths.omarchy = omarchyBin();
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

const QString repoRoot = QStringLiteral(SOURCE_ROOT);
const QString fakeEngine = repoRoot + "/bin/fake-sd-cli";

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

// Everything a Backend, a Batch, a Config or a Timings reads or writes, pointed
// at temp folders for the length of one test: limgen's config, state, data and
// cache, the models folder, a fake engine, a fake nvidia-smi (12000 MiB, 2000
// used), an empty DRM root and a dead download port. Nothing of the real
// ~/.config/limgen, ~/.local/share/limgen, ~/.local/state/limgen,
// ~/.cache/limgen or Omarchy folders is reachable. The environment is put back
// when the Sandbox goes.
struct Sandbox {
    Fixture fixture;
    EnvGuard guard;
    QString smi() const { return fixture.root.path() + "/nvidia-smi"; }

    explicit Sandbox(int usedMiB = 2000, int totalMiB = 12000)
        : guard({"LIMGEN_SD_CLI", "LIMGEN_MODELS", "LIMGEN_NVIDIA_SMI", "LIMGEN_DRM_ROOT", "LIMGEN_CATALOGUE",
                 "LIMGEN_DOWNLOAD_BASE", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "HOME",
                 "FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS", "LIMGEN_OMARCHY_STATE",
                 "LIMGEN_OMARCHY_BACKGROUNDS", "LIMGEN_HYPRCTL", "HYPRLAND_INSTANCE_SIGNATURE", "LIMGEN_OMARCHY_THEMES",
                 "LIMGEN_PICTURES", "LIMGEN_OMARCHY_BIN", "FAKE_OMARCHY_FAIL"}) {
        const QString base = fixture.root.path();
        qputenv("HOME", (base + "/home").toUtf8());
        qputenv("XDG_CONFIG_HOME", (base + "/config").toUtf8());
        qputenv("XDG_STATE_HOME", (base + "/state").toUtf8());
        qputenv("XDG_DATA_HOME", fixture.data.toUtf8());
        qputenv("XDG_CACHE_HOME", fixture.cache.toUtf8());
        qputenv("LIMGEN_MODELS", fixture.models.toUtf8());
        qputenv("LIMGEN_SD_CLI", fakeEngine.toUtf8());
        qputenv("LIMGEN_NVIDIA_SMI", fakeNvidiaSmi(fixture, totalMiB, usedMiB).toUtf8());
        qputenv("LIMGEN_DRM_ROOT", fixture.drm.toUtf8());
        qputenv("LIMGEN_DOWNLOAD_BASE", "http://127.0.0.1:1"); // nothing listens there
        qputenv("LIMGEN_OMARCHY_STATE", fixture.state.toUtf8());
        qputenv("LIMGEN_OMARCHY_BACKGROUNDS", fixture.backgrounds.toUtf8());
        qputenv("FAKE_SD_CLI_LOG", (base + "/fake.log").toUtf8());
        // Where kept images go, and the tools that put them in place.
        qputenv("LIMGEN_OMARCHY_THEMES", fixture.themes.toUtf8());
        qputenv("LIMGEN_PICTURES", fixture.pictures().toUtf8());
        qputenv("LIMGEN_OMARCHY_BIN", fixture.omarchyBin().toUtf8());
        qputenv("LIMGEN_HYPRCTL", fixture.hyprctl().toUtf8());
    }
    // The Omarchy paths a Backend or a Keeper should use: the fixture's, with
    // a small fake monitor, so a keep makes a small image.
    OmarchyPaths paths() const {
        OmarchyPaths paths = fixture.paths();
        paths.hyprctl = fixture.hyprctl();
        return paths;
    }
    QString configFile() const { return fixture.root.path() + "/config/limgen/config.json"; }
    QString benchFile() const { return fixture.root.path() + "/state/limgen/bench.json"; }
    QString fakeLog() const { return fixture.root.path() + "/fake.log"; }
    QString library() const { return fixture.data + "/limgen/library"; }
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
    // A safety net for the whole run: even a test that forgets its own Sandbox
    // cannot reach the real config, state, data or cache folders of limgen.
    void initTestCase() {
        m_global = std::make_unique<Sandbox>();
        QQuickStyle::setStyle("Material"); // as the app does, before any window
    }
    void cleanupTestCase() { m_global.reset(); }
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
    void engine_repeatedCancelStillKills();
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

    // ---- L4: Batch, Timings, Config ---------------------------------------
    void batch_prepareUnknownModel();
    void batch_prepareMissingFilesNamesThem();
    void batch_previewsAlsoNeedTheTaeFile();
    void batch_prepareChecksInOrder();
    void batch_prepareTooLittleMemory();
    void batch_themeIsReadOnce();
    void batch_fullPromptOverridesTheBuiltOne();
    void batch_seedsCountUp();
    void batch_previewsOnlyWhenAsked();
    void batch_finishedEntriesAreFiled();
    void batch_batchFolderIsRemovedOnSuccess();
    void batch_failureOutcomes_data();
    void batch_failureOutcomes();
    void batch_cancelGivesCancelledAndRemovesTheFolder();
    void batch_destroyingARunningBatchLeavesNothing();
    void batch_noTimingAfterARetry();
    void batch_recordsTimingAndEstimates();
    void timings_estimateFormula();
    void timings_unknownPartsGiveNoEstimate_data();
    void timings_unknownPartsGiveNoEstimate();
    void timings_keyAndDefaultFile();
    void timings_recordBlendsHalfAndHalf();
    void timings_recordKeepsOtherKeysAndUnknownParts();
    void timings_missingOrGarbledFile();
    void timings_saveIsAtomicAndReportsFailure();
    void config_roundTripAndUnknownKeysAreKept();
    void config_missingOrGarbledFileGivesDefaults();
    void config_saveFailureAndNoPartialFile();

    // ---- L4: Backend, in process -------------------------------------------
    void backend_engineProbeGoesFromCheckingToReady();
    void backend_engineProblemsAreShownAndRecheckRecovers();
    void backend_modelsListAndInstalledState();
    void backend_modelsRunsHereFollowsFreeMemory();
    void backend_selectModelPersistsAndRefreshes();
    void backend_unknownModelOrStyleInConfigFallsBack();
    void backend_downloadInstallsTheModel();
    void backend_downloadPausesAndResumes();
    void backend_downloadIsRefusedWhileLocked();
    void backend_promptFollowsSubjectStyleAndTheme();
    void backend_editedPromptSticksUntilReset();
    void backend_canGenerateRules();
    void backend_generateFillsFourDrafts();
    void backend_rerunReusesThePromptWithNewSeeds();
    void backend_rerunRecordsTheOriginalSubjectAndStyle();
    void backend_cancelClearsDraftsAndSaysSo();
    void backend_estimateTexts();
    void backend_durationText();
    void backend_stopAllStopsABatch();
    void backend_stopAllPausesADownload();

    // ---- L4: the window (QML, offscreen) ----------------------------------
    void backend_destroyingWhileABatchRunsIsSafe();

    void qml_loadsWithoutWarnings_data();
    void qml_loadsWithoutWarnings();
    void qml_setupPanelOrGrid_data();
    void qml_setupPanelOrGrid();
    void qml_typingKeysDoNotActAsShortcuts();
    void qml_escLeavesTheFieldThenShortcutsAct();
    void qml_enterInTheSubjectGeneratesAndLeavesTheField();
    void qml_escCancelsARunningBatch();
    void qml_arrowsMoveTheSelectionInTheGrid();
    void qml_fullViewOnlyForADoneDraft();
    void qml_helpToggles();
    void qml_selectionStaysWhileTypingInABatch();
    void qml_setupPanelLetsKeysWorkAfterTheField();
    void qml_modelPickerOpensAndCloses();
    void qml_quitWithARunningBatchAsksFirst();

    // ---- L5: upscale parser and engine -------------------------------------
    void progress_upscaleLogCountsTiles();
    void progress_upscaleLogChunkSizeDoesNotMatter();
    void progress_anyTotalOnlyWhenStepsIsZero();
    void engine_argumentsUpscale();
    void engine_upscaleRunMakesFourTimes();
    void engine_upscaleOutOfMemoryRetriesWithSmallerTiles();
    void engine_upscaleOutOfMemoryFailsAfterOneRetry();
    void engine_upscaleSmallTilesAreNotRetried();
    void engine_upscaleSizeCheckAndFailures_data();
    void engine_upscaleSizeCheckAndFailures();
    void engine_upscaleCancelRemovesTheOutput();

    // ---- L5: ColorPass and Resample ---------------------------------------
    void colorpass_toLabKnownValues_data();
    void colorpass_toLabKnownValues();
    void colorpass_roundTrip();
    void colorpass_fromLabClipsOutOfGamut();
    void colorpass_statisticsOfImages();
    void colorpass_statisticsOfPalette();
    void colorpass_strengthZeroIsIdentity();
    void colorpass_strengthOneReachesTheTargetMeans();
    void colorpass_spreadFollowsTheTargetWithinTheClamp();
    void colorpass_paletteTargetMovesOnlyTheCast();
    void colorpass_halfStrengthIsHalfway();
    void colorpass_alphaIsKept();
    void resample_lanczosGivesExactSizes();
    void resample_flatImageStaysFlat();
    void resample_lanczosKeepsAnEdge();
    void resample_coverCropsTheCentre();
    void resample_coverUpAndDown();
    void resample_nullAndEmpty();

    // ---- L5: Library finals and discards, Config, Omarchy -----------------
    void library_addFinalPngAndJpeg();
    void library_addFinalFailsCleanly();
    void library_discardMovesTheDraftAside();
    void library_restoreBringsItBackExactly();
    void library_aFinalCannotBeDiscarded();
    void library_purgeAndPurgeDiscarded();
    void config_finishKeysDefaultsAndBounds();
    void omarchy_themeDirectoryLookupOrder();
    void omarchy_themeBackgroundsOfATheme();
    void omarchy_keepDirectory();
    void omarchy_currentBackground();
    void omarchy_setBackgroundThroughTheFakeOmarchy();
    void omarchy_pathsThemesAndPictures();

    // ---- L5: Keeper --------------------------------------------------------
    void keeper_fileNames_data();
    void keeper_fileNames();
    void keeper_parametersText();
    void keeper_prepareOutcomesInOrder();
    void keeper_prepareFillsTheResult();
    void keeper_alreadyKeptIsFoundInEitherFormat();
    void keeper_fullKeepWritesEverythingTheDocsSay();
    void keeper_colorTargets_data();
    void keeper_colorTargets();
    void keeper_colorPassMovesTheColourTowardTheBackgrounds();
    void keeper_jpegQualityAndComment();
    void keeper_alreadyKeptIsReportedAsynchronously();
    void keeper_cancelDuringTheUpscale();
    void keeper_destroyingARunningKeeperIsSilent();
    void keeper_failuresRemoveTheWorkFolder_data();
    void keeper_failuresRemoveTheWorkFolder();
    void keeper_aFileThatAppearedMeanwhileIsNeverOverwritten();
    void keeper_noThemeGoesToThePicturesFolder();
    void keeper_withoutAMonitorTheFinalIs2560x1440();
    void keeper_keepFolderHoldsOnlyLimgenFiles();

    // ---- L5: CLI keep and set ---------------------------------------------
    void cli_keepGoesToTheThemeTheDraftWasMadeFor();
    void cli_keepThemeOptionOverridesAndChecksTheTheme();
    void cli_keepDraftWithoutAThemeUsesTheCurrentThenPictures();
    void cli_keepJson();
    void cli_keepSetShowsItAndFailsWhenOmarchyFails();
    void cli_keepAlreadyKeptMakesNothingNew();
    void cli_keepArgumentErrors_data();
    void cli_keepArgumentErrors();
    void cli_keepNotRunnableOrFailing_data();
    void cli_keepNotRunnableOrFailing();
    void cli_keepTakesColourPassAndFormatFromTheConfigAndTheOptions();
    void cli_keepInterruptedExits130AndKeepsNothing();
    void cli_setAFile();
    void cli_setADraftKeepsItIntoTheCurrentTheme();
    void cli_setAFinalGoesThroughItsDraft();
    void cli_setAFinalWhoseDraftIsGone();
    void cli_setErrors();
    void cli_helpListsKeepAndSet();

    // ---- L5: Backend, keeping ----------------------------------------------
    void backend_keepShowsTileStatesAndInstallsTheImage();
    void backend_keepAndSetSetsTheBackgroundOnce();
    void backend_keepOnlyAppliesToDoneDrafts();
    void backend_themeChoiceWhenTheThemeChangedThenKeepInto();
    void backend_discardUndoAndRedo();
    void backend_undoAndRedoOfAKeep();
    void backend_undoWhileKeepingCancelsIt();
    void backend_undoThenRedoAtOnceKeepsTheTileKeeping();
    void backend_undoOfTheOnlyBackgroundIsRefused();
    void backend_generateWhileKeepingWaitsThenStarts();
    void backend_cancelOfAWaitingBatchAndStopAll();
    void backend_aNewBatchClearsUndoAndPurgesDiscards();
    void backend_destroyingPurgesDiscardsAndStopsKeeps();
    void backend_purgeDiscardedAtStartLeavesRecentOnes();
    void backend_finishSettingsAreSaved();
    void backend_keepMessageSurvivesTheThemeReloadItCauses();
    void backend_keepFailuresAreShown();
    void backend_keepWithoutAThemeGoesToPictures();

    // ---- L5: the window, keeping -------------------------------------------
    void qml_keysKeepSetDiscardUndoRedo();
    void qml_actionKeysDoNothingWhileTypingOrUnderHelpOrADialog();
    void qml_themeDialogKeepsIntoTheChosenTheme();
    void qml_quitWhileKeepingAsksFirst();
    void qml_finishSettingsUnderTheFullPrompt();

    // ---- L6: image to image, the library, variations ----------------------
    void engine_samplingStepsFollowTheEnginesFloatMaths_data();
    void engine_samplingStepsFollowTheEnginesFloatMaths();
    void engine_argumentsForAVariation();
    void progress_targetLineSetsTheStepsPerImage();
    void progress_vaeTileLinesAreNotStepsInAVariation();
    void engine_imageToImageRunWithTheFake();
    void engine_imageToImagePreviewsFollowTheShortRun();
    void engine_imageToImageNeedsAReadableSource();
    void library_listIsNewestFirstAcrossMonths();
    void library_listSkipsWhatIsNotAnEntry();
    void library_sidecarRecordsTheSource();
    void batch_variationFromAFileRecordsItsSource();
    void batch_retriedVariationCountsOnlySamplingSteps();
    void batch_variationFromALibraryIdAndWhenItsPromptIsReused();
    void batch_variationStrengthIsClampedAndRaised();
    void batch_variationSourceProblemsComeInOrder();
    void batch_variationSourceIsCoveredToTheDraftSize();
    void batch_variationRecordsNoTimingAndEstimatesItsSteps();
    void batch_variationSourceThatCannotBePrepared();
    void cli_generateFromAFileAndALibraryImage();
    void cli_generateFromArgumentErrors_data();
    void cli_generateFromArgumentErrors();
    void cli_libraryListsNewestFirst();
    void cli_libraryJsonAndTheme();
    void cli_libraryEmptyAndErrors();

    // ---- L6: Backend, variations and the library ---------------------------
    void backend_backgroundsAreTheThemesOwnThenTheUsers();
    void backend_backgroundsRefreshAfterAKeepAndAThemeSwitch();
    void backend_useBackgroundTogglesTheSource();
    void backend_cycleBackgroundBothWaysAndWraps();
    void backend_aBackgroundSourceGoesWithItsFile();
    void backend_libraryFollowsDiscardUndoAndRedo();
    void backend_varyDoesNothingWhenNoBatchCanStart();
    void backend_fullPromptShowsTheReusedPrompt();
    void batch_strengthForRaisesAndClamps();
    void backend_estimateUsesTheRaisedStrength();
    void backend_varyAndVaryLibrary();
    void backend_rerunAndTheWaitingBatchKeepTheSource();
    void backend_canGenerateWithAnEmptySubjectOnlyWithASource();
    void backend_loadLibraryFiltersByThemeAndFlagsKept();
    void backend_strengthIsSavedToConfig();

    // ---- L6: the window, variations and the library ------------------------
    void qml_keysBAndShiftBCycleTheBackgrounds();
    void qml_clickInTheLeftColumnChoosesAndClears();
    void qml_vVariesTheSelectedDraftAndTheSourceRowShows();
    void qml_strengthSliderSavesWhenLetGo();
    void qml_libraryOpensAndClosesWithL();
    void qml_arrowsMoveByRowInTheLibrary();
    void qml_theLibraryTakesTheClicks();
    void qml_closingTheLibraryClosesItsFullView();
    void qml_librarySelectionFollowsTheList();
    void qml_aKeptLibraryDraftNamesNoEmptyFile();
    void qml_fShowsTheLibraryImage();
    void qml_vInTheLibraryVariesTheSelectedImage();
    void qml_keepSetDiscardDoNothingWhileTheLibraryIsOpen();
    void qml_escChainOrder();

private:
    std::unique_ptr<Sandbox> m_global;
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
    Sandbox sandbox;
    Fixture &fixture = sandbox.fixture;
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
    Sandbox sandbox;
    Fixture &fixture = sandbox.fixture;
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
    Sandbox sandbox;
    Fixture &fixture = sandbox.fixture;
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
    Sandbox sandbox;
    Fixture &fixture = sandbox.fixture;
    QVERIFY(writeFile(fixture.state + "/theme.name", "bare\n"));
    Omarchy omarchy(fixture.paths());
    Backend backend(&omarchy);
    QCOMPARE(backend.themeName(), QString("bare"));
    QVERIFY2(backend.status().contains("colors.toml"), qPrintable(backend.status()));
    QVERIFY(backend.swatches().isEmpty());
}

void LimgenTests::backend_paletteChangedAfterThemeSwitch() {
    Sandbox sandbox;
    Fixture &fixture = sandbox.fixture;
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
                             "LIMGEN_CATALOGUE", "LIMGEN_DOWNLOAD_BASE", "LIMGEN_OMARCHY_THEMES", "LIMGEN_PICTURES",
                             "FAKE_OMARCHY_FAIL"})
        environment.remove(name);
    // Kept images and the background go to the fixture's folders and fake
    // omarchy, never to the real Omarchy or ~/Pictures.
    environment.insert("LIMGEN_OMARCHY_THEMES", fixture.themes);
    environment.insert("LIMGEN_PICTURES", fixture.pictures());
    environment.insert("LIMGEN_OMARCHY_BIN", fixture.omarchyBin());
    environment.insert("LIMGEN_HYPRCTL", fixture.hyprctl());
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

void LimgenTests::engine_repeatedCancelStillKills() {
    EnvGuard guard({"FAKE_SD_CLI_MODE", "FAKE_SD_CLI_LOG", "FAKE_SD_CLI_STEP_SECONDS"});
    Fixture fixture;
    QVERIFY(fixture.ok());
    // An engine stuck where it cannot act on SIGTERM; a command that cancels
    // every 100 ms until it is gone must still end it with SIGKILL.
    qputenv("FAKE_SD_CLI_MODE", "hang-ignore-term");
    EngineRequest request = runnableRequest(fixture.root.path(), 1, 4);
    request.previews = true;
    request.previewInterval = 1;
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    watch(engine, run);
    engine.start(request);
    // The fake writes the first preview once SIGTERM is ignored, then hangs.
    QTRY_VERIFY_WITH_TIMEOUT(!QDir(request.outputDirectory + "/preview").entryList({"*.png"}).isEmpty(), 10000);
    QTimer again;
    QObject::connect(&again, &QTimer::timeout, &engine, [&] { engine.cancel(); });
    again.start(100);
    QTRY_VERIFY_WITH_TIMEOUT(run.done(), 8000);
    again.stop();
    QCOMPARE(int(run.error.kind), int(EngineError::Cancelled));
    QVERIFY(!engine.isRunning());
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

World makeWorld(int steps = 1) {
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
    auto model = [steps](const char *id, bool isDefault, const QJsonObject &levelFiles) {
        return QJsonObject{{"id", id},
                           {"name", id},
                           {"publisher", "Test"},
                           {"default", isDefault},
                           {"licence", QJsonObject{{"id", "MIT"}, {"summary", "s"}, {"url", "https://x.test/l"}}},
                           {"native", QJsonArray{64, 64}},
                           {"steps", steps},
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
    // The store's own rewriting (LIMGEN_DOWNLOAD_BASE) is off here: these tests
    // send the catalogue's URLs to the local server through the test network.
    EnvGuard noDownloadBase{{"LIMGEN_DOWNLOAD_BASE"}};
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

// ============================== L4: Batch =================================

namespace {

// What a Batch reported.
struct BatchRun {
    bool finished = false, failed = false;
    Batch::Outcome outcome = Batch::Outcome::Ok;
    QString message;
    QList<LibraryEntry> entries;
    int progress = 0;
    QList<int> sampled;
    QList<QPair<int, QString>> previews;
    bool over() const { return finished || failed; }
};

void watchBatch(Batch &batch, BatchRun &run) {
    QObject::connect(&batch, &Batch::progress, &batch, [&run](int, int, int, double) { run.progress++; });
    QObject::connect(&batch, &Batch::sampled, &batch, [&run](int image) { run.sampled << image; });
    QObject::connect(&batch, &Batch::preview, &batch,
                     [&run](int image, const QString &path) { run.previews.append({image, path}); });
    QObject::connect(&batch, &Batch::finished, &batch, [&run](const QList<LibraryEntry> &entries) {
        run.finished = true;
        run.entries = entries;
    });
    QObject::connect(&batch, &Batch::failed, &batch, [&run](Batch::Outcome outcome, const QString &message) {
        run.failed = true;
        run.outcome = outcome;
        run.message = message;
    });
}

// A sandbox, the built-in catalogue with z-image-turbo installed as sparse
// files, a probe of the fake engine and a Batch on it.
struct BatchRig {
    Sandbox sb;
    Catalogue catalogue = Catalogue::builtIn();
    EngineProbe probe;
    BatchRun run;
    std::unique_ptr<Batch> batch;

    explicit BatchRig(bool install = true, int usedMiB = 2000) : sb(usedMiB) {
        if (install)
            installModel(catalogue, "z-image-turbo", sb.fixture.models);
        probe = SdCppEngine(fakeEngine).probe();
        remake();
    }
    void remake() {
        run = BatchRun();
        batch = std::make_unique<Batch>(catalogue, sb.fixture.paths(), fakeEngine);
        watchBatch(*batch, run);
    }
    static BatchSettings settings() {
        BatchSettings settings;
        settings.subject = "oil rig at dusk";
        return settings;
    }
    Batch::Outcome prepare(const BatchSettings &settings, QString *message = nullptr) {
        return batch->prepare(settings, probe, message);
    }
    QString taePath() const {
        return Catalogue::path(*catalogue.file("taef1"), sb.fixture.models);
    }
};

#define WAIT_FOR_BATCH(rig) QTRY_VERIFY_WITH_TIMEOUT((rig).run.over(), 30000)

} // namespace

void LimgenTests::batch_prepareUnknownModel() {
    BatchRig rig(false); // not even installed: the model is checked first
    BatchSettings settings = BatchRig::settings();
    settings.modelId = "nope";
    QString message;
    QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::UnknownModel));
    QVERIFY2(message.contains("nope") && message.contains("z-image-turbo"), qPrintable(message));
    QVERIFY(!rig.batch->isRunning());
    // A null message pointer is accepted.
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::UnknownModel));
}

void LimgenTests::batch_prepareMissingFilesNamesThem() {
    BatchRig rig(false);
    QString message;
    QCOMPARE(int(rig.prepare(BatchRig::settings(), &message)), int(Batch::Outcome::NotRunnable));
    const CatalogueModel &model = *rig.catalogue.model("z-image-turbo");
    for (const QString &role : QStringList{"diffusion-model", "llm", "vae"})
        QVERIFY2(message.contains(describeFile(*rig.catalogue.file(model.levels.first().files.value(role)))),
                 qPrintable(role + ": " + message));
    QVERIFY(!message.contains("taef1")); // not needed without previews
    QVERIFY(message.contains(rig.sb.fixture.models));
    QVERIFY(message.contains("limgen models pull z-image-turbo"));
    // Each missing file is named once it is the only one missing, too.
    QVERIFY(installModel(rig.catalogue, "z-image-turbo", rig.sb.fixture.models));
    const CatalogueFile &vae = *rig.catalogue.file("flux1-vae");
    QVERIFY(QFile::remove(Catalogue::path(vae, rig.sb.fixture.models)));
    QCOMPARE(int(rig.prepare(BatchRig::settings(), &message)), int(Batch::Outcome::NotRunnable));
    QVERIFY2(message.contains(describeFile(vae)) && !message.contains("z_image_turbo-Q4_K"), qPrintable(message));
    // A truncated file counts as missing.
    QVERIFY(makeSparse(Catalogue::path(vae, rig.sb.fixture.models), vae.bytes - 1));
    QCOMPARE(int(rig.prepare(BatchRig::settings(), &message)), int(Batch::Outcome::NotRunnable));
}

void LimgenTests::batch_previewsAlsoNeedTheTaeFile() {
    BatchRig rig(true);
    QVERIFY(QFile::remove(rig.taePath()));
    BatchSettings settings = BatchRig::settings();
    settings.previews = false;
    QString message;
    QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::Ok));
    QVERIFY(!rig.batch->request().files.contains("tae"));
    settings.previews = true;
    QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::NotRunnable));
    QVERIFY2(message.contains("taef1.safetensors"), qPrintable(message));
    // With the file back, previews are allowed and the engine is given it.
    QVERIFY(installModel(rig.catalogue, "z-image-turbo", rig.sb.fixture.models));
    QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::Ok));
    QCOMPARE(rig.batch->request().files.value("tae"), rig.taePath());
    QVERIFY(rig.batch->request().previews);
}

void LimgenTests::batch_prepareChecksInOrder() {
    EngineProbe bad;
    bad.error = {EngineError::NoGpu, "the engine says no", {}};
    {
        // Files before the engine: a missing model is reported even if the engine is broken too.
        BatchRig rig(false);
        rig.probe = bad;
        QString message;
        QCOMPARE(int(rig.prepare(BatchRig::settings(), &message)), int(Batch::Outcome::NotRunnable));
        QVERIFY2(message.contains("not installed"), qPrintable(message));
        QVERIFY(!message.contains("the engine says no"));
    }
    {
        // The engine before the memory check.
        BatchRig rig(true, 9000);
        rig.probe = bad;
        QString message;
        QCOMPARE(int(rig.prepare(BatchRig::settings(), &message)), int(Batch::Outcome::NotRunnable));
        QCOMPARE(message, QString("the engine says no"));
    }
    {
        // Unknown model before everything.
        BatchRig rig(false);
        rig.probe = bad;
        BatchSettings settings = BatchRig::settings();
        settings.modelId = "nope";
        QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::UnknownModel));
    }
    {
        // A real probe error: the fake engine with no GPU.
        BatchRig rig(true);
        qputenv("FAKE_SD_CLI_MODE", "no-gpu");
        rig.probe = SdCppEngine(fakeEngine).probe();
        QString message;
        QCOMPARE(int(rig.prepare(BatchRig::settings(), &message)), int(Batch::Outcome::NotRunnable));
        QCOMPARE(message, rig.probe.error.message);
        QVERIFY(!message.isEmpty());
    }
}

void LimgenTests::batch_prepareTooLittleMemory() {
    const int need = Catalogue::builtIn().model("z-image-turbo")->levels.first().minFreeMiB;
    {
        BatchRig rig(true, 12000 - need + 1); // one MiB short
        QString message;
        QCOMPARE(int(rig.prepare(BatchRig::settings(), &message)), int(Batch::Outcome::NotRunnable));
        QVERIFY2(message.contains(QString::number(need - 1)) && message.contains("12000") &&
                     message.contains(QString::number(need)),
                 qPrintable(message));
        QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 0); // nothing was started
    }
    {
        BatchRig rig(true, 12000 - need); // exactly enough
        QCOMPARE(int(rig.prepare(BatchRig::settings())), int(Batch::Outcome::Ok));
    }
    {
        // No reading at all (as on Intel) is not a refusal.
        BatchRig rig(true);
        qputenv("LIMGEN_NVIDIA_SMI", (rig.sb.fixture.root.path() + "/no-smi").toUtf8());
        QCOMPARE(int(rig.prepare(BatchRig::settings())), int(Batch::Outcome::Ok));
    }
}

void LimgenTests::batch_themeIsReadOnce() {
    BatchRig rig;
    writeStockTheme(rig.sb.fixture);
    QCOMPARE(int(rig.prepare(BatchRig::settings())), int(Batch::Outcome::Ok));
    const QString promptBefore = rig.batch->request().prompt;
    QVERIFY(promptBefore.contains("near-black indigo"));
    QCOMPARE(rig.batch->themeSlug(), QString("night-city"));
    // The theme is switched after prepare(): the batch keeps the old one.
    QVERIFY(rig.sb.fixture.addTheme("other", tomlWith("#fafafa", "#111111", "#ff0000")));
    QVERIFY(rig.sb.fixture.switchTo("other"));
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.batch->themeSlug(), QString("night-city"));
    QCOMPARE(valueAfter(imageCalls(rig.sb.fakeLog()).first(), "--prompt"), promptBefore);
    for (const LibraryEntry &entry : rig.run.entries) {
        const QJsonObject theme = entry.metadata["theme"].toObject();
        QCOMPARE(theme["slug"].toString(), QString("night-city"));
        QCOMPARE(theme["mode"].toString(), QString("dark"));
        QCOMPARE(theme["palette"].toObject()["accent"].toString(), QString("#7aa2f7"));
        QCOMPARE(entry.metadata["prompt"].toString(), promptBefore);
    }
}

void LimgenTests::batch_fullPromptOverridesTheBuiltOne() {
    BatchRig rig;
    writeStockTheme(rig.sb.fixture);
    BatchSettings settings = BatchRig::settings();
    settings.fullPrompt = "  only this prompt  ";
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    QCOMPARE(rig.batch->request().prompt, QString("only this prompt"));
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(valueAfter(imageCalls(rig.sb.fakeLog()).first(), "--prompt"), QString("only this prompt"));
    QCOMPARE(rig.run.entries.first().metadata["prompt"].toString(), QString("only this prompt"));
    QCOMPARE(rig.run.entries.first().metadata["subject"].toString(), QString("oil rig at dusk"));
    // Blank text is no override.
    settings.fullPrompt = "   ";
    rig.remake();
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    QVERIFY(rig.batch->request().prompt.startsWith("oil rig at dusk, cinematic wide shot, "));
}

void LimgenTests::batch_seedsCountUp() {
    BatchRig rig;
    BatchSettings settings = BatchRig::settings();
    settings.seed = 100;
    settings.count = 3;
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    QCOMPARE(rig.batch->request().seed, quint32(100));
    QCOMPARE(rig.batch->request().count, 3);
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.run.entries.size(), 3);
    for (int i = 0; i < 3; ++i)
        QCOMPARE(rig.run.entries[i].metadata["seed"].toDouble(), double(100 + i));
    QCOMPARE(valueAfter(imageCalls(rig.sb.fakeLog()).first(), "--seed"), QString("100"));
    // Without a seed, a random one below 2^31.
    rig.remake();
    settings.seed.reset();
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    QVERIFY(rig.batch->request().seed < (quint32(1) << 31));
}

void LimgenTests::batch_previewsOnlyWhenAsked() {
    {
        BatchRig rig;
        BatchSettings settings = BatchRig::settings();
        settings.previews = true;
        QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
        rig.batch->start();
        WAIT_FOR_BATCH(rig);
        QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
        // 8 steps, a preview every 2: 4 per draft.
        QCOMPARE(rig.run.previews.size(), 16);
        QMap<int, int> perImage;
        for (const auto &preview : rig.run.previews)
            perImage[preview.first]++;
        QCOMPARE(perImage, (QMap<int, int>{{0, 4}, {1, 4}, {2, 4}, {3, 4}}));
        const QStringList arguments = imageCalls(rig.sb.fakeLog()).first();
        QVERIFY(arguments.contains("--taesd-preview-only"));
        QCOMPARE(valueAfter(arguments, "--tae"), rig.taePath());
    }
    {
        BatchRig rig;
        QCOMPARE(int(rig.prepare(BatchRig::settings())), int(Batch::Outcome::Ok)); // previews off by default
        rig.batch->start();
        WAIT_FOR_BATCH(rig);
        QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
        QVERIFY(rig.run.previews.isEmpty());
        const QStringList arguments = imageCalls(rig.sb.fakeLog()).first();
        QVERIFY(!arguments.contains("--preview"));
        QVERIFY(!arguments.contains("--tae"));
    }
}

void LimgenTests::batch_finishedEntriesAreFiled() {
    BatchRig rig;
    writeStockTheme(rig.sb.fixture);
    BatchSettings settings = BatchRig::settings();
    settings.seed = 7;
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    QCOMPARE(rig.batch->model()->id, QString("z-image-turbo"));
    const QString folder = rig.batch->request().outputDirectory;
    rig.batch->start();
    QVERIFY(rig.batch->isRunning());
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QVERIFY(!rig.batch->isRunning());
    QCOMPARE(rig.run.entries.size(), 4);
    QCOMPARE(rig.run.progress, 32);
    QCOMPARE(rig.run.sampled, (QList<int>{0, 1, 2, 3}));
    const Library library(rig.sb.library());
    for (int i = 0; i < 4; ++i) {
        const LibraryEntry &entry = rig.run.entries[i];
        const LibraryEntry found = library.find(entry.id);
        QVERIFY2(found.isValid(), qPrintable(entry.id));
        QCOMPARE(found.image, entry.image);
        QCOMPARE(QImageReader(entry.image).size(), QSize(1344, 768));
        QCOMPARE(found.metadata["seed"].toDouble(), double(7 + i));
        QCOMPARE(found.metadata["model"].toObject()["id"].toString(), QString("z-image-turbo"));
        QCOMPARE(found.metadata["engine"].toObject()["version"].toString(), QString(SdCppEngine::pinnedVersion));
        QCOMPARE(found.metadata["engine"].toObject()["backend"].toString(), QString("vulkan"));
        QCOMPARE(found.metadata["theme"].toObject()["slug"].toString(), QString("night-city"));
        QCOMPARE(found.metadata["style"].toString(), QString("cinematic"));
        QCOMPARE(found.metadata["steps"].toInt(), 8);
    }
    QCOMPARE(filesUnder(rig.sb.library()).size(), 8);
    // The batch folder is gone, and the cache holds nothing.
    QVERIFY(!QFileInfo::exists(folder));
    QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
    QVERIFY(!QFileInfo::exists(folder));
}

void LimgenTests::batch_batchFolderIsRemovedOnSuccess() {
    BatchRig rig;
    BatchSettings settings = BatchRig::settings();
    settings.previews = true; // previews are in the folder too
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    const QString folder = rig.batch->request().outputDirectory;
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QVERIFY(!QFileInfo::exists(folder));
    QVERIFY(!QFileInfo::exists(folder + "/preview"));
    QCOMPARE(QDir(rig.sb.fixture.cache + "/limgen").entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList());
}

void LimgenTests::batch_failureOutcomes_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("outcome");
    QTest::newRow("out of memory is not runnable here") << "oom" << int(Batch::Outcome::NotRunnable);
    QTest::newRow("engine fails") << "fail" << int(Batch::Outcome::Failed);
    QTest::newRow("error with exit 0") << "error-exit0" << int(Batch::Outcome::Failed);
    QTest::newRow("wrong size") << "wrong-size" << int(Batch::Outcome::Failed);
}

void LimgenTests::batch_failureOutcomes() {
    QFETCH(QString, mode);
    QFETCH(int, outcome);
    BatchRig rig;
    qputenv("FAKE_SD_CLI_MODE", mode.toUtf8());
    BatchSettings settings = BatchRig::settings();
    settings.count = 2;
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    const QString folder = rig.batch->request().outputDirectory;
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY(rig.run.failed);
    QVERIFY(!rig.run.finished);
    QCOMPARE(int(rig.run.outcome), outcome);
    QVERIFY(!rig.run.message.isEmpty());
    if (mode == "oom")
        QVERIFY2(rig.run.message.toLower().contains("memory"), qPrintable(rig.run.message));
    QVERIFY(!rig.batch->isRunning());
    QVERIFY(!QFileInfo::exists(folder));
    QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
    QCOMPARE(filesUnder(rig.sb.library()), QStringList()); // nothing was filed
}

void LimgenTests::batch_cancelGivesCancelledAndRemovesTheFolder() {
    BatchRig rig;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    BatchSettings settings = BatchRig::settings();
    settings.previews = true;
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    const QString folder = rig.batch->request().outputDirectory;
    rig.batch->start();
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(rig.sb.fakeLog()).isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(folder + "/preview"), 5000);
    rig.batch->cancel();
    WAIT_FOR_BATCH(rig);
    QVERIFY(rig.run.failed);
    QCOMPARE(int(rig.run.outcome), int(Batch::Outcome::Cancelled));
    QVERIFY(!rig.batch->isRunning());
    QVERIFY(!QFileInfo::exists(folder));
    QCOMPARE(filesUnder(rig.sb.library()), QStringList());
    // Cancelling a batch that is not running does nothing.
    rig.run = BatchRun();
    rig.batch->cancel();
    QTest::qWait(100);
    QVERIFY(!rig.run.over());
}

void LimgenTests::batch_destroyingARunningBatchLeavesNothing() {
    BatchRig rig;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    BatchSettings settings = BatchRig::settings();
    settings.previews = true;
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    const QString folder = rig.batch->request().outputDirectory;
    rig.batch->start();
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(rig.sb.fakeLog()).isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(folder + "/preview"), 5000);
    // The engine process is there: its command line holds the batch folder.
    QCOMPARE(QProcess::execute("pgrep", {"-f", folder}), 0);
    // A batch being destroyed reports nothing: the end of its engine must not
    // reach it, since its members are already gone by then.
    int reports = 0;
    QObject::connect(rig.batch.get(), &Batch::failed, [&reports] { ++reports; });
    QObject::connect(rig.batch.get(), &Batch::finished, [&reports] { ++reports; });
    rig.batch.reset();
    QCOMPARE(reports, 0);
    QVERIFY(!QFileInfo::exists(folder));
    QCOMPARE(QProcess::execute("pgrep", {"-f", folder}), 1); // no process left
    QCOMPARE(filesUnder(rig.sb.library()), QStringList());
}

void LimgenTests::batch_noTimingAfterARetry() {
    BatchRig rig;
    qputenv("FAKE_SD_CLI_MODE", "oom-once");
    QCOMPARE(int(rig.prepare(BatchRig::settings())), int(Batch::Outcome::Ok));
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    // The clock ran over both attempts, so the batch says nothing about this
    // machine's speed and is not recorded.
    QVERIFY(!QFileInfo::exists(rig.sb.benchFile()));
}

void LimgenTests::batch_recordsTimingAndEstimates() {
    BatchRig rig;
    QCOMPARE(rig.batch->estimate(), -1.0); // nothing prepared yet
    QCOMPARE(int(rig.prepare(BatchRig::settings())), int(Batch::Outcome::Ok));
    QCOMPARE(rig.batch->estimate(), -1.0); // nothing measured yet
    QVERIFY(!QFileInfo::exists(rig.sb.benchFile()));
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    // bench.json is under XDG_STATE_HOME, keyed by model, level and backend.
    QVERIFY(QFileInfo::exists(rig.sb.benchFile()));
    const QJsonObject json = QJsonDocument::fromJson(readFile(rig.sb.benchFile())).object();
    QVERIFY2(json.contains("z-image-turbo/q4_k/vulkan"), qPrintable(QString(readFile(rig.sb.benchFile()))));
    const Timing timing = Timings(rig.sb.benchFile()).get("z-image-turbo/q4_k/vulkan");
    QVERIFY(timing.isKnown());
    // The fake engine prints 0.01 s per step.
    QVERIFY2(timing.secondsPerStep > 0.005 && timing.secondsPerStep < 0.5, qPrintable(QString::number(timing.secondsPerStep)));
    QVERIFY2(timing.overheadSeconds >= 0 && timing.overheadSeconds < 10, qPrintable(QString::number(timing.overheadSeconds)));
    QVERIFY2(timing.decodeSecondsPerImage >= 0 && timing.decodeSecondsPerImage < 10,
             qPrintable(QString::number(timing.decodeSecondsPerImage)));
    // The estimate of the next batch of the same size follows from it.
    rig.remake();
    QCOMPARE(int(rig.prepare(BatchRig::settings())), int(Batch::Outcome::Ok));
    QVERIFY(rig.batch->estimate() > 0);
    QCOMPARE(rig.batch->estimate(), timing.estimate(4, 8));
    // A batch of another size scales with the count.
    BatchSettings two = BatchRig::settings();
    two.count = 2;
    rig.remake();
    QCOMPARE(int(rig.prepare(two)), int(Batch::Outcome::Ok));
    QCOMPARE(rig.batch->estimate(), timing.estimate(2, 8));
    // And nothing was written outside the sandbox: the only state file is this one.
    QCOMPARE(filesUnder(rig.sb.fixture.root.path() + "/state"), QStringList{rig.sb.benchFile()});
}

// ============================== L4: Timings and Config ====================

void LimgenTests::timings_estimateFormula() {
    const Timing timing{2.0, 0.5, 0.25};
    QVERIFY(timing.isKnown());
    QCOMPARE(timing.estimate(4, 8), 2.0 + 4 * (8 * 0.5 + 0.25)); // 19
    QCOMPARE(timing.estimate(1, 1), 2.75);
    QCOMPARE(timing.estimate(8, 4), 2.0 + 8 * (4 * 0.5 + 0.25));
    // Zero overhead and zero decode time are measurements, not "unknown".
    const Timing free{0.0, 1.0, 0.0};
    QVERIFY(free.isKnown());
    QCOMPARE(free.estimate(2, 3), 6.0);
}

void LimgenTests::timings_unknownPartsGiveNoEstimate_data() {
    QTest::addColumn<double>("overhead");
    QTest::addColumn<double>("perStep");
    QTest::addColumn<double>("decode");
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("steps");
    QTest::newRow("overhead unknown") << -1.0 << 0.5 << 0.25 << 4 << 8;
    QTest::newRow("step time unknown") << 2.0 << -1.0 << 0.25 << 4 << 8;
    QTest::newRow("step time zero") << 2.0 << 0.0 << 0.25 << 4 << 8;
    QTest::newRow("decode unknown") << 2.0 << 0.5 << -1.0 << 4 << 8;
    QTest::newRow("no drafts") << 2.0 << 0.5 << 0.25 << 0 << 8;
    QTest::newRow("no steps") << 2.0 << 0.5 << 0.25 << 4 << 0;
}

void LimgenTests::timings_unknownPartsGiveNoEstimate() {
    QFETCH(double, overhead);
    QFETCH(double, perStep);
    QFETCH(double, decode);
    QFETCH(int, count);
    QFETCH(int, steps);
    const Timing timing{overhead, perStep, decode};
    QCOMPARE(timing.estimate(count, steps), -1.0);
    const Timing nothing;
    QVERIFY(!nothing.isKnown());
    QCOMPARE(nothing.estimate(4, 8), -1.0);
}

void LimgenTests::timings_keyAndDefaultFile() {
    QCOMPARE(Timings::key("m", "q4_k", "vulkan"), QString("m/q4_k/vulkan"));
    EnvGuard guard({"XDG_STATE_HOME", "XDG_CONFIG_HOME"});
    qputenv("XDG_STATE_HOME", "/tmp/limgen-test/xdgstate");
    QCOMPARE(Timings::defaultFile(), QString("/tmp/limgen-test/xdgstate/limgen/bench.json"));
    qputenv("XDG_CONFIG_HOME", "/tmp/limgen-test/xdgconfig");
    QCOMPARE(Config::defaultFile(), QString("/tmp/limgen-test/xdgconfig/limgen/config.json"));
}

void LimgenTests::timings_recordBlendsHalfAndHalf() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString file = fixture.root.path() + "/state/bench.json";
    Timings timings(file);
    QVERIFY(timings.record("k", {10.0, 1.0, 2.0}));
    Timing got = timings.get("k");
    QCOMPARE(got.overheadSeconds, 10.0); // the first measurement is taken as it is
    QCOMPARE(got.secondsPerStep, 1.0);
    QCOMPARE(got.decodeSecondsPerImage, 2.0);
    QVERIFY(timings.record("k", {20.0, 3.0, 4.0}));
    got = timings.get("k");
    QCOMPARE(got.overheadSeconds, 15.0); // an even mix with the last value
    QCOMPARE(got.secondsPerStep, 2.0);
    QCOMPARE(got.decodeSecondsPerImage, 3.0);
    // A new reader sees the saved values; another key is separate.
    const Timings reread(file);
    QCOMPARE(reread.get("k").secondsPerStep, 2.0);
    QVERIFY(!reread.get("other").isKnown());
    QCOMPARE(reread.get("other").overheadSeconds, -1.0);
}

void LimgenTests::timings_recordKeepsOtherKeysAndUnknownParts() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString file = fixture.root.path() + "/state/bench.json";
    QVERIFY(writeFile(file, R"({"other": {"overheadSeconds": 1, "secondsPerStep": 2, "decodeSecondsPerImage": 3},
                                "extra": "kept"})"));
    Timings timings(file);
    QVERIFY(timings.record("k", {-1.0, 0.5, -1.0}));
    const QJsonObject json = QJsonDocument::fromJson(readFile(file)).object();
    QCOMPARE(json["extra"].toString(), QString("kept"));
    QCOMPARE(json["other"].toObject()["secondsPerStep"].toDouble(), 2.0);
    QVERIFY(json.contains("k"));
    // Only some parts known: the others stay unknown, and so does the estimate.
    Timing got = timings.get("k");
    QCOMPARE(got.secondsPerStep, 0.5);
    QCOMPARE(got.overheadSeconds, -1.0);
    QVERIFY(!got.isKnown());
    // A later measurement without a part leaves what is stored.
    QVERIFY(timings.record("k", {3.0, -1.0, -1.0}));
    got = timings.get("k");
    QCOMPARE(got.overheadSeconds, 3.0);
    QCOMPARE(got.secondsPerStep, 0.5);
    QVERIFY(!got.isKnown());
    QVERIFY(timings.record("k", {-1.0, -1.0, 2.0}));
    got = timings.get("k");
    QVERIFY(got.isKnown());
    QCOMPARE(got.estimate(1, 2), 3.0 + 1 * (2 * 0.5 + 2.0));
}

void LimgenTests::timings_missingOrGarbledFile() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString dir = fixture.root.path() + "/state";
    {
        const Timings missing(dir + "/none.json");
        QVERIFY(!missing.get("k").isKnown());
        QCOMPARE(missing.get("k").secondsPerStep, -1.0);
    }
    for (const QByteArray &content : {QByteArray("not json at all"), QByteArray("[1, 2, 3]"), QByteArray(""),
                                      QByteArray("{\"k\": 5}"), QByteArray("{\"k\": {\"secondsPerStep\": \"fast\"}}")}) {
        const QString file = dir + "/bench.json";
        QVERIFY(writeFile(file, content));
        Timings timings(file);
        QVERIFY2(!timings.get("k").isKnown(), content.constData());
        // Recording still works, and the file is valid JSON afterwards.
        QVERIFY(timings.record("k", {1.0, 1.0, 1.0}));
        QVERIFY(QJsonDocument::fromJson(readFile(file)).isObject());
        QVERIFY(Timings(file).get("k").isKnown());
    }
}

void LimgenTests::timings_saveIsAtomicAndReportsFailure() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString dir = fixture.root.path() + "/benchstate";
    const QString file = dir + "/bench.json";
    Timings timings(file);
    QVERIFY(timings.record("k", {1.0, 1.0, 1.0}));
    // Written beside the file and renamed: only the file itself is left.
    QCOMPARE(QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{"bench.json"});
    // A folder that cannot be created: the record fails and says so.
    QVERIFY(writeFile(fixture.root.path() + "/blocker", "x"));
    Timings blocked(fixture.root.path() + "/blocker/state/bench.json");
    QVERIFY(!blocked.record("k", {1.0, 1.0, 1.0}));
    QVERIFY(QFileInfo(fixture.root.path() + "/blocker").isFile());
    if (geteuid() != 0) {
        // A read-only folder: the old file is untouched and no temp file appears.
        const QByteArray before = readFile(file);
        QVERIFY(QFile::setPermissions(dir, QFile::ReadOwner | QFile::ExeOwner));
        const bool saved = timings.record("k", {9.0, 9.0, 9.0});
        QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QVERIFY(!saved);
        QCOMPARE(readFile(file), before);
        QCOMPARE(QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{"bench.json"});
    }
}

void LimgenTests::config_roundTripAndUnknownKeysAreKept() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString file = fixture.root.path() + "/config/limgen/config.json";
    Config config(file);
    QCOMPARE(config.file(), file);
    QCOMPARE(config.modelId(), QString());
    QCOMPARE(config.styleId(), QString());
    config.setModelId("flux2-klein-4b");
    config.setStyleId("painting");
    QString error;
    QVERIFY2(config.save(&error), qPrintable(error)); // creates the folder
    const QJsonObject json = QJsonDocument::fromJson(readFile(file)).object();
    QCOMPARE(json["model"].toString(), QString("flux2-klein-4b"));
    QCOMPARE(json["style"].toString(), QString("painting"));
    const Config again(file);
    QCOMPARE(again.modelId(), QString("flux2-klein-4b"));
    QCOMPARE(again.styleId(), QString("painting"));
    // Keys a newer limgen wrote survive a save by this one.
    QVERIFY(writeFile(file, R"({"model": "a", "future": {"x": [1, 2]}, "n": 3})"));
    Config older(file);
    older.setModelId("b");
    QVERIFY(older.save());
    const QJsonObject kept = QJsonDocument::fromJson(readFile(file)).object();
    QCOMPARE(kept["model"].toString(), QString("b"));
    QCOMPARE(kept["n"].toInt(), 3);
    QCOMPARE(kept["future"].toObject()["x"].toArray().size(), 2);
}

void LimgenTests::config_missingOrGarbledFileGivesDefaults() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString file = fixture.root.path() + "/config/limgen/config.json";
    QCOMPARE(Config(file).modelId(), QString());
    for (const QByteArray &content : {QByteArray("not json"), QByteArray("[1]"), QByteArray(""),
                                      QByteArray("{\"model\": 5, \"style\": null}")}) {
        QVERIFY(writeFile(file, content));
        Config config(file);
        QCOMPARE(config.modelId(), QString());
        QCOMPARE(config.styleId(), QString());
        // It is rewritten as a valid file on the next save.
        config.setModelId("m");
        QVERIFY(config.save());
        QCOMPARE(QJsonDocument::fromJson(readFile(file)).object()["model"].toString(), QString("m"));
    }
}

void LimgenTests::config_saveFailureAndNoPartialFile() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString dir = fixture.root.path() + "/config/limgen";
    const QString file = dir + "/config.json";
    Config config(file);
    config.setModelId("m");
    QVERIFY(config.save());
    QCOMPARE(QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{"config.json"});
    // The folder cannot be made below a plain file.
    QVERIFY(writeFile(fixture.root.path() + "/blocker", "x"));
    Config blocked(fixture.root.path() + "/blocker/limgen/config.json");
    blocked.setModelId("m");
    QString error;
    QVERIFY(!blocked.save(&error));
    QVERIFY2(error.contains("config.json"), qPrintable(error));
    QVERIFY(!blocked.save()); // a null error pointer is accepted
    if (geteuid() != 0) {
        const QByteArray before = readFile(file);
        QVERIFY(QFile::setPermissions(dir, QFile::ReadOwner | QFile::ExeOwner));
        config.setModelId("changed");
        const bool saved = config.save(&error);
        QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QVERIFY(!saved);
        QVERIFY(error.contains("config.json"));
        QCOMPARE(readFile(file), before); // the old file is intact
        QCOMPARE(QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{"config.json"});
    }
}

// ============================== L4: Backend, in process ===================

namespace {

void stockTheme(Fixture &fixture) {
    fixture.addTheme("night-city", stockToml);
    fixture.switchTo("night-city");
}

// A test that fails halfway must not leave a batch or a download running when
// its Backend is deleted: ~Backend with a running batch corrupts the heap (see
// backend_destroyingWhileABatchRunsIsSafe), which would take the whole run
// down. Everything is stopped and waited for first.
void settleBackend(Backend *backend) {
    if (!backend)
        return;
    backend->stopAll();
    for (int i = 0; i < 200 && (backend->running() || backend->downloading()); ++i)
        QTest::qWait(50);
}

// A sandbox with a Backend on it. Environment changes a test wants (a fake
// engine mode, a catalogue) are made before create().
struct BackendRig {
    Sandbox sb;
    std::unique_ptr<Omarchy> omarchy;
    std::unique_ptr<Backend> backend;
    Catalogue catalogue = Catalogue::builtIn();

    explicit BackendRig(bool theme = true, bool installed = true, int usedMiB = 2000) : sb(usedMiB) {
        if (theme)
            stockTheme(sb.fixture);
        if (installed) {
            installModel(catalogue, "z-image-turbo", sb.fixture.models);
            const CatalogueFile &upscaler = *catalogue.upscaler();
            makeSparse(Catalogue::path(upscaler, sb.fixture.models), upscaler.bytes);
        }
    }
    void create(const QString &engine = fakeEngine) {
        settleBackend(backend.get());
        backend.reset();
        omarchy = std::make_unique<Omarchy>(sb.paths());
        backend = std::make_unique<Backend>(omarchy.get(), engine);
    }
    ~BackendRig() {
        settleBackend(backend.get());
        backend.reset();
        omarchy.reset();
    }
};

// engineChecking() goes false when the worker thread ends, a moment before the
// result is stored, so "done" also asks for the result.
#define WAIT_FOR_ENGINE(rig) \
    QTRY_VERIFY_WITH_TIMEOUT(!(rig).backend->engineChecking() && ((rig).backend->engineReady() || !(rig).backend->engineProblem().isEmpty()), 20000)
#define WAIT_FOR_READY(rig) QTRY_VERIFY_WITH_TIMEOUT((rig).backend->engineReady(), 20000)
#define WAIT_FOR_IDLE(rig) QTRY_VERIFY_WITH_TIMEOUT(!(rig).backend->running() && !(rig).backend->downloading(), 40000)

// The built prompt for a subject, a style index and a theme's colours.
QString builtPromptFor(const QString &subject, int styleIndex, const QByteArray &toml) {
    Prompt::Style style;
    Prompt::styleFromId(Prompt::styleIds().at(styleIndex), &style);
    return Prompt::build(subject, Palette::fromToml(toml), style).positive;
}

// The states each draft went through, collapsed, from the drafts signal.
struct DraftWatcher {
    QList<QStringList> states{{}, {}, {}, {}};
    bool previewWhileSampling = false;
    QString previewSeen;
    QList<QVariantList> snapshots;
    void attach(Backend &backend) {
        QObject::connect(&backend, &Backend::draftsChanged, &backend, [this, &backend] {
            const QVariantList drafts = backend.drafts();
            snapshots << drafts;
            for (int i = 0; i < drafts.size() && i < 4; ++i) {
                const QVariantMap draft = drafts[i].toMap();
                const QString state = draft["state"].toString();
                if (states[i].isEmpty() || states[i].last() != state)
                    states[i] << state;
                if (state == "sampling" && !draft["preview"].toString().isEmpty()) {
                    previewWhileSampling = true;
                    previewSeen = draft["preview"].toString();
                }
            }
        });
    }
};

// A sandbox with a Backend, the local HTTP server and the test catalogue
// (small files) that LIMGEN_CATALOGUE and LIMGEN_DOWNLOAD_BASE point at.
struct DownloadRig {
    Sandbox sb;
    HttpServer server;
    World world;
    std::unique_ptr<Omarchy> omarchy;
    std::unique_ptr<Backend> backend;

    DownloadRig() : world(makeWorld()) {
        for (const Blob &blob : world.blobs)
            server.serve(blob.path(), blob.data);
        const QString catalogueFile = sb.fixture.root.path() + "/catalogue.json";
        writeFile(catalogueFile, world.json);
        qputenv("LIMGEN_CATALOGUE", catalogueFile.toUtf8());
        qputenv("LIMGEN_DOWNLOAD_BASE", QString("http://127.0.0.1:%1").arg(server.port()).toUtf8());
        stockTheme(sb.fixture);
        omarchy = std::make_unique<Omarchy>(sb.fixture.paths());
        backend = std::make_unique<Backend>(omarchy.get(), fakeEngine);
    }
    ~DownloadRig() {
        settleBackend(backend.get());
        backend.reset();
        omarchy.reset();
    }
    QString target(const QString &key) const { return Catalogue::path(world.file(key), sb.fixture.models); }
    QString part(const QString &key) const { return target(key) + ".part"; }
    HttpServer::Route &route(const QString &key) { return server.routes[world.blob(key).path()]; }
};

} // namespace

void LimgenTests::backend_engineProbeGoesFromCheckingToReady() {
    BackendRig rig;
    rig.create();
    QSignalSpy changed(rig.backend.get(), &Backend::engineChanged);
    QVERIFY(rig.backend->engineChecking()); // the probe runs off the UI thread
    QVERIFY(!rig.backend->engineReady());
    WAIT_FOR_READY(rig);
    QVERIFY(!rig.backend->engineChecking());
    QCOMPARE(rig.backend->engineProblem(), QString());
    QVERIFY2(rig.backend->gpuText().contains("Fake GPU"), qPrintable(rig.backend->gpuText()));
    QVERIFY(rig.backend->gpuText().contains("GB"));
    QVERIFY(changed.count() >= 1);
}

void LimgenTests::backend_engineProblemsAreShownAndRecheckRecovers() {
    {
        BackendRig rig;
        rig.create(rig.sb.fixture.root.path() + "/no-such-engine");
        WAIT_FOR_ENGINE(rig);
        QVERIFY(!rig.backend->engineReady());
        QVERIFY2(rig.backend->engineProblem().contains("no-such-engine"), qPrintable(rig.backend->engineProblem()));
    }
    for (const QString &mode : QStringList{"no-gpu", "old-version"}) {
        BackendRig rig;
        qputenv("FAKE_SD_CLI_MODE", mode.toUtf8());
        rig.create();
        WAIT_FOR_ENGINE(rig);
        QVERIFY2(!rig.backend->engineReady(), qPrintable(mode));
        QVERIFY2(!rig.backend->engineProblem().isEmpty(), qPrintable(mode));
        QVERIFY(!rig.backend->canGenerate()); // whatever the subject
        rig.backend->setSubject("a fox");
        QVERIFY(!rig.backend->canGenerate());
        // The user fixes the engine and checks again.
        qunsetenv("FAKE_SD_CLI_MODE");
        rig.backend->recheckEngine();
        QVERIFY(rig.backend->engineChecking());
        rig.backend->recheckEngine(); // while a check runs, a second one is ignored
        WAIT_FOR_READY(rig);
        QCOMPARE(rig.backend->engineProblem(), QString());
        QVERIFY(rig.backend->canGenerate());
    }
}

void LimgenTests::backend_modelsListAndInstalledState() {
    {
        BackendRig rig(true, false); // nothing installed
        rig.create();
        QCOMPARE(rig.backend->modelId(), QString("z-image-turbo"));
        QVERIFY(!rig.backend->modelInstalled());
        QVERIFY(!rig.backend->downloadSize().isEmpty());
        QVERIFY2(rig.backend->diskText().contains(rig.sb.fixture.models), qPrintable(rig.backend->diskText()));
        QVERIFY(!rig.backend->licenceText().isEmpty());
        QVERIFY(rig.backend->licenceText().contains(rig.catalogue.defaultModel()->licenceId));
        QVERIFY(rig.backend->licenceUrl().startsWith("https://"));
        QVERIFY(!rig.backend->publisher().isEmpty());
        QCOMPARE(rig.backend->modelName(), rig.catalogue.defaultModel()->name);
        QVERIFY(!rig.backend->diskTooFull());
        const QVariantList models = rig.backend->models();
        QCOMPARE(models.size(), rig.catalogue.models().size());
        int selected = 0;
        for (const QVariant &value : models) {
            const QVariantMap model = value.toMap();
            for (const QString &field : QStringList{"id", "name", "publisher", "licence", "isDefault", "installed",
                                                    "size", "runsHere", "selected"})
                QVERIFY2(model.contains(field), qPrintable(field));
            QCOMPARE(model["installed"].toBool(), false);
            QVERIFY(!model["size"].toString().isEmpty());
            QVERIFY(!model["licence"].toString().isEmpty());
            selected += model["selected"].toBool();
            QCOMPARE(model["isDefault"].toBool(), model["id"].toString() == "z-image-turbo");
            QCOMPARE(model["selected"].toBool(), model["id"].toString() == "z-image-turbo");
        }
        QCOMPARE(selected, 1);
    }
    {
        BackendRig rig; // z-image-turbo installed, with the preview decoder and the upscaler
        rig.create();
        QVERIFY(rig.backend->modelInstalled());
        const QVariantMap z = rig.backend->models().at(0).toMap();
        QCOMPARE(z["id"].toString(), QString("z-image-turbo"));
        QCOMPARE(z["installed"].toBool(), true);
        QCOMPARE(z["size"].toString(), QString());
        // The other model shares the encoder, but is not installed.
        QCOMPARE(rig.backend->models().at(1).toMap()["installed"].toBool(), false);
    }
    {
        // One missing file, here the preview decoder the window needs, means not installed.
        BackendRig rig;
        QVERIFY(QFile::remove(Catalogue::path(*rig.catalogue.file("taef1"), rig.sb.fixture.models)));
        rig.create();
        QVERIFY(!rig.backend->modelInstalled());
    }
    {
        // A file of the wrong size too.
        BackendRig rig;
        const CatalogueFile &vae = *rig.catalogue.file("flux1-vae");
        QVERIFY(makeSparse(Catalogue::path(vae, rig.sb.fixture.models), vae.bytes - 1));
        rig.create();
        QVERIFY(!rig.backend->modelInstalled());
    }
}

void LimgenTests::backend_modelsRunsHereFollowsFreeMemory() {
    auto runsHere = [](BackendRig &rig) -> int {
        rig.create();
        for (int i = 0; i < 400 && !(rig.backend->engineReady() || !rig.backend->engineProblem().isEmpty()); ++i)
            QTest::qWait(50);
        return rig.backend->models().at(0).toMap()["runsHere"].toInt();
    };
    BackendRig enough(true, true, 2000); // 10000 MiB free, 5000 needed
    QCOMPARE(runsHere(enough), 1);
    BackendRig low(true, true, 9000); // 3000 free
    QCOMPARE(runsHere(low), 0);
    BackendRig unknown(true, true);
    qputenv("LIMGEN_NVIDIA_SMI", (unknown.sb.fixture.root.path() + "/no-smi").toUtf8());
    QCOMPARE(runsHere(unknown), -1);
}

void LimgenTests::backend_selectModelPersistsAndRefreshes() {
    BackendRig rig;
    rig.create();
    // The config is the sandbox's, never the user's.
    QCOMPARE(Config::defaultFile(), rig.sb.configFile());
    QVERIFY(!QFileInfo::exists(rig.sb.configFile())); // nothing is written until a choice is made
    QSignalSpy modelsChanged(rig.backend.get(), &Backend::modelsChanged);
    QVERIFY(rig.backend->modelInstalled());
    rig.backend->selectModel("flux2-klein-4b");
    QCOMPARE(rig.backend->modelId(), QString("flux2-klein-4b"));
    QCOMPARE(rig.backend->modelName(), rig.catalogue.model("flux2-klein-4b")->name);
    QVERIFY(!rig.backend->modelInstalled()); // refreshed: klein's own files are not there
    QVERIFY(modelsChanged.count() >= 1);
    QCOMPARE(QJsonDocument::fromJson(readFile(rig.sb.configFile())).object()["model"].toString(),
             QString("flux2-klein-4b"));
    QVERIFY(rig.backend->models().at(1).toMap()["selected"].toBool());
    // The same model again, and an unknown one, change nothing.
    modelsChanged.clear();
    rig.backend->selectModel("flux2-klein-4b");
    rig.backend->selectModel("nope");
    rig.backend->selectModel("");
    QCOMPARE(modelsChanged.count(), 0);
    QCOMPARE(rig.backend->modelId(), QString("flux2-klein-4b"));
    // A new window starts with the saved choice.
    rig.create();
    QCOMPARE(rig.backend->modelId(), QString("flux2-klein-4b"));
    rig.backend->selectModel("z-image-turbo");
    QVERIFY(rig.backend->modelInstalled());
    // The style is saved too, and the other keys of the file stay.
    QVERIFY(writeFile(rig.sb.configFile(), R"({"model": "z-image-turbo", "future": {"x": 1}})"));
    rig.create();
    QCOMPARE(rig.backend->styleIndex(), 1); // cinematic, the default
    rig.backend->setStyleIndex(2);
    QCOMPARE(rig.backend->styleIndex(), 2);
    QJsonObject config = QJsonDocument::fromJson(readFile(rig.sb.configFile())).object();
    QCOMPARE(config["style"].toString(), QString("painting"));
    QCOMPARE(config["future"].toObject()["x"].toInt(), 1);
    rig.backend->setStyleIndex(9); // out of range: ignored
    rig.backend->setStyleIndex(-1);
    QCOMPARE(rig.backend->styleIndex(), 2);
    rig.create();
    QCOMPARE(rig.backend->styleIndex(), 2);
    // Only the sandbox's config folder was written.
    QCOMPARE(filesUnder(rig.sb.fixture.root.path() + "/config"), QStringList{rig.sb.configFile()});
}

void LimgenTests::backend_unknownModelOrStyleInConfigFallsBack() {
    BackendRig rig;
    QVERIFY(writeFile(rig.sb.configFile(), R"({"model": "gone", "style": "gone"})"));
    rig.create();
    QCOMPARE(rig.backend->modelId(), QString("z-image-turbo"));
    QCOMPARE(rig.backend->styleIndex(), 1);
    QVERIFY(writeFile(rig.sb.configFile(), "not json"));
    rig.create();
    QCOMPARE(rig.backend->modelId(), QString("z-image-turbo"));
}

void LimgenTests::backend_downloadInstallsTheModel() {
    DownloadRig rig;
    QVERIFY(rig.server.listening);
    WAIT_FOR_READY(rig);
    QCOMPARE(rig.backend->modelId(), QString("m"));
    QVERIFY(!rig.backend->modelInstalled());
    QVERIFY(!rig.backend->downloadSize().isEmpty());
    QVERIFY(!rig.backend->canGenerate());
    QList<double> fractions;
    QObject::connect(rig.backend.get(), &Backend::downloadChanged, rig.backend.get(),
                     [&] { fractions << rig.backend->downloadFraction(); });
    rig.backend->startDownload();
    QVERIFY(rig.backend->downloading());
    QVERIFY(rig.backend->busy());
    QVERIFY(QFileInfo::exists(rig.sb.fixture.models + "/.download.lock")); // held while it runs
    rig.backend->startDownload(); // a second start while downloading is ignored
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->downloading(), 30000);
    QVERIFY2(rig.backend->modelInstalled(), qPrintable(rig.backend->status()));
    QVERIFY(!rig.backend->statusIsError());
    QVERIFY2(rig.backend->status().contains("installed"), qPrintable(rig.backend->status()));
    for (const QString &key : QStringList{"a", "c", "b", "u"}) {
        QCOMPARE(readFile(rig.target(key)), rig.world.blob(key).data);
        QVERIFY(!QFileInfo::exists(rig.part(key)));
    }
    QVERIFY(!QFileInfo::exists(rig.sb.fixture.models + "/.download.lock"));
    QCOMPARE(rig.server.paths(), (QStringList{"/files/a.bin", "/files/c.bin", "/files/b.bin", "/files/u.bin"}));
    QVERIFY(rig.backend->models().at(0).toMap()["installed"].toBool());
    // The fraction rose from the start to the whole.
    QVERIFY(!fractions.isEmpty());
    for (int i = 1; i < fractions.size(); ++i)
        QVERIFY2(fractions[i] >= fractions[i - 1] - 1e-9, "the fraction never goes backwards");
    QVERIFY(fractions.first() < 0.5);
    QVERIFY2(qAbs(fractions.last() - 1.0) < 1e-9, qPrintable(QString::number(fractions.last())));
    // A model that is installed has nothing to download.
    rig.backend->startDownload();
    QVERIFY(!rig.backend->downloading());
    QCOMPARE(rig.server.requests.size(), 4);
    QVERIFY(rig.backend->canGenerate() == false); // no subject yet
    rig.backend->setSubject("a fox");
    QVERIFY(rig.backend->canGenerate());
}

void LimgenTests::backend_downloadPausesAndResumes() {
    DownloadRig rig;
    WAIT_FOR_READY(rig);
    const QByteArray &data = rig.world.blob("a").data;
    HttpServer::Route &route = rig.route("a");
    route.chunk = 10000;
    route.chunkDelayMs = 100;
    rig.backend->startDownload();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(rig.part("a")).size() > 0, 20000);
    rig.backend->pauseDownload();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->downloading(), 20000);
    const qint64 kept = QFileInfo(rig.part("a")).size();
    QVERIFY2(kept > 0 && kept < data.size(), qPrintable(QString::number(kept)));
    QVERIFY(!QFileInfo::exists(rig.target("a")));
    QVERIFY(!rig.backend->statusIsError());
    QVERIFY2(rig.backend->status().toLower().contains("paused"), qPrintable(rig.backend->status()));
    QVERIFY(!QFileInfo::exists(rig.sb.fixture.models + "/.download.lock")); // released
    QVERIFY(!rig.backend->modelInstalled());
    QVERIFY2(rig.backend->downloadSize().contains("already"), qPrintable(rig.backend->downloadSize()));
    // Download again: it resumes where it stopped.
    route.chunk = 0;
    rig.backend->startDownload();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->downloading(), 30000);
    QVERIFY2(rig.backend->modelInstalled(), qPrintable(rig.backend->status()));
    QCOMPARE(rig.server.rangesOf("/files/a.bin"), (QList<QByteArray>{"", "bytes=" + QByteArray::number(kept) + "-"}));
    QCOMPARE(readFile(rig.target("a")), data);
}

void LimgenTests::backend_downloadIsRefusedWhileLocked() {
    DownloadRig rig;
    WAIT_FOR_READY(rig);
    QVERIFY(QDir().mkpath(rig.sb.fixture.models));
    QLockFile lock(rig.sb.fixture.models + "/.download.lock");
    QVERIFY(lock.tryLock(0)); // another limgen, or limgen models pull
    rig.backend->startDownload();
    QVERIFY(!rig.backend->downloading());
    QVERIFY(rig.backend->statusIsError());
    QVERIFY2(rig.backend->status().contains(rig.sb.fixture.models), qPrintable(rig.backend->status()));
    QCOMPARE(rig.server.requests.size(), 0);
    QCOMPARE(filesUnder(rig.sb.fixture.models), QStringList());
    QVERIFY(lock.isLocked());
    // When the other download ends, this one can start.
    lock.unlock();
    rig.backend->startDownload();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->downloading() && rig.backend->modelInstalled(), 30000);
}

void LimgenTests::backend_promptFollowsSubjectStyleAndTheme() {
    BackendRig rig;
    rig.create();
    QSignalSpy changed(rig.backend.get(), &Backend::promptChanged);
    QCOMPARE(rig.backend->fullPrompt(), builtPromptFor("", 1, stockToml));
    QVERIFY(!rig.backend->promptEdited());
    rig.backend->setSubject("oil rig at dusk");
    QCOMPARE(rig.backend->fullPrompt(), builtPromptFor("oil rig at dusk", 1, stockToml));
    QVERIFY(rig.backend->fullPrompt().startsWith("oil rig at dusk, cinematic wide shot, near-black indigo"));
    QVERIFY(changed.count() >= 1);
    rig.backend->setStyleIndex(0);
    QCOMPARE(rig.backend->fullPrompt(), builtPromptFor("oil rig at dusk", 0, stockToml));
    QVERIFY(rig.backend->fullPrompt().contains("photograph, wide angle"));
    // The theme is switched: the built prompt takes the new colours.
    const QByteArray other = tomlWith("#fafafa", "#111111", "#ff0000");
    QVERIFY(rig.sb.fixture.addTheme("other", other));
    QVERIFY(rig.sb.fixture.switchTo("other"));
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->fullPrompt(), builtPromptFor("oil rig at dusk", 0, other), 5000);
    QCOMPARE(rig.backend->themeName(), QString("other"));
}

void LimgenTests::backend_editedPromptSticksUntilReset() {
    BackendRig rig;
    rig.create();
    rig.backend->setSubject("oil rig");
    QSignalSpy changed(rig.backend.get(), &Backend::promptChanged);
    rig.backend->setFullPrompt("my own prompt");
    QVERIFY(rig.backend->promptEdited());
    QCOMPARE(rig.backend->fullPrompt(), QString("my own prompt"));
    QCOMPARE(changed.count(), 1);
    rig.backend->setFullPrompt("my own prompt"); // the same text again: nothing changes
    QCOMPARE(changed.count(), 1);
    // An edit sticks across a new subject, a new style and a theme switch.
    rig.backend->setSubject("a lighthouse");
    rig.backend->setStyleIndex(3);
    QVERIFY(rig.sb.fixture.addTheme("other", tomlWith("#fafafa", "#111111", "#ff0000")));
    QVERIFY(rig.sb.fixture.switchTo("other"));
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->themeName(), QString("other"), 5000);
    QVERIFY(rig.backend->promptEdited());
    QCOMPARE(rig.backend->fullPrompt(), QString("my own prompt"));
    // Reset returns to the built prompt, which follows again.
    rig.backend->resetPrompt();
    QVERIFY(!rig.backend->promptEdited());
    QCOMPARE(rig.backend->fullPrompt(), builtPromptFor("a lighthouse", 3, tomlWith("#fafafa", "#111111", "#ff0000")));
    changed.clear();
    rig.backend->resetPrompt(); // nothing to reset
    QCOMPARE(changed.count(), 0);
    rig.backend->setSubject("a harbour");
    QCOMPARE(rig.backend->fullPrompt(), builtPromptFor("a harbour", 3, tomlWith("#fafafa", "#111111", "#ff0000")));
    // Text equal to the built prompt is not an edit.
    changed.clear();
    rig.backend->setFullPrompt(rig.backend->fullPrompt());
    QVERIFY(!rig.backend->promptEdited());
    QCOMPARE(changed.count(), 0);
    // And typing the built text back over an edit ends the edit.
    rig.backend->setFullPrompt("something else");
    QVERIFY(rig.backend->promptEdited());
    rig.backend->setFullPrompt(builtPromptFor("a harbour", 3, tomlWith("#fafafa", "#111111", "#ff0000")));
    QVERIFY(!rig.backend->promptEdited());
    rig.backend->setSubject("a bridge");
    QCOMPARE(rig.backend->fullPrompt(), builtPromptFor("a bridge", 3, tomlWith("#fafafa", "#111111", "#ff0000")));
}

void LimgenTests::backend_canGenerateRules() {
    {
        BackendRig rig;
        rig.create();
        rig.backend->setSubject("a fox");
        QVERIFY(rig.backend->engineChecking());
        QVERIFY2(!rig.backend->canGenerate(), "the engine is not ready yet");
        rig.backend->generate(); // does nothing
        QVERIFY(!rig.backend->running());
        QVERIFY(rig.backend->drafts().isEmpty());
        WAIT_FOR_READY(rig);
        QVERIFY(rig.backend->canGenerate());
        rig.backend->setSubject("");
        QVERIFY(!rig.backend->canGenerate());
        rig.backend->setSubject("   ");
        QVERIFY(!rig.backend->canGenerate());
        // An edited prompt stands in for the subject.
        rig.backend->setFullPrompt("a complete prompt");
        QVERIFY(rig.backend->canGenerate());
        rig.backend->setFullPrompt("  ");
        QVERIFY(rig.backend->promptEdited());
        QVERIFY(!rig.backend->canGenerate());
        rig.backend->generate();
        QVERIFY(!rig.backend->running());
        QVERIFY(rig.backend->drafts().isEmpty());
        QVERIFY(!rig.backend->canRerun()); // nothing was made yet
    }
    {
        BackendRig rig(true, false); // model not installed
        rig.create();
        WAIT_FOR_READY(rig);
        rig.backend->setSubject("a fox");
        QVERIFY(!rig.backend->canGenerate());
        rig.backend->generate();
        QVERIFY(!rig.backend->running());
    }
    {
        BackendRig rig; // busy: a batch is running
        qputenv("FAKE_SD_CLI_MODE", "hang");
        rig.create();
        WAIT_FOR_READY(rig);
        rig.backend->setSubject("a fox");
        QVERIFY(rig.backend->canGenerate());
        rig.backend->generate();
        QVERIFY(rig.backend->running());
        QVERIFY(rig.backend->busy());
        QVERIFY(!rig.backend->canGenerate());
        rig.backend->generate(); // ignored while busy
        QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(rig.sb.fakeLog()).isEmpty(), 15000);
        QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 1);
        QVERIFY(!rig.backend->canRerun());
        rig.backend->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 15000);
        QVERIFY(rig.backend->canGenerate());
    }
}

namespace {
// The sidecar of a draft the window shows.
LibraryEntry entryOf(const BackendRig &rig, const QVariantMap &draft) {
    return Library(rig.sb.library()).find(draft["id"].toString());
}
} // namespace

void LimgenTests::backend_generateFillsFourDrafts() {
    BackendRig rig;
    rig.create();
    WAIT_FOR_READY(rig);
    DraftWatcher watcher;
    watcher.attach(*rig.backend);
    QSignalSpy stateChanged(rig.backend.get(), &Backend::stateChanged);
    rig.backend->setSubject("oil rig at dusk");
    rig.backend->generate();
    QVERIFY(rig.backend->running());
    QCOMPARE(rig.backend->drafts().size(), 4);
    QCOMPARE(rig.backend->drafts().at(0).toMap()["state"].toString(), QString("waiting"));
    QCOMPARE(rig.backend->drafts().at(3).toMap()["state"].toString(), QString("waiting"));
    QVERIFY(!rig.backend->canGenerate());
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 40000);
    QVERIFY2(!rig.backend->statusIsError(), qPrintable(rig.backend->status()));
    QCOMPARE(rig.backend->status(), QString());
    const QVariantList drafts = rig.backend->drafts();
    QCOMPARE(drafts.size(), 4);
    QSet<QString> ids;
    for (int i = 0; i < 4; ++i) {
        const QVariantMap draft = drafts[i].toMap();
        QCOMPARE(draft["state"].toString(), QString("done"));
        // The states in order: waiting, then sampling, sampled and done, none skipped.
        QCOMPARE(watcher.states[i], (QStringList{"waiting", "sampling", "sampled", "done"}));
        QVERIFY2(draft["image"].toString().startsWith("file://"), qPrintable(draft["image"].toString()));
        const QString path = QUrl(draft["image"].toString()).toLocalFile();
        QVERIFY2(QFileInfo(path).isFile(), qPrintable(path));
        QVERIFY(path.startsWith(rig.sb.library()));
        QCOMPARE(QImageReader(path).size(), QSize(1344, 768));
        QVERIFY(Library::isValidId(draft["id"].toString()));
        ids.insert(draft["id"].toString());
        QCOMPARE(draft["steps"].toInt(), 8);
        QCOMPARE(draft["preview"].toString(), QString()); // the preview gives way to the draft
        if (i > 0)
            QCOMPARE(draft["seed"].toDouble(), drafts[0].toMap()["seed"].toDouble() + i);
        const LibraryEntry entry = entryOf(rig, draft);
        QVERIFY(entry.isValid());
        QCOMPARE(entry.metadata["seed"].toDouble(), draft["seed"].toDouble());
        QCOMPARE(entry.metadata["subject"].toString(), QString("oil rig at dusk"));
        QCOMPARE(entry.metadata["theme"].toObject()["slug"].toString(), QString("night-city"));
    }
    QCOMPARE(ids.size(), 4);
    // Previews showed while sampling, and came from the batch folder.
    QVERIFY(watcher.previewWhileSampling);
    QVERIFY2(QUrl(watcher.previewSeen).toLocalFile().startsWith(rig.sb.fixture.cache + "/limgen/batch-"),
             qPrintable(watcher.previewSeen));
    QVERIFY(rig.backend->canRerun());
    QVERIFY(rig.backend->canGenerate());
    QVERIFY2(rig.backend->estimate().contains("4") && !rig.backend->estimate().contains("%n"),
             qPrintable(rig.backend->estimate()));
    QCOMPARE(filesUnder(rig.sb.library()).size(), 8);
    QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList()); // the batch folder is gone
    QVERIFY(stateChanged.count() >= 2);
}

void LimgenTests::backend_rerunReusesThePromptWithNewSeeds() {
    BackendRig rig;
    rig.create();
    WAIT_FOR_READY(rig);
    QVERIFY(!rig.backend->canRerun());
    rig.backend->rerun(); // nothing to repeat
    QVERIFY(!rig.backend->running());
    rig.backend->setSubject("oil rig at dusk");
    rig.backend->generate();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 40000);
    const QVariantMap first = rig.backend->drafts().at(0).toMap();
    const QString firstPrompt = entryOf(rig, first).metadata["prompt"].toString();
    QVERIFY(!firstPrompt.isEmpty());
    QVERIFY(rig.backend->canRerun());
    rig.backend->rerun();
    QVERIFY(rig.backend->running());
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 40000);
    const QVariantMap second = rig.backend->drafts().at(0).toMap();
    QCOMPARE(second["state"].toString(), QString("done"));
    QVERIFY2(second["seed"].toDouble() != first["seed"].toDouble(), "new seeds");
    QVERIFY(second["id"].toString() != first["id"].toString());
    QCOMPARE(entryOf(rig, second).metadata["prompt"].toString(), firstPrompt);
    QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 2);
    QCOMPARE(valueAfter(imageCalls(rig.sb.fakeLog())[1], "--prompt"), firstPrompt);
    QCOMPARE(filesUnder(rig.sb.library()).size(), 16);
}

void LimgenTests::backend_rerunRecordsTheOriginalSubjectAndStyle() {
    BackendRig rig;
    rig.create();
    WAIT_FOR_READY(rig);
    rig.backend->setSubject("oil rig at dusk");
    rig.backend->generate();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 40000);
    const QString firstPrompt = entryOf(rig, rig.backend->drafts().at(0).toMap()).metadata["prompt"].toString();
    // The subject and the style change after the batch; "the same prompt again"
    // must still be filed with the subject and style that prompt was made from.
    rig.backend->setSubject("a completely different subject");
    rig.backend->setStyleIndex(0);
    rig.backend->rerun();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 40000);
    const LibraryEntry entry = entryOf(rig, rig.backend->drafts().at(0).toMap());
    QCOMPARE(entry.metadata["prompt"].toString(), firstPrompt);
    QCOMPARE(entry.metadata["subject"].toString(), QString("oil rig at dusk"));
    QCOMPARE(entry.metadata["style"].toString(), QString("cinematic"));
}

void LimgenTests::backend_cancelClearsDraftsAndSaysSo() {
    BackendRig rig;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.create();
    WAIT_FOR_READY(rig);
    rig.backend->cancel(); // idle: nothing happens
    QCOMPARE(rig.backend->status(), QString());
    rig.backend->setSubject("a fox");
    rig.backend->generate();
    QVERIFY(rig.backend->running());
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(rig.sb.fakeLog()).isEmpty(), 15000);
    QCOMPARE(rig.backend->drafts().size(), 4);
    rig.backend->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 15000);
    QVERIFY(rig.backend->drafts().isEmpty());
    QVERIFY(!rig.backend->statusIsError());
    QVERIFY2(rig.backend->status().toLower().contains("cancelled"), qPrintable(rig.backend->status()));
    QVERIFY(rig.backend->canGenerate()); // free again
    QCOMPARE(filesUnder(rig.sb.library()), QStringList());
    QTRY_COMPARE_WITH_TIMEOUT(filesUnder(rig.sb.fixture.cache), QStringList(), 5000);
}

void LimgenTests::backend_estimateTexts() {
    BackendRig rig;
    rig.create();
    WAIT_FOR_READY(rig);
    // Nothing measured yet: it says so, with no figure.
    QVERIFY(!rig.backend->estimate().isEmpty());
    QVERIFY2(!rig.backend->estimate().contains("About"), qPrintable(rig.backend->estimate()));
    rig.backend->setSubject("a fox");
    rig.backend->generate();
    QVERIFY(rig.backend->running());
    // While the first batch runs with no earlier measurement there is no "left" figure to give yet.
    QVERIFY2(!rig.backend->estimate().contains("About"), qPrintable(rig.backend->estimate()));
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 40000);
    QVERIFY2(rig.backend->estimate().startsWith("Made 4 drafts in "), qPrintable(rig.backend->estimate()));
    QVERIFY(QFileInfo::exists(rig.sb.benchFile()));
    // A new window, with that measurement on disk, knows how long a batch takes.
    rig.create();
    WAIT_FOR_READY(rig);
    QTRY_VERIFY_WITH_TIMEOUT(rig.backend->estimate().contains("About"), 5000);
    QVERIFY2(rig.backend->estimate().contains("4 drafts"), qPrintable(rig.backend->estimate()));
    // And during a batch it counts down.
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.backend->setSubject("a fox");
    rig.backend->generate();
    QVERIFY(rig.backend->running());
    QVERIFY2(rig.backend->estimate().contains("About") && rig.backend->estimate().contains("left"),
             qPrintable(rig.backend->estimate()));
    rig.backend->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 15000);
    // After a cancel the estimate for the next batch is back.
    QTRY_VERIFY_WITH_TIMEOUT(rig.backend->estimate().contains("for 4 drafts"), 5000);
}

void LimgenTests::backend_durationText() {
    QLocale::setDefault(QLocale::c());
    QCOMPARE(Backend::durationText(5), QString("5 s"));
    QCOMPARE(Backend::durationText(0.2), QString("1 s")); // never "0 s"
    QCOMPARE(Backend::durationText(0), QString("1 s"));
    QCOMPARE(Backend::durationText(59.4), QString("59 s"));
    QCOMPARE(Backend::durationText(59.6), QString("1 min 0 s")); // rounds up to a minute
    QCOMPARE(Backend::durationText(60), QString("1 min 0 s"));
    QCOMPARE(Backend::durationText(125), QString("2 min 5 s"));
    QCOMPARE(Backend::durationText(3725), QString("62 min 5 s"));
}

void LimgenTests::backend_stopAllStopsABatch() {
    BackendRig rig;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.create();
    WAIT_FOR_READY(rig);
    rig.backend->stopAll(); // idle: harmless
    QVERIFY(!rig.backend->busy());
    rig.backend->setSubject("a fox");
    rig.backend->generate();
    QVERIFY(rig.backend->running());
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(rig.sb.fakeLog()).isEmpty(), 15000);
    rig.backend->stopAll();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 15000);
    QVERIFY(rig.backend->drafts().isEmpty());
    QCOMPARE(filesUnder(rig.sb.library()), QStringList());
}

void LimgenTests::backend_stopAllPausesADownload() {
    DownloadRig rig;
    WAIT_FOR_READY(rig);
    HttpServer::Route &route = rig.route("a");
    route.chunk = 10000;
    route.chunkDelayMs = 100;
    rig.backend->startDownload();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(rig.part("a")).size() > 0, 20000);
    rig.backend->stopAll();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->downloading(), 20000);
    QVERIFY(QFileInfo(rig.part("a")).size() > 0); // paused, not deleted
    QVERIFY(!rig.backend->statusIsError());
    QVERIFY(!QFileInfo::exists(rig.sb.fixture.models + "/.download.lock"));
    QVERIFY(!rig.backend->modelInstalled());
}

// A Backend that is deleted while a batch runs, as when the window is closed
// at the wrong moment. It runs in a child process, since the failure this
// looks for is a crash that would take the whole test run with it.
void LimgenTests::backend_destroyingWhileABatchRunsIsSafe() {
    BackendRig rig;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    QProcess child; // inherits the sandbox environment
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {"--child-destroy-backend"});
    QVERIFY(child.waitForStarted(5000));
    QVERIFY(child.waitForFinished(60000));
    const QString output = QString::fromUtf8(child.readAll());
    // Whatever happened, no engine process may outlive the test.
    const bool leftover = QProcess::execute("pgrep", {"-f", rig.sb.fixture.cache}) == 0;
    QProcess::execute("pkill", {"-f", rig.sb.fixture.cache});
    const bool survived = child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
    QVERIFY2(survived, qPrintable(QString("exit %1 (%2): %3").arg(child.exitCode()).arg(int(child.exitStatus())).arg(output.right(600))));
    QVERIFY(output.contains("destroyed"));
    // No batch folder and no engine process are left behind.
    QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
    QVERIFY2(!leftover, "an engine process was left behind");
    QCOMPARE(filesUnder(rig.sb.library()), QStringList());
}

// ============================== L4: the window ============================

namespace {

QStringList *qmlWarnings = nullptr;
QtMessageHandler previousHandler = nullptr;

// Collects the warnings QML prints about limgen's own files: a binding loop,
// an undefined property, a type that does not load.
void collectQmlWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (qmlWarnings && (type == QtWarningMsg || type == QtCriticalMsg) &&
        (message.contains(".qml") || message.contains("qrc:") || QByteArray(context.category).startsWith("qt.qml") ||
         QByteArray(context.category).startsWith("qml")))
        qmlWarnings->append(message);
    if (previousHandler)
        previousHandler(type, context, message);
}

// QTest has no keyClicks for a QWindow: one key click per character.
void typeText(QWindow *window, const QString &text) {
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

// The real window with the real Backend, Theme and Translator, as main() builds it.
struct QmlRig {
    BackendRig rig;
    QStringList warnings;
    std::unique_ptr<Translator> translator;
    std::unique_ptr<Theme> theme;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;

    // With `small`, the model and the upscaler come from the small test
    // catalogue (64x64 drafts), so batches and keeps are quick.
    explicit QmlRig(bool installed = true, const QString &engineProgram = fakeEngine, bool small = false, int steps = 1)
        : rig(true, installed && !small) {
        qmlWarnings = &warnings;
        previousHandler = qInstallMessageHandler(collectQmlWarnings);
        if (small) {
            const World world = makeWorld(steps);
            const QString catalogueFile = rig.sb.fixture.root.path() + "/catalogue.json";
            writeFile(catalogueFile, world.json);
            qputenv("LIMGEN_CATALOGUE", catalogueFile.toUtf8());
            for (const QString &key : QStringList{"a", "c", "b", "u"})
                makeSparse(Catalogue::path(world.file(key), rig.sb.fixture.models), world.file(key).bytes);
            rig.sb.fixture.addTheme("other", tomlWith("#fafafa", "#111111", "#ff0000"));
        }
        rig.create(engineProgram);
        translator = std::make_unique<Translator>(rig.sb.fixture.paths().language);
        QCoreApplication::installTranslator(translator.get());
        theme = std::make_unique<Theme>(rig.sb.fixture.state, nullptr, QString());
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty("translator", translator.get());
        engine->rootContext()->setContextProperty("theme", theme.get());
        engine->rootContext()->setContextProperty("backend", rig.backend.get());
        engine->load(QUrl("qrc:/Main.qml"));
        if (!engine->rootObjects().isEmpty())
            window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
    }
    ~QmlRig() {
        settleBackend(rig.backend.get());
        // The window goes before the backend it is bound to.
        engine.reset();
        theme.reset();
        QCoreApplication::removeTranslator(translator.get());
        translator.reset();
        qInstallMessageHandler(previousHandler);
        qmlWarnings = nullptr;
    }
    Backend &backend() { return *rig.backend; }
    QVariant prop(const char *name) const { return window->property(name); }

    // Item and popup lookups by what limgen's QML files declare.
    QList<QQuickItem *> items() const {
        QList<QQuickItem *> all;
        QList<QQuickItem *> queue{window->contentItem()};
        while (!queue.isEmpty()) {
            QQuickItem *item = queue.takeFirst();
            all << item;
            queue << item->childItems();
        }
        return all;
    }
    QQuickItem *itemInheriting(const char *base) const {
        for (QQuickItem *item : items())
            if (item->inherits(base))
                return item;
        return nullptr;
    }
    QQuickItem *itemNamed(const char *prefix) const {
        for (QQuickItem *item : items())
            if (QString(item->metaObject()->className()).startsWith(prefix))
                return item;
        return nullptr;
    }
    QObject *popup(const std::function<bool(QObject *)> &match) const {
        for (QObject *object : window->findChildren<QObject *>())
            if (object->inherits("QQuickPopup") && match(object))
                return object;
        return nullptr;
    }
    QObject *modelPicker() const {
        return popup([](QObject *o) { return QString(o->metaObject()->className()).startsWith("ModelPicker"); });
    }
    QObject *quitDialog() const {
        return popup([](QObject *o) { return o->property("title").toString() == "Quit limgen?"; });
    }
    bool isOpen(QObject *popup) const { return popup && popup->property("opened").toBool(); }
    QQuickItem *subjectField() const { return itemInheriting("QQuickTextField"); }
    void focusSubject() { QMetaObject::invokeMethod(subjectField(), "forceActiveFocus"); }
    bool typing() const { return prop("typing").toBool(); }
};

#define LOAD_WINDOW(q)                                                  \
    QVERIFY2((q).window, "Main.qml did not load");                      \
    QVERIFY((q).window->isVisible());                                   \
    (q).window->requestActivate();                                      \
    QVERIFY(QTest::qWaitForWindowActive((q).window))

#define NO_QML_WARNINGS(q) QVERIFY2((q).warnings.isEmpty(), qPrintable((q).warnings.join('\n')))

} // namespace

void LimgenTests::qml_loadsWithoutWarnings_data() {
    QTest::addColumn<bool>("installed");
    QTest::newRow("model installed (the grid)") << true;
    QTest::newRow("model missing (the setup panel)") << false;
}

void LimgenTests::qml_loadsWithoutWarnings() {
    QFETCH(bool, installed);
    QmlRig q(installed);
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    QTest::qWait(300); // let the bindings settle
    NO_QML_WARNINGS(q);
    // The palette is shown and the theme's accent drives the style.
    QCOMPARE(q.backend().themeName(), QString("night-city"));
    QCOMPARE(q.window->title(), QString("Limgen"));
}

void LimgenTests::qml_setupPanelOrGrid_data() {
    QTest::addColumn<bool>("installed");
    QTest::addColumn<bool>("goodEngine");
    QTest::addColumn<bool>("setup");
    QTest::newRow("model installed") << true << true << false;
    QTest::newRow("model missing") << false << true << true;
    QTest::newRow("engine cannot run") << true << false << true;
}

void LimgenTests::qml_setupPanelOrGrid() {
    QFETCH(bool, installed);
    QFETCH(bool, goodEngine);
    QFETCH(bool, setup);
    QmlRig q(installed, goodEngine ? fakeEngine : QString("/nonexistent/sd-cli"));
    LOAD_WINDOW(q);
    WAIT_FOR_ENGINE(q.rig);
    QTRY_COMPARE_WITH_TIMEOUT(q.prop("setupNeeded").toBool(), setup, 5000);
    QQuickItem *panel = q.itemNamed("SetupPanel"), *grid = q.itemNamed("DraftGrid");
    QVERIFY(panel && grid);
    QCOMPARE(panel->isVisible(), setup);
    QCOMPARE(grid->isVisible(), !setup);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_typingKeysDoNotActAsShortcuts() {
    QmlRig q;
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    QVERIFY(q.subjectField());
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    // Every single-letter shortcut, and Space, with a subject that could be
    // generated: they type text and do nothing else.
    typeText(q.window, "oil rig q r f m p ?");
    QCOMPARE(q.backend().subject(), QString("oil rig q r f m p ?"));
    QVERIFY(q.typing());
    QCOMPARE(q.prop("helpVisible").toBool(), false);
    QCOMPARE(q.prop("promptVisible").toBool(), false);
    QCOMPARE(q.prop("fullView").toBool(), false);
    QCOMPARE(q.prop("dialogOpen").toBool(), false);
    QVERIFY(!q.isOpen(q.modelPicker()));
    QVERIFY(!q.isOpen(q.quitDialog()));
    QVERIFY(!q.backend().running());
    QVERIFY(q.backend().drafts().isEmpty());
    QCOMPARE(imageCalls(q.rig.sb.fakeLog()).size(), 0);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_escLeavesTheFieldThenShortcutsAct() {
    QmlRig q;
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    typeText(q.window, "oil rig");
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.typing());
    QCOMPARE(q.backend().subject(), QString("oil rig")); // the text stays
    QVERIFY(!q.backend().running()); // Esc only left the field
    // Now the letters are shortcuts: P shows the full prompt, ? the help.
    QTest::keyClick(q.window, 'p');
    QTRY_COMPARE(q.prop("promptVisible").toBool(), true);
    QTest::keyClick(q.window, 'p');
    QTRY_COMPARE(q.prop("promptVisible").toBool(), false);
    QTest::keyClick(q.window, '?');
    QTRY_COMPARE(q.prop("helpVisible").toBool(), true);
    // Space does not generate under the help overlay; Esc closes the overlay first.
    QTest::keyClick(q.window, Qt::Key_Space);
    QVERIFY(!q.backend().running());
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE(q.prop("helpVisible").toBool(), false);
    // And Space generates.
    QTest::keyClick(q.window, Qt::Key_Space);
    QTRY_VERIFY(q.backend().running());
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 40000);
    QCOMPARE(q.backend().drafts().at(0).toMap()["state"].toString(), QString("done"));
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_enterInTheSubjectGeneratesAndLeavesTheField() {
    QmlRig q;
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    // Enter with nothing typed generates nothing, but still leaves the field.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    QTest::keyClick(q.window, Qt::Key_Return);
    QTRY_VERIFY(!q.typing());
    QVERIFY(!q.backend().running());
    // With a subject, Enter starts the batch and leaves the field.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    typeText(q.window, "oil rig at dusk");
    QTest::keyClick(q.window, Qt::Key_Return);
    QTRY_VERIFY(q.backend().running());
    QVERIFY(!q.typing());
    QVERIFY(!qobject_cast<QQuickItem *>(q.window->activeFocusItem())->inherits("QQuickTextField"));
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 40000);
    QCOMPARE(q.backend().drafts().size(), 4);
    QCOMPARE(q.backend().drafts().at(3).toMap()["state"].toString(), QString("done"));
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_escCancelsARunningBatch() {
    QmlRig q;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    typeText(q.window, "oil rig");
    QTest::keyClick(q.window, Qt::Key_Return);
    QTRY_VERIFY(q.backend().running());
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(q.rig.sb.fakeLog()).isEmpty(), 15000);
    // Back in the field: the first Esc leaves it and the batch keeps running.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.typing());
    QVERIFY(q.backend().running());
    // The second Esc cancels it.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 15000);
    QVERIFY(q.backend().drafts().isEmpty());
    QVERIFY(q.backend().status().toLower().contains("cancelled"));
    // Esc with nothing to close or cancel does nothing.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTest::qWait(100);
    QVERIFY(!q.backend().running());
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_arrowsMoveTheSelectionInTheGrid() {
    QmlRig q;
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    q.backend().setSubject("oil rig");
    q.backend().generate();
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 40000);
    QCOMPARE(q.backend().drafts().size(), 4);
    auto press = [&](Qt::Key key) { QTest::keyClick(q.window, key); };
    auto selected = [&] { return q.prop("selected").toInt(); };
    QCOMPARE(selected(), 0);
    press(Qt::Key_Right);
    QTRY_COMPARE(selected(), 1);
    press(Qt::Key_Right); // the right edge
    QTest::qWait(50);
    QCOMPARE(selected(), 1);
    press(Qt::Key_Down);
    QTRY_COMPARE(selected(), 3);
    press(Qt::Key_Down); // the bottom edge
    QTest::qWait(50);
    QCOMPARE(selected(), 3);
    press(Qt::Key_Left);
    QTRY_COMPARE(selected(), 2);
    press(Qt::Key_Left); // the left edge
    QTest::qWait(50);
    QCOMPARE(selected(), 2);
    press(Qt::Key_Up);
    QTRY_COMPARE(selected(), 0);
    press(Qt::Key_Up); // the top edge
    QTest::qWait(50);
    QCOMPARE(selected(), 0);
    press(Qt::Key_Down);
    QTRY_COMPARE(selected(), 2);
    press(Qt::Key_Right);
    QTRY_COMPARE(selected(), 3);
    // In the subject field the arrows move the cursor, not the selection.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    press(Qt::Key_Left);
    press(Qt::Key_Up);
    QTest::qWait(50);
    QCOMPARE(selected(), 3);
    // A new batch starts with the first draft selected.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.typing());
    q.backend().generate();
    QTRY_VERIFY(q.backend().running());
    QTRY_COMPARE(selected(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 40000);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_fullViewOnlyForADoneDraft() {
    QmlRig q;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    // Nothing to show yet: F does nothing.
    QTest::keyClick(q.window, 'f');
    QTest::qWait(50);
    QCOMPARE(q.prop("fullView").toBool(), false);
    // A draft that is still waiting cannot be shown full size either.
    q.backend().setSubject("oil rig");
    q.backend().generate();
    QTRY_VERIFY(q.backend().running());
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(q.rig.sb.fakeLog()).isEmpty(), 15000);
    QTest::keyClick(q.window, 'f');
    QTest::qWait(100);
    QCOMPARE(q.prop("fullView").toBool(), false);
    q.backend().cancel();
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 15000);
    // A finished batch: F opens the selected draft, Esc and F close it.
    qunsetenv("FAKE_SD_CLI_MODE");
    q.backend().generate();
    QTRY_VERIFY(q.backend().running());
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 40000);
    QQuickItem *full = q.itemNamed("FullView");
    QVERIFY(full);
    QVERIFY(!full->isVisible());
    QTest::keyClick(q.window, 'f');
    QTRY_COMPARE(q.prop("fullView").toBool(), true);
    QTRY_VERIFY(full->isVisible());
    // The arrows do not move the selection under the full view.
    QTest::keyClick(q.window, Qt::Key_Right);
    QTest::qWait(50);
    QCOMPARE(q.prop("selected").toInt(), 0);
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE(q.prop("fullView").toBool(), false);
    QTRY_VERIFY(!full->isVisible());
    QTest::keyClick(q.window, 'f');
    QTRY_COMPARE(q.prop("fullView").toBool(), true);
    QTest::keyClick(q.window, 'f'); // F again closes it
    QTRY_COMPARE(q.prop("fullView").toBool(), false);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_helpToggles() {
    QmlRig q;
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    QQuickItem *help = q.itemNamed("HelpOverlay");
    QVERIFY(help);
    QVERIFY(!help->isVisible());
    QTest::keyClick(q.window, '?');
    QTRY_COMPARE(q.prop("helpVisible").toBool(), true);
    QTRY_VERIFY(help->isVisible());
    QTest::keyClick(q.window, '?');
    QTRY_COMPARE(q.prop("helpVisible").toBool(), false);
    QTRY_VERIFY(!help->isVisible());
    QTest::keyClick(q.window, '?');
    QTRY_COMPARE(q.prop("helpVisible").toBool(), true);
    // Under the help, the other shortcuts do nothing.
    QTest::keyClick(q.window, 'p');
    QTest::keyClick(q.window, 'm');
    QTest::keyClick(q.window, Qt::Key_Down);
    QTest::qWait(100);
    QCOMPARE(q.prop("promptVisible").toBool(), false);
    QVERIFY(!q.isOpen(q.modelPicker()));
    QCOMPARE(q.prop("helpVisible").toBool(), true);
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE(q.prop("helpVisible").toBool(), false);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_selectionStaysWhileTypingInABatch() {
    QmlRig q;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    q.backend().setSubject("oil rig");
    q.backend().generate();
    QTRY_VERIFY(q.backend().running());
    QTest::keyClick(q.window, Qt::Key_Down);
    QTRY_COMPARE(q.prop("selected").toInt(), 2);
    // Typing changes the backend's state; the selection stays where it was.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    typeText(q.window, " at dusk");
    QCOMPARE(q.backend().subject(), QString("oil rig at dusk"));
    QTest::qWait(100);
    QCOMPARE(q.prop("selected").toInt(), 2);
    QVERIFY(q.backend().running());
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_setupPanelLetsKeysWorkAfterTheField() {
    QmlRig q(false); // the model is missing: the setup panel, no grid
    LOAD_WINDOW(q);
    WAIT_FOR_ENGINE(q.rig);
    QTRY_COMPARE_WITH_TIMEOUT(q.prop("setupNeeded").toBool(), true, 5000);
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.typing());
    QTest::keyClick(q.window, '?');
    QTRY_COMPARE(q.prop("helpVisible").toBool(), true);
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE(q.prop("helpVisible").toBool(), false);
    // Enter in the field leaves it too, though nothing can be generated.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    typeText(q.window, "oil rig");
    QTest::keyClick(q.window, Qt::Key_Return);
    QTRY_VERIFY(!q.typing());
    QVERIFY(!q.backend().running());
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_modelPickerOpensAndCloses() {
    QmlRig q;
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    QObject *picker = q.modelPicker();
    QVERIFY(picker);
    QVERIFY(!q.isOpen(picker));
    QTest::keyClick(q.window, 'm');
    QTRY_VERIFY(q.isOpen(picker));
    QCOMPARE(q.prop("dialogOpen").toBool(), true);
    // Single-letter shortcuts are off while it is open: Q does not ask to quit, ? shows no help.
    QTest::keyClick(q.window, 'q');
    QTest::keyClick(q.window, '?');
    QTest::qWait(100);
    QVERIFY(!q.isOpen(q.quitDialog()));
    QCOMPARE(q.prop("helpVisible").toBool(), false);
    QVERIFY(q.isOpen(picker));
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.isOpen(picker));
    QCOMPARE(q.prop("dialogOpen").toBool(), false);
    // Down and Enter pick the second model.
    QTest::keyClick(q.window, 'm');
    QTRY_VERIFY(q.isOpen(picker));
    QTest::keyClick(q.window, Qt::Key_Down);
    QTest::keyClick(q.window, Qt::Key_Return);
    QTRY_VERIFY(!q.isOpen(picker));
    QCOMPARE(q.backend().modelId(), QString("flux2-klein-4b"));
    QCOMPARE(QJsonDocument::fromJson(readFile(q.rig.sb.configFile())).object()["model"].toString(),
             QString("flux2-klein-4b"));
    // klein is not installed: the window shows the setup panel for it.
    QTRY_COMPARE_WITH_TIMEOUT(q.prop("setupNeeded").toBool(), true, 5000);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_quitWithARunningBatchAsksFirst() {
    QmlRig q;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    QSignalSpy aboutToQuit(QCoreApplication::instance(), &QCoreApplication::aboutToQuit);
    q.backend().setSubject("oil rig");
    q.backend().generate();
    QTRY_VERIFY(q.backend().running());
    QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(q.rig.sb.fakeLog()).isEmpty(), 15000);
    QObject *dialog = q.quitDialog();
    QVERIFY(dialog);
    QVERIFY(!q.isOpen(dialog));
    // While the model picker cannot open and Q asks.
    QTest::keyClick(q.window, 'm');
    QTest::qWait(100);
    QVERIFY(!q.isOpen(q.modelPicker())); // M is off while a batch runs
    QTest::keyClick(q.window, 'q');
    QTRY_VERIFY(q.isOpen(dialog));
    QVERIFY(dialog->property("visible").toBool());
    QCOMPARE(q.prop("dialogOpen").toBool(), true);
    QCOMPARE(aboutToQuit.count(), 0);
    QVERIFY(q.backend().running()); // nothing was stopped
    // Esc is "keep working": the dialog closes and the batch carries on.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.isOpen(dialog));
    QVERIFY(q.backend().running());
    QCOMPARE(aboutToQuit.count(), 0);
    QCOMPARE(q.prop("quitting").toBool(), false);
    // Q while typing is a letter, not a quit.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    QTest::keyClick(q.window, 'q');
    QTest::qWait(100);
    QVERIFY(!q.isOpen(dialog));
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.typing());
    q.backend().cancel();
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 15000);
    NO_QML_WARNINGS(q);
}

// ============================== L5: upscale parser and engine =============

namespace {

QImage solidImage(const QSize &size, const QColor &colour, QImage::Format format = QImage::Format_RGB32) {
    QImage image(size, format);
    image.fill(colour);
    return image;
}

bool savePng(const QImage &image, const QString &path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    return image.save(path, "PNG");
}

QRgb labRgb(double l, double a, double b) {
    const double lab[3] = {l, a, b};
    return ColorPass::fromLab(lab);
}

// An image of two flat halves, top and bottom, for statistics with known spread.
QImage twoTone(const QRgb top, const QRgb bottom, const QSize &size = QSize(40, 40)) {
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixel(x, y, y < size.height() / 2 ? top : bottom);
    return image;
}

LabStats statsOf(const QImage &image) { return ColorPass::statistics({image}); }

// An upscale request on a PNG of the given size, with the model file in place.
EngineRequest upscaleRequest(const QString &directory, const QSize &input = QSize(300, 200)) {
    EngineRequest request;
    request.mode = EngineRequest::Mode::Upscale;
    savePng(solidImage(input, QColor(10, 20, 30)), directory + "/in.png");
    writeFile(directory + "/models/esrgan.pth", "x");
    request.files.insert("upscale-model", directory + "/models/esrgan.pth");
    request.input = directory + "/in.png";
    request.width = input.width() * 4;
    request.height = input.height() * 4;
    request.upscaleTileSize = 256;
    request.outputDirectory = directory + "/out";
    return request;
}

} // namespace

void LimgenTests::progress_upscaleLogCountsTiles() {
    const QByteArray log = dataFile("sd-cli-esrgan-upscale.log");
    QVERIFY(!log.isEmpty());
    const QList<ProgressEvent> events = parseInChunks(log, 1, 0, log.size());
    const auto steps = ofKind(events, ProgressEvent::Step);
    // 112 tiles, counted like sampling steps, all of the one image.
    QCOMPARE(steps.size(), 112);
    for (int k = 0; k < steps.size(); ++k) {
        QCOMPARE(steps[k].image, 0);
        QCOMPARE(steps[k].step, k + 1);
        QCOMPARE(steps[k].steps, 112);
        QVERIFY(steps[k].secondsPerStep > 0);
    }
    // The first line is in s/it, the later ones in it/s, both given as seconds.
    QCOMPARE(steps[0].secondsPerStep, 2.08);
    QVERIFY2(qAbs(steps[1].secondsPerStep - 1.0 / 4.57) < 1e-9, qPrintable(QString::number(steps[1].secondsPerStep)));
    QCOMPARE(ofKind(events, ProgressEvent::Sampled).size(), 0);
    QCOMPARE(ofKind(events, ProgressEvent::Saved).size(), 1);
    QVERIFY(ofKind(events, ProgressEvent::Saved).first().text.endsWith("x4.png"));
    QVERIFY(ofKind(events, ProgressEvent::Error).isEmpty());
}

void LimgenTests::progress_upscaleLogChunkSizeDoesNotMatter() {
    const QByteArray log = dataFile("sd-cli-esrgan-upscale.log");
    QStringList whole;
    for (const ProgressEvent &e : parseInChunks(log, 1, 0, log.size()))
        whole << eventText(e);
    for (const qsizetype chunk : {qsizetype(1), qsizetype(13), qsizetype(500)}) {
        QStringList parts;
        for (const ProgressEvent &e : parseInChunks(log, 1, 0, chunk))
            parts << eventText(e);
        QVERIFY2(parts == whole, qPrintable(QString("chunk size %1").arg(chunk)));
    }
}

void LimgenTests::progress_anyTotalOnlyWhenStepsIsZero() {
    ProgressParser tiles(1, 0);
    QCOMPARE(tiles.feed("  |==| 3/12 - 1.00s/it\n  |==| 7/40 - 2.00s/it\n").size(), 2);
    // With a step count, a line with another total is still not a step.
    ProgressParser steps(1, 8);
    QVERIFY(steps.feed("  |==| 3/12 - 1.00s/it\n").isEmpty());
    QCOMPARE(steps.feed("  |==| 3/8 - 1.00s/it\n").size(), 1);
    // Out of range counts are rejected either way.
    QVERIFY(tiles.feed("  |==| 13/12 - 1.00s/it\n  |==| 0/12 - 1.00s/it\n").isEmpty());
}

void LimgenTests::engine_argumentsUpscale() {
    EngineRequest request;
    request.mode = EngineRequest::Mode::Upscale;
    request.files.insert("upscale-model", "/m/esrgan.pth");
    request.input = "/d/draft.png";
    request.outputDirectory = "/cache/keep-1";
    request.upscaleTileSize = 256;
    request.prompt = "ignored"; // a batch's fields play no part
    request.count = 4;
    request.previews = true;
    const QStringList arguments = SdCppEngine::arguments(request);
    QCOMPARE(arguments, (QStringList{"-M", "upscale", "--upscale-model", "/m/esrgan.pth", "-i", "/d/draft.png", "-o",
                                     "/cache/keep-1/upscaled.png", "--upscale-tile-size", "256"}));
    QCOMPARE(SdCppEngine::draftPaths(request), QStringList{"/cache/keep-1/upscaled.png"});
    // Every option it uses is in the pinned engine's --help.
    const QString help = QString::fromUtf8(dataFile("sd-cli-help.txt"));
    for (const QString &option : QStringList{"-M", "--upscale-model", "-i", "-o", "--upscale-tile-size"}) {
        const QRegularExpression word("(^|[\\s,])" + QRegularExpression::escape(option) + "([\\s,]|$)",
                                      QRegularExpression::MultilineOption);
        QVERIFY2(word.match(help).hasMatch(), qPrintable(option + " is not an option of the pinned sd-cli"));
    }
    QVERIFY(help.contains("upscale")); // the mode itself is listed
    request.upscaleTileSize = 128;
    QCOMPARE(valueAfter(SdCppEngine::arguments(request), "--upscale-tile-size"), QString("128"));
}

void LimgenTests::engine_upscaleRunMakesFourTimes() {
    Sandbox sb;
    const EngineRequest request = upscaleRequest(sb.fixture.root.path());
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY2(run.finished, qPrintable(run.error.message + run.error.detail));
    QCOMPARE(run.drafts, QStringList{request.outputDirectory + "/upscaled.png"});
    QCOMPARE(QImageReader(run.drafts[0]).size(), QSize(1200, 800));
    QVERIFY(!engine.retried());
    // 300x200 at tile 256 is 6 tiles in the fake; they arrive as steps of image 0.
    QCOMPARE(run.progress.size(), 6);
    for (int k = 0; k < 6; ++k) {
        QCOMPARE(run.progress[k].image, 0);
        QCOMPARE(run.progress[k].step, k + 1);
        QCOMPARE(run.progress[k].steps, 6);
    }
    QCOMPARE(imageCalls(sb.fakeLog()).size(), 0); // not a batch call
    QCOMPARE(fakeCalls(sb.fakeLog()).size(), 1);
    QCOMPARE(fakeCalls(sb.fakeLog())[0], SdCppEngine::arguments(request));
}

void LimgenTests::engine_upscaleOutOfMemoryRetriesWithSmallerTiles() {
    Sandbox sb;
    qputenv("FAKE_SD_CLI_MODE", "oom-once"); // fails, with exit 0, above tile size 128
    const EngineRequest request = upscaleRequest(sb.fixture.root.path());
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY2(run.finished, qPrintable(run.error.message + run.error.detail));
    QVERIFY(engine.retried());
    const QList<QStringList> calls = fakeCalls(sb.fakeLog());
    QCOMPARE(calls.size(), 2);
    QCOMPARE(valueAfter(calls[0], "--upscale-tile-size"), QString("256"));
    QCOMPARE(valueAfter(calls[1], "--upscale-tile-size"), QString("128"));
    QCOMPARE(QImageReader(run.drafts[0]).size(), QSize(1200, 800));
}

void LimgenTests::engine_upscaleOutOfMemoryFailsAfterOneRetry() {
    Sandbox sb;
    qputenv("FAKE_SD_CLI_MODE", "oom"); // fails at every tile size, saving the input unscaled
    const EngineRequest request = upscaleRequest(sb.fixture.root.path());
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    // The engine exits 0 and writes a file, but it is a failure, not an upscale.
    QVERIFY(run.failed);
    QVERIFY(!run.finished);
    QCOMPARE(int(run.error.kind), int(EngineError::OutOfMemory));
    QVERIFY(engine.retried());
    QCOMPARE(fakeCalls(sb.fakeLog()).size(), 2); // one retry, not more
}

void LimgenTests::engine_upscaleSmallTilesAreNotRetried() {
    Sandbox sb;
    qputenv("FAKE_SD_CLI_MODE", "oom");
    EngineRequest request = upscaleRequest(sb.fixture.root.path());
    request.upscaleTileSize = 128;
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QCOMPARE(int(run.error.kind), int(EngineError::OutOfMemory));
    QVERIFY(!engine.retried());
    QCOMPARE(fakeCalls(sb.fakeLog()).size(), 1);
}

void LimgenTests::engine_upscaleSizeCheckAndFailures_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("fragment");
    QTest::newRow("wrong size") << "wrong-size" << "upscaled image";
    QTest::newRow("engine fails") << "fail" << "fake engine failure";
}

void LimgenTests::engine_upscaleSizeCheckAndFailures() {
    QFETCH(QString, mode);
    QFETCH(QString, fragment);
    Sandbox sb;
    qputenv("FAKE_SD_CLI_MODE", mode.toUtf8());
    const EngineRequest request = upscaleRequest(sb.fixture.root.path());
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY(run.failed);
    QVERIFY(!run.finished);
    QCOMPARE(int(run.error.kind), int(EngineError::Failed));
    QVERIFY2((run.error.message + run.error.detail).contains(fragment), qPrintable(run.error.message + run.error.detail));
}

void LimgenTests::engine_upscaleCancelRemovesTheOutput() {
    Sandbox sb;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    const EngineRequest request = upscaleRequest(sb.fixture.root.path());
    QVERIFY(writeFile(request.outputDirectory + "/upscaled.png", "partial"));
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    watch(engine, run);
    engine.start(request);
    QTRY_VERIFY_WITH_TIMEOUT(!fakeCalls(sb.fakeLog()).isEmpty(), 10000);
    QTest::qWait(300);
    engine.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(run.done(), 10000);
    QCOMPARE(int(run.error.kind), int(EngineError::Cancelled));
    QVERIFY(!QFileInfo::exists(request.outputDirectory + "/upscaled.png"));
}

// ============================== L5: ColorPass =============================

void LimgenTests::colorpass_toLabKnownValues_data() {
    QTest::addColumn<QRgb>("rgb");
    QTest::addColumn<double>("l");
    QTest::addColumn<double>("a");
    QTest::addColumn<double>("b");
    QTest::newRow("white") << qRgb(255, 255, 255) << 100.0 << 0.0 << 0.0;
    QTest::newRow("black") << qRgb(0, 0, 0) << 0.0 << 0.0 << 0.0;
    QTest::newRow("red") << qRgb(255, 0, 0) << 53.24 << 80.09 << 67.20;
    QTest::newRow("green") << qRgb(0, 255, 0) << 87.73 << -86.18 << 83.18;
    QTest::newRow("blue") << qRgb(0, 0, 255) << 32.30 << 79.19 << -107.86;
    QTest::newRow("mid grey") << qRgb(119, 119, 119) << 50.0 << 0.0 << 0.0;
}

void LimgenTests::colorpass_toLabKnownValues() {
    QFETCH(QRgb, rgb);
    QFETCH(double, l);
    QFETCH(double, a);
    QFETCH(double, b);
    double lab[3];
    ColorPass::toLab(rgb, lab);
    const QString got = QString("%1 %2 %3").arg(lab[0]).arg(lab[1]).arg(lab[2]);
    QVERIFY2(qAbs(lab[0] - l) < 0.2 && qAbs(lab[1] - a) < 0.2 && qAbs(lab[2] - b) < 0.2, qPrintable(got));
}

void LimgenTests::colorpass_roundTrip() {
    for (int r = 0; r < 256; r += 51)
        for (int g = 0; g < 256; g += 51)
            for (int b = 0; b < 256; b += 51) {
                double lab[3];
                ColorPass::toLab(qRgb(r, g, b), lab);
                const QRgb back = ColorPass::fromLab(lab);
                QVERIFY2(qAbs(qRed(back) - r) <= 1 && qAbs(qGreen(back) - g) <= 1 && qAbs(qBlue(back) - b) <= 1,
                         qPrintable(QString("%1 %2 %3 -> %4 %5 %6").arg(r).arg(g).arg(b).arg(qRed(back)).arg(qGreen(back)).arg(qBlue(back))));
            }
}

void LimgenTests::colorpass_fromLabClipsOutOfGamut() {
    // Colours no screen can show are clipped to the nearest channel values, not wrapped.
    const QRgb vivid = labRgb(100, 100, 100);
    QVERIFY(qRed(vivid) >= 0 && qRed(vivid) <= 255 && qGreen(vivid) >= 0 && qBlue(vivid) <= 255);
    QCOMPARE(qRed(vivid), 255);
    QCOMPARE(labRgb(-20, 0, 0), qRgb(0, 0, 0));
    QCOMPARE(qRed(labRgb(0, 120, -120)) >= 0, true);
    QCOMPARE(qAlpha(labRgb(50, 10, 10)), 255);
}

void LimgenTests::colorpass_statisticsOfImages() {
    const LabStats flat = statsOf(solidImage(QSize(8, 8), QColor(200, 100, 50)));
    QVERIFY(flat.valid);
    double lab[3];
    ColorPass::toLab(qRgb(200, 100, 50), lab);
    for (int c = 0; c < 3; ++c) {
        QVERIFY(qAbs(flat.mean[c] - lab[c]) < 1e-6);
        QVERIFY(flat.deviation[c] < 1e-3);
    }
    QVERIFY(flat.lightness && flat.spread);
    // Half white, half black: mean L 50, deviation L 50.
    const LabStats two = statsOf(twoTone(qRgb(255, 255, 255), qRgb(0, 0, 0)));
    QVERIFY(qAbs(two.mean[0] - 50) < 0.1);
    QVERIFY(qAbs(two.deviation[0] - 50) < 0.1);
    QVERIFY(two.deviation[1] < 0.01);
    // Several images count together.
    const LabStats both = ColorPass::statistics({solidImage(QSize(4, 4), Qt::white), solidImage(QSize(4, 4), Qt::black)});
    QVERIFY(qAbs(both.mean[0] - 50) < 0.1);
    // A large image is sampled, and a flat one gives the same figures.
    const LabStats large = statsOf(solidImage(QSize(1500, 900), QColor(200, 100, 50)));
    QVERIFY(qAbs(large.mean[0] - lab[0]) < 1e-6);
    // Nothing to measure.
    QVERIFY(!ColorPass::statistics(QList<QImage>{}).valid);
    QVERIFY(!ColorPass::statistics(QList<QImage>{QImage()}).valid);
}

void LimgenTests::colorpass_statisticsOfPalette() {
    const QList<QColor> palette{QColor("#ff0000"), QColor("#00ff00"), QColor(), QColor("#0000ff")};
    const LabStats stats = ColorPass::statistics(palette);
    QVERIFY(stats.valid);
    // The palette moves the colour cast only.
    QVERIFY(!stats.lightness);
    QVERIFY(!stats.spread);
    double red[3], green[3], blue[3];
    ColorPass::toLab(qRgb(255, 0, 0), red);
    ColorPass::toLab(qRgb(0, 255, 0), green);
    ColorPass::toLab(qRgb(0, 0, 255), blue);
    for (int c = 0; c < 3; ++c)
        QVERIFY2(qAbs(stats.mean[c] - (red[c] + green[c] + blue[c]) / 3) < 1e-6, "the invalid colour is skipped"); 
    QVERIFY(!ColorPass::statistics(QList<QColor>{}).valid);
    QVERIFY(!ColorPass::statistics(QList<QColor>{QColor()}).valid);
}

void LimgenTests::colorpass_strengthZeroIsIdentity() {
    const QImage image = twoTone(qRgb(30, 80, 160), qRgb(200, 150, 90));
    const LabStats target = statsOf(solidImage(QSize(8, 8), QColor(230, 120, 40)));
    for (const double strength : {0.0, -1.0}) {
        const QImage result = ColorPass::apply(image, target, strength);
        QCOMPARE(result, image);
    }
    // No target, no image: nothing to do either.
    QCOMPARE(ColorPass::apply(image, LabStats(), 1.0), image);
    QVERIFY(ColorPass::apply(QImage(), target, 1.0).isNull());
}

void LimgenTests::colorpass_strengthOneReachesTheTargetMeans() {
    // A bluish picture with some spread, moved toward an orange one.
    QImage source(60, 40, QImage::Format_RGB32);
    QImage target(60, 40, QImage::Format_RGB32);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 60; ++x) {
            source.setPixel(x, y, labRgb(45 + y * 0.5, -5 + x * 0.1, -25 + y * 0.2));
            target.setPixel(x, y, labRgb(60 + x * 0.3, 20 + y * 0.2, 35 + x * 0.1));
        }
    const LabStats want = statsOf(target);
    const LabStats before = statsOf(source);
    const QImage result = ColorPass::apply(source, want, 1.0);
    QCOMPARE(result.size(), source.size());
    const LabStats got = statsOf(result);
    for (int c = 0; c < 3; ++c) {
        QVERIFY2(qAbs(got.mean[c] - want.mean[c]) < 1.5,
                 qPrintable(QString("channel %1: %2 vs %3 (was %4)").arg(c).arg(got.mean[c]).arg(want.mean[c]).arg(before.mean[c])));
        // The means really moved: the source was far from the target.
        QVERIFY(qAbs(before.mean[c] - want.mean[c]) > 5);
    }
}

void LimgenTests::colorpass_spreadFollowsTheTargetWithinTheClamp() {
    const QRgb grey40 = labRgb(40, 0, 0), grey60 = labRgb(60, 0, 0);
    const QImage source = twoTone(grey40, grey60); // spread 10 in L
    // A target 1.5 times as spread out: the result follows.
    LabStats target;
    target.mean[0] = 50;
    target.deviation[0] = 15;
    target.valid = true;
    LabStats got = statsOf(ColorPass::apply(source, target, 1.0));
    QVERIFY2(qAbs(got.deviation[0] - 15) < 1.5, qPrintable(QString::number(got.deviation[0])));
    // A target 5 times as spread out: at most twice.
    target.deviation[0] = 50;
    got = statsOf(ColorPass::apply(source, target, 1.0));
    QVERIFY2(qAbs(got.deviation[0] - 20) < 2.0, qPrintable(QString::number(got.deviation[0])));
    // A nearly flat target: at least half.
    target.deviation[0] = 0.5;
    got = statsOf(ColorPass::apply(source, target, 1.0));
    QVERIFY2(qAbs(got.deviation[0] - 5) < 1.0, qPrintable(QString::number(got.deviation[0])));
    // A flat image is not blown up, whatever the target says.
    target.deviation[0] = 50;
    got = statsOf(ColorPass::apply(solidImage(QSize(20, 20), QColor(120, 120, 120)), target, 1.0));
    QVERIFY2(got.deviation[0] < 1.0, qPrintable(QString::number(got.deviation[0])));
}

void LimgenTests::colorpass_paletteTargetMovesOnlyTheCast() {
    const QImage source = twoTone(labRgb(35, -3, -12), labRgb(65, -3, -12));
    const QList<QColor> warm{QColor("#e07a30"), QColor("#c05020"), QColor("#f0c070")};
    const LabStats target = ColorPass::statistics(warm);
    const LabStats before = statsOf(source);
    const LabStats after = statsOf(ColorPass::apply(source, target, 1.0));
    // Lightness and its spread stay; the colour cast takes the palette's.
    QVERIFY2(qAbs(after.mean[0] - before.mean[0]) < 1.0, qPrintable(QString::number(after.mean[0])));
    QVERIFY2(qAbs(after.deviation[0] - before.deviation[0]) < 1.0, "the spread of L is untouched");
    QVERIFY2(qAbs(after.mean[1] - target.mean[1]) < 2.5, qPrintable(QString("a %1 vs %2").arg(after.mean[1]).arg(target.mean[1])));
    QVERIFY2(qAbs(after.mean[2] - target.mean[2]) < 2.5, qPrintable(QString("b %1 vs %2").arg(after.mean[2]).arg(target.mean[2])));
}

void LimgenTests::colorpass_halfStrengthIsHalfway() {
    const QImage source = solidImage(QSize(20, 20), QColor(80, 110, 160));
    const LabStats target = statsOf(solidImage(QSize(8, 8), QColor(220, 140, 60)));
    const LabStats from = statsOf(source), full = statsOf(ColorPass::apply(source, target, 1.0)),
                   half = statsOf(ColorPass::apply(source, target, 0.5));
    for (int c = 0; c < 3; ++c)
        QVERIFY2(qAbs(half.mean[c] - (from.mean[c] + full.mean[c]) / 2) < 1.0, qPrintable(QString::number(c)));
    // A strength above 1 is 1.
    const LabStats over = statsOf(ColorPass::apply(source, target, 7.0));
    for (int c = 0; c < 3; ++c)
        QVERIFY(qAbs(over.mean[c] - full.mean[c]) < 0.5);
}

void LimgenTests::colorpass_alphaIsKept() {
    QImage image(10, 10, QImage::Format_ARGB32);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            image.setPixel(x, y, qRgba(80, 110, 160, x == 0 ? 0 : (y < 5 ? 255 : 128)));
    const LabStats target = statsOf(solidImage(QSize(8, 8), QColor(220, 140, 60)));
    const QImage result = ColorPass::apply(image, target, 1.0);
    QVERIFY(result.hasAlphaChannel());
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            QCOMPARE(qAlpha(result.pixel(x, y)), qAlpha(image.pixel(x, y)));
    QVERIFY(result.pixel(5, 2) != image.pixel(5, 2)); // the colours did move
    // An opaque image comes back without an alpha channel.
    const QImage opaque = ColorPass::apply(solidImage(QSize(4, 4), QColor(80, 110, 160)), target, 1.0);
    QVERIFY(!opaque.hasAlphaChannel());
}

// ============================== L5: Resample ==============================

void LimgenTests::resample_lanczosGivesExactSizes() {
    const QImage image = twoTone(qRgb(255, 0, 0), qRgb(0, 0, 255), QSize(40, 30));
    for (const QSize size : {QSize(80, 60), QSize(20, 15), QSize(7, 11), QSize(100, 20), QSize(40, 30), QSize(1, 1)}) {
        const QImage result = Resample::lanczos(image, size);
        QCOMPARE(result.size(), size);
        QVERIFY(!result.hasAlphaChannel());
    }
    QVERIFY(Resample::lanczos(QImage(10, 10, QImage::Format_ARGB32), QSize(20, 20)).hasAlphaChannel());
}

void LimgenTests::resample_flatImageStaysFlat() {
    for (const QSize size : {QSize(100, 50), QSize(7, 3)}) {
        const QImage result = Resample::lanczos(solidImage(QSize(20, 10), QColor(40, 120, 200)), size);
        for (int y = 0; y < result.height(); ++y)
            for (int x = 0; x < result.width(); ++x) {
                const QRgb pixel = result.pixel(x, y);
                QVERIFY2(qAbs(qRed(pixel) - 40) <= 1 && qAbs(qGreen(pixel) - 120) <= 1 && qAbs(qBlue(pixel) - 200) <= 1,
                         qPrintable(QString("%1,%2").arg(x).arg(y)));
            }
    }
    // With alpha too.
    QImage translucent(20, 10, QImage::Format_ARGB32);
    translucent.fill(QColor(40, 120, 200, 128));
    const QImage up = Resample::lanczos(translucent, QSize(60, 30));
    QVERIFY(qAbs(qAlpha(up.pixel(30, 15)) - 128) <= 1);
    QVERIFY(qAbs(qRed(up.pixel(30, 15)) - 40) <= 2);
}

void LimgenTests::resample_lanczosKeepsAnEdge() {
    QImage image(20, 10, QImage::Format_RGB32);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x)
            image.setPixel(x, y, x < 10 ? qRgb(0, 0, 0) : qRgb(255, 255, 255));
    const QImage up = Resample::lanczos(image, QSize(80, 40));
    QVERIFY(qRed(up.pixel(0, 5)) <= 3);
    QVERIFY(qRed(up.pixel(79, 5)) >= 252);
    // The step stays sharp: two pixels either side of the middle are already close to the ends.
    QVERIFY(qRed(up.pixel(34, 5)) < 40);
    QVERIFY(qRed(up.pixel(45, 5)) > 215);
    QVERIFY(qRed(up.pixel(30, 5)) <= qRed(up.pixel(44, 5)));
}

void LimgenTests::resample_coverCropsTheCentre() {
    // Four vertical bands of 10 pixels: red, green, blue, yellow.
    QImage image(40, 20, QImage::Format_RGB32);
    const QRgb bands[4] = {qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(255, 255, 0)};
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            image.setPixel(x, y, bands[x / 10]);
    // The same height, half the width: the centre 20 pixels, green then blue, with no resampling.
    const QImage square = Resample::cover(image, QSize(20, 20));
    QCOMPARE(square.size(), QSize(20, 20));
    QCOMPARE(square.pixel(0, 5), bands[1]);
    QCOMPARE(square.pixel(9, 5), bands[1]);
    QCOMPARE(square.pixel(10, 5), bands[2]);
    QCOMPARE(square.pixel(19, 5), bands[2]);
    // The ratio is kept: a taller target is covered by scaling up and cropping the sides.
    const QImage tall = Resample::cover(image, QSize(20, 40));
    QCOMPARE(tall.size(), QSize(20, 40));
    // Scale 2: 80x40, crop x from 30: columns 30..49 of the scaled image, green (20..39) and blue (40..59).
    QVERIFY(qGreen(tall.pixel(2, 20)) > 200 && qRed(tall.pixel(2, 20)) < 40);
    QVERIFY(qBlue(tall.pixel(17, 20)) > 200 && qRed(tall.pixel(17, 20)) < 40);
    // Rows: a wide target crops top and bottom.
    QImage rows(20, 40, QImage::Format_RGB32);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 20; ++x)
            rows.setPixel(x, y, y < 10 ? qRgb(255, 0, 0) : y < 30 ? qRgb(0, 255, 0) : qRgb(0, 0, 255));
    const QImage wide = Resample::cover(rows, QSize(20, 20));
    QCOMPARE(wide.pixel(5, 0), qRgb(0, 255, 0)); // rows 10..29: all green
    QCOMPARE(wide.pixel(5, 19), qRgb(0, 255, 0));
}

void LimgenTests::resample_coverUpAndDown() {
    QImage image(100, 50, QImage::Format_RGB32);
    image.fill(QColor(30, 60, 90));
    for (const QSize size : {QSize(40, 40), QSize(400, 300), QSize(100, 50), QSize(33, 77), QSize(1, 1)}) {
        const QImage result = Resample::cover(image, size);
        QCOMPARE(result.size(), size);
        QVERIFY(qAbs(qRed(result.pixel(size.width() / 2, size.height() / 2)) - 30) <= 1);
    }
}

void LimgenTests::resample_nullAndEmpty() {
    QVERIFY(Resample::lanczos(QImage(), QSize(10, 10)).isNull());
    QVERIFY(Resample::lanczos(solidImage(QSize(4, 4), Qt::red), QSize(0, 10)).isNull());
    QVERIFY(Resample::cover(QImage(), QSize(10, 10)).isNull());
    QVERIFY(Resample::cover(solidImage(QSize(4, 4), Qt::red), QSize()).isNull());
}

// ============================== L5: Library finals and discards ===========

namespace {

LibraryEntry addTestDraft(Library &library, const QString &directory, const QString &subject = "oil rig at dusk",
                          const QDateTime &created = fixedTime) {
    DraftRecord record = fullRecord();
    record.subject = subject;
    return library.addDraft(makePng(directory + "/src-" + QString::number(QRandomGenerator::global()->generate()) + ".png"),
                            record, created);
}

void setAge(const QString &path, const QDateTime &when) {
    QFile file(path);
    if (file.open(QIODevice::ReadWrite))
        file.setFileTime(when, QFileDevice::FileModificationTime);
}

} // namespace

void LimgenTests::library_addFinalPngAndJpeg() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    const QJsonObject metadata{{"subject", "oil rig at dusk"}, {"size", QJsonArray{320, 180}},
                               {"finish", QJsonObject{{"from", "2026-11-20-aaaaaa"}}}};
    const QString png = fixture.root.path() + "/final.png", jpg = fixture.root.path() + "/final.jpg";
    QVERIFY(savePng(solidImage(QSize(32, 18), QColor(10, 120, 200)), png));
    QVERIFY(solidImage(QSize(32, 18), QColor(10, 120, 200)).save(jpg, "JPEG"));

    QString error;
    const LibraryEntry a = library.addFinal(png, metadata, fixedTime, &error);
    QVERIFY2(a.isValid(), qPrintable(error));
    QVERIFY(Library::isValidId(a.id));
    QVERIFY(a.id.startsWith("2026-11-20-"));
    QCOMPARE(a.image, fixture.data + "/library/2026-11/" + a.id + ".png");
    QCOMPARE(readFile(a.image), readFile(png));
    // The sidecar is the given one with id, kind and created filled in.
    const QJsonObject json = QJsonDocument::fromJson(readFile(a.sidecar)).object();
    QCOMPARE(json["id"].toString(), a.id);
    QCOMPARE(json["kind"].toString(), QString("final"));
    QCOMPARE(json["created"].toString(), QString("2026-11-20T10:30:15Z"));
    QCOMPARE(json["subject"].toString(), QString("oil rig at dusk"));
    QCOMPARE(json["size"].toArray()[0].toInt(), 320);
    QCOMPARE(json["finish"].toObject()["from"].toString(), QString("2026-11-20-aaaaaa"));
    // The given metadata could carry an id, a kind or a date of its own: they are replaced.
    QJsonObject odd = metadata;
    odd["id"] = "wrong";
    odd["kind"] = "draft";
    odd["created"] = "never";
    const LibraryEntry b = library.addFinal(jpg, odd, fixedTime);
    QVERIFY(b.isValid());
    QVERIFY(b.id != "wrong");
    QCOMPARE(b.metadata["kind"].toString(), QString("final"));
    QVERIFY(b.image.endsWith(".jpg"));
    QCOMPARE(readFile(b.image), readFile(jpg));
    // A ".jpeg" source is filed as .jpg; the suffix is read in any case.
    const QString jpeg = fixture.root.path() + "/final.JPEG";
    QVERIFY(QFile::copy(jpg, jpeg));
    QVERIFY(library.addFinal(jpeg, metadata, fixedTime).image.endsWith(".jpg"));
    const QString upper = fixture.root.path() + "/final.PNG";
    QVERIFY(QFile::copy(png, upper));
    QVERIFY(library.addFinal(upper, metadata, fixedTime).image.endsWith(".png"));
    // Both are found by id, whichever the format, and no .part is left.
    QCOMPARE(library.find(a.id).image, a.image);
    QCOMPARE(library.find(b.id).image, b.image);
    QCOMPARE(library.find(b.id).metadata["kind"].toString(), QString("final"));
    for (const QString &file : QDir(fixture.data + "/library/2026-11").entryList(QDir::Files))
        QVERIFY2(!file.endsWith(".part"), qPrintable(file));
}

void LimgenTests::library_addFinalFailsCleanly() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    QString error;
    QVERIFY(!library.addFinal(fixture.root.path() + "/nope.png", {}, fixedTime, &error).isValid());
    QVERIFY(!error.isEmpty());
    QCOMPARE(QDir(fixture.data + "/library/2026-11").entryList(QDir::Files | QDir::NoDotAndDotDot), QStringList());
    QVERIFY(!library.addFinal(fixture.root.path() + "/nope.png", {}, fixedTime).isValid()); // null error pointer
    QVERIFY(writeFile(fixture.root.path() + "/blocker", "x"));
    Library blocked(fixture.root.path() + "/blocker/library");
    const QString png = fixture.root.path() + "/a.png";
    QVERIFY(savePng(solidImage(QSize(4, 4), Qt::red), png));
    QVERIFY(!blocked.addFinal(png, {}, fixedTime, &error).isValid());
}

void LimgenTests::library_discardMovesTheDraftAside() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    const LibraryEntry draft = addTestDraft(library, fixture.root.path());
    const QByteArray image = readFile(draft.image), sidecar = readFile(draft.sidecar);
    // Discarded long after it was made: the file's own date is old.
    setAge(draft.image, QDateTime::currentDateTime().addDays(-5));
    setAge(draft.sidecar, QDateTime::currentDateTime().addDays(-5));
    QString error;
    QVERIFY2(library.discard(draft.id, &error), qPrintable(error));
    const QString bin = fixture.data + "/library/.discarded/" + draft.id;
    QVERIFY(!QFileInfo::exists(draft.image));
    QVERIFY(!QFileInfo::exists(draft.sidecar));
    QVERIFY(!library.find(draft.id).isValid());
    QCOMPARE(readFile(bin + ".png"), image); // moved, not changed
    QCOMPARE(readFile(bin + ".json"), sidecar);
    // The modification time says when it was discarded.
    for (const char *suffix : {".png", ".json"})
        QVERIFY2(QFileInfo(bin + suffix).lastModified() > QDateTime::currentDateTime().addSecs(-60), suffix);
    // A draft that is not there cannot be discarded.
    QVERIFY(!library.discard("2026-11-20-ffffff", &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!library.discard("../x", &error));
    QVERIFY(!library.discard(draft.id, &error)); // already discarded
}

void LimgenTests::library_restoreBringsItBackExactly() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    const LibraryEntry draft = addTestDraft(library, fixture.root.path());
    const QByteArray image = readFile(draft.image), sidecar = readFile(draft.sidecar);
    QVERIFY(library.discard(draft.id));
    QString error;
    QVERIFY2(library.restore(draft.id, &error), qPrintable(error));
    const LibraryEntry back = library.find(draft.id);
    QVERIFY(back.isValid());
    QCOMPARE(back.image, draft.image);
    QCOMPARE(readFile(back.image), image);
    QCOMPARE(readFile(back.sidecar), sidecar);
    QVERIFY(!QFileInfo::exists(fixture.data + "/library/.discarded/" + draft.id + ".png"));
    // Nothing to restore the second time, or for an unknown or malformed id.
    QVERIFY(!library.restore(draft.id, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!library.restore("2026-11-20-ffffff"));
    QVERIFY(!library.restore("../2026-11-20-ffffff"));
    QVERIFY(!library.restore(""));
    // Discard and restore again: the round trip can be repeated.
    QVERIFY(library.discard(draft.id));
    QVERIFY(library.restore(draft.id));
    QCOMPARE(readFile(library.find(draft.id).image), image);
}

void LimgenTests::library_aFinalCannotBeDiscarded() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    const QString png = fixture.root.path() + "/final.png";
    QVERIFY(savePng(solidImage(QSize(8, 8), Qt::blue), png));
    const LibraryEntry final = library.addFinal(png, {{"subject", "x"}}, fixedTime);
    QVERIFY(final.isValid());
    QString error;
    QVERIFY(!library.discard(final.id, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo::exists(final.image));
    QVERIFY(QFileInfo::exists(final.sidecar));
    QVERIFY(library.find(final.id).isValid());
    QVERIFY(!QFileInfo::exists(fixture.data + "/library/.discarded"));
}

void LimgenTests::library_purgeAndPurgeDiscarded() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    const LibraryEntry a = addTestDraft(library, fixture.root.path()), b = addTestDraft(library, fixture.root.path()),
                       c = addTestDraft(library, fixture.root.path());
    for (const LibraryEntry &entry : {a, b, c})
        QVERIFY(library.discard(entry.id));
    const QString bin = fixture.data + "/library/.discarded/";
    // purge deletes one discarded draft, and only that one.
    library.purge(a.id);
    QVERIFY(!QFileInfo::exists(bin + a.id + ".png") && !QFileInfo::exists(bin + a.id + ".json"));
    QVERIFY(QFileInfo::exists(bin + b.id + ".png"));
    library.purge(a.id);           // already gone
    library.purge("../library/x"); // never leaves the bin
    library.purge("");
    QVERIFY(QFileInfo::exists(bin + c.id + ".json"));
    // purgeDiscarded deletes those discarded before a time: b is old, c is new.
    for (const char *suffix : {".png", ".json"})
        setAge(bin + b.id + suffix, QDateTime::currentDateTime().addDays(-3));
    library.purgeDiscarded(QDateTime::currentDateTime().addDays(-1));
    QVERIFY(!QFileInfo::exists(bin + b.id + ".png") && !QFileInfo::exists(bin + b.id + ".json"));
    QVERIFY(QFileInfo::exists(bin + c.id + ".png") && QFileInfo::exists(bin + c.id + ".json"));
    QVERIFY(library.restore(c.id)); // the newer one can still be restored
    // Nothing discarded: nothing to do.
    Library empty(fixture.data + "/none");
    empty.purgeDiscarded(QDateTime::currentDateTime());
}

void LimgenTests::config_finishKeysDefaultsAndBounds() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString file = fixture.root.path() + "/config/limgen/config.json";
    Config config(file);
    QCOMPARE(config.colorPass(), 0.3);
    QCOMPARE(config.format(), QString("png"));
    QCOMPARE(config.jpegQuality(), 92);
    QCOMPARE(Config::defaultColorPass, 0.3);
    QCOMPARE(Config::defaultJpegQuality, 92);
    // The setters keep to the bounds.
    config.setColorPass(1.7);
    QCOMPARE(config.colorPass(), 1.0);
    config.setColorPass(-2);
    QCOMPARE(config.colorPass(), 0.0);
    config.setColorPass(0.55);
    config.setFormat("jpeg");
    QVERIFY(config.save());
    const QJsonObject json = QJsonDocument::fromJson(readFile(file)).object();
    QCOMPARE(json["colorPass"].toDouble(), 0.55);
    QCOMPARE(json["format"].toString(), QString("jpeg"));
    Config again(file);
    QCOMPARE(again.colorPass(), 0.55);
    QCOMPARE(again.format(), QString("jpeg"));
    again.setFormat("bmp"); // anything but jpeg is png
    QCOMPARE(again.format(), QString("png"));
    // A file with values out of range reads as the defaults.
    for (const QByteArray &content :
         {QByteArray(R"({"colorPass": 2, "format": "bmp", "jpegQuality": 0})"),
          QByteArray(R"({"colorPass": -0.1, "format": 5, "jpegQuality": 101})"),
          QByteArray(R"({"colorPass": "high", "format": null, "jpegQuality": "good"})")}) {
        QVERIFY(writeFile(file, content));
        const Config odd(file);
        QCOMPARE(odd.colorPass(), 0.3);
        QCOMPARE(odd.format(), QString("png"));
        QCOMPARE(odd.jpegQuality(), 92);
    }
    // The ends of the ranges are allowed.
    QVERIFY(writeFile(file, R"({"colorPass": 0, "jpegQuality": 1})"));
    QCOMPARE(Config(file).colorPass(), 0.0);
    QCOMPARE(Config(file).jpegQuality(), 1);
    QVERIFY(writeFile(file, R"({"colorPass": 1, "jpegQuality": 100})"));
    QCOMPARE(Config(file).colorPass(), 1.0);
    QCOMPARE(Config(file).jpegQuality(), 100);
}

void LimgenTests::omarchy_themeDirectoryLookupOrder() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString user = fixture.root.path() + "/user-themes", stock = fixture.root.path() + "/stock-themes";
    QVERIFY(QDir().mkpath(user + "/alpha"));
    QVERIFY(QDir().mkpath(stock + "/alpha"));
    QVERIFY(QDir().mkpath(stock + "/beta"));
    QVERIFY(fixture.addTheme("gamma", tomlWith("#111111", "#eeeeee", "#ff0000")));
    QVERIFY(fixture.switchTo("gamma"));
    OmarchyPaths paths = fixture.paths();
    paths.themes = {user, stock};
    const Omarchy omarchy(paths);
    QCOMPARE(omarchy.themeDirectory("alpha"), user + "/alpha"); // the user's first
    QCOMPARE(omarchy.themeDirectory("beta"), stock + "/beta");  // then the stock one
    QCOMPARE(omarchy.themeDirectory("gamma"), fixture.state + "/theme"); // the current theme: Omarchy's own copy
    QCOMPARE(omarchy.themeDirectory("nope"), QString());
    for (const QString &slug : {QString(), QString("../alpha"), QString("a/b"), QString(".hidden"), QString(".."), QString("x y")})
        QCOMPARE(omarchy.themeDirectory(slug), QString());
    // The current theme wins over a folder of the same name.
    QVERIFY(fixture.addTheme("alpha", tomlWith("#111111", "#eeeeee", "#ff0000")));
    QVERIFY(fixture.switchTo("alpha"));
    QCOMPARE(omarchy.themeDirectory("alpha"), fixture.state + "/theme");
    // A file named like a theme is not a theme.
    QVERIFY(writeFile(stock + "/file-not-folder", "x"));
    QCOMPARE(omarchy.themeDirectory("file-not-folder"), QString());
}

void LimgenTests::omarchy_themeBackgroundsOfATheme() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString stock = fixture.root.path() + "/stock-themes";
    touchAll(stock + "/beta/backgrounds", imageFixtureNames);
    QVERIFY(QDir().mkpath(stock + "/bare"));
    QVERIFY(fixture.addTheme("gamma", tomlWith("#111111", "#eeeeee", "#ff0000")));
    touchAll(fixture.themes + "/gamma/backgrounds", {"c.png", "a.jpg"});
    QVERIFY(fixture.switchTo("gamma"));
    OmarchyPaths paths = fixture.paths();
    paths.themes = {stock};
    const Omarchy omarchy(paths);
    QStringList expected;
    for (const QString &name : imageFixtureWanted)
        expected << stock + "/beta/backgrounds/" + name;
    QCOMPARE(omarchy.themeBackgrounds("beta"), expected);
    QCOMPARE(omarchy.themeBackgrounds("bare"), QStringList()); // no backgrounds folder
    QCOMPARE(omarchy.themeBackgrounds("nope"), QStringList());
    QCOMPARE(omarchy.themeBackgrounds("../beta"), QStringList());
    // The current theme's images are read through Omarchy's copy.
    QCOMPARE(omarchy.themeBackgrounds("gamma"),
             (QStringList{fixture.state + "/theme/backgrounds/a.jpg", fixture.state + "/theme/backgrounds/c.png"}));
}

void LimgenTests::omarchy_keepDirectory() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Omarchy omarchy(fixture.paths());
    QCOMPARE(omarchy.keepDirectory("tokyo-night"), fixture.backgrounds + "/tokyo-night");
    QCOMPARE(omarchy.keepDirectory(""), fixture.pictures()); // no theme: the pictures folder
    for (const QString &slug : {QString("../x"), QString("a/b"), QString(".hidden"), QString(".."), QString("x y")})
        QCOMPARE(omarchy.keepDirectory(slug), QString());
    // Asking creates nothing.
    QVERIFY(!QFileInfo::exists(fixture.backgrounds + "/tokyo-night"));
    QVERIFY(!QFileInfo::exists(fixture.pictures()));
}

void LimgenTests::omarchy_currentBackground() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const Omarchy omarchy(fixture.paths());
    QCOMPARE(omarchy.currentBackground(), QString()); // no link
    const QString target = fixture.root.path() + "/bg/one.png";
    QVERIFY(writeFile(target, "x"));
    QVERIFY(QFile::link(target, fixture.state + "/background"));
    QCOMPARE(omarchy.currentBackground(), target);
    QVERIFY(replaceSymlink(fixture.root.path() + "/bg/two.png", fixture.state + "/background"));
    QCOMPARE(omarchy.currentBackground(), fixture.root.path() + "/bg/two.png");
    // A plain file of that name is not Omarchy's link.
    QVERIFY(QFile::remove(fixture.state + "/background"));
    QVERIFY(writeFile(fixture.state + "/background", "x"));
    QCOMPARE(omarchy.currentBackground(), QString());
}

void LimgenTests::omarchy_setBackgroundThroughTheFakeOmarchy() {
    Sandbox sb; // FAKE_OMARCHY_FAIL is put back when it ends
    Fixture &fixture = sb.fixture;
    const Omarchy omarchy(fixture.paths());
    const QString image = fixture.root.path() + "/bg/new.png";
    QString error;
    QVERIFY2(omarchy.setBackground(image, &error), qPrintable(error));
    QCOMPARE(fixture.omarchyCalls(), QStringList{"theme bg set " + image});
    QCOMPARE(omarchy.currentBackground(), image); // the fake moved the link like Omarchy does
    QVERIFY(omarchy.setBackground(image)); // a null error pointer is accepted
    QCOMPARE(fixture.omarchyCalls().size(), 2);
    // Omarchy fails: false, and what it said.
    qputenv("FAKE_OMARCHY_FAIL", "1");
    QVERIFY(!omarchy.setBackground(fixture.root.path() + "/bg/other.png", &error));
    QVERIFY2(error.contains("boom"), qPrintable(error));
    QCOMPARE(omarchy.currentBackground(), image); // unchanged
    qunsetenv("FAKE_OMARCHY_FAIL");
    // No Omarchy known, or one that is not there.
    OmarchyPaths paths = fixture.paths();
    paths.omarchy = QString();
    QVERIFY(!Omarchy(paths).setBackground(image, &error));
    QVERIFY2(error.contains(image), qPrintable(error)); // says where the image is instead
    paths.omarchy = fixture.root.path() + "/no-such-omarchy";
    QVERIFY(!Omarchy(paths).setBackground(image, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(fixture.omarchyCalls().size(), 3); // the failing call was logged, the others never ran
}

void LimgenTests::omarchy_pathsThemesAndPictures() {
    EnvGuard guard({"LIMGEN_OMARCHY_THEMES", "LIMGEN_PICTURES", "XDG_CONFIG_HOME", "LIMGEN_OMARCHY_BIN"});
    qputenv("XDG_CONFIG_HOME", "/tmp/limgen-test/config");
    qputenv("LIMGEN_OMARCHY_THEMES", "/tmp/limgen-test/a:/tmp/limgen-test/b::/tmp/limgen-test/c");
    qputenv("LIMGEN_PICTURES", "/tmp/limgen-test/pictures");
    OmarchyPaths paths = OmarchyPaths::fromEnvironment();
    QCOMPARE(paths.themes, (QStringList{"/tmp/limgen-test/a", "/tmp/limgen-test/b", "/tmp/limgen-test/c"}));
    QCOMPARE(paths.pictures, QString("/tmp/limgen-test/pictures"));
    // Without overrides: the user's themes, then the stock ones; pictures under Pictures/limgen.
    qunsetenv("LIMGEN_OMARCHY_THEMES");
    qputenv("LIMGEN_PICTURES", "");
    paths = OmarchyPaths::fromEnvironment();
    QCOMPARE(paths.themes, (QStringList{"/tmp/limgen-test/config/omarchy/themes", "/usr/share/omarchy/themes"}));
    QVERIFY2(paths.pictures.endsWith("/limgen"), qPrintable(paths.pictures));
    QVERIFY(paths.pictures.contains("Pictures"));
}

// ============================== L5: Keeper ================================

namespace {

struct KeeperRun {
    bool finished = false, failed = false;
    Keeper::Outcome outcome = Keeper::Outcome::Ok;
    QString message;
    KeepResult result;
    QList<double> progress;
    bool over() const { return finished || failed; }
};

void watchKeeper(Keeper &keeper, KeeperRun &run) {
    QObject::connect(&keeper, &Keeper::progress, &keeper, [&run](double fraction) { run.progress << fraction; });
    QObject::connect(&keeper, &Keeper::finished, &keeper, [&run](const KeepResult &result) {
        run.finished = true;
        run.result = result;
    });
    QObject::connect(&keeper, &Keeper::failed, &keeper, [&run](Keeper::Outcome outcome, const QString &message) {
        run.failed = true;
        run.outcome = outcome;
        run.message = message;
    });
}

// A sandbox with the upscaler installed, a theme "night-city" with a
// colors.toml, a 64x48 draft made for it, and a Keeper on a 320x180 monitor.
struct KeepRig {
    Sandbox sb;
    Catalogue catalogue = Catalogue::builtIn();
    EngineProbe probe;
    KeeperRun run;
    std::unique_ptr<Keeper> keeper;
    LibraryEntry draft;

    explicit KeepRig(bool upscaler = true) {
        if (upscaler)
            makeSparse(Catalogue::path(*catalogue.upscaler(), sb.fixture.models), catalogue.upscaler()->bytes);
        sb.fixture.addTheme("night-city", stockToml);
        probe = SdCppEngine(fakeEngine).probe();
        remake();
        draft = makeDraft();
    }
    void remake(const OmarchyPaths &paths = OmarchyPaths()) {
        run = KeeperRun();
        keeper = std::make_unique<Keeper>(catalogue, paths.state.isEmpty() ? sb.paths() : paths, fakeEngine);
        watchKeeper(*keeper, run);
    }
    LibraryEntry makeDraft(const QString &subject = "oil rig at dusk", const QString &themeSlug = "night-city",
                           const QSize &size = QSize(64, 48)) {
        Library library(sb.library());
        DraftRecord record = fullRecord();
        record.subject = subject;
        record.themeSlug = themeSlug;
        if (themeSlug.isEmpty()) {
            record.themeMode.clear();
            record.palette = QJsonObject();
        }
        record.width = size.width();
        record.height = size.height();
        return library.addDraft(makePng(sb.fixture.root.path() + "/src-" + QString::number(QRandomGenerator::global()->generate()) + ".png",
                                        size.width(), size.height()),
                                record, QDateTime::currentDateTimeUtc());
    }
    KeepSettings settings(const QString &theme = "night-city") const {
        KeepSettings settings;
        settings.draftId = draft.id;
        settings.theme = theme;
        settings.colorPass = 0.3;
        settings.size = QSize(320, 180);
        return settings;
    }
    QString keepFolder(const QString &theme = "night-city") const { return sb.fixture.backgrounds + "/" + theme; }
    Keeper::Outcome prepare(const KeepSettings &settings, QString *message = nullptr) {
        return keeper->prepare(settings, probe, message);
    }
    void keepNow(const KeepSettings &settings) {
        QString message;
        QVERIFY2(prepare(settings, &message) == Keeper::Outcome::Ok, qPrintable(message));
        keeper->start();
    }
    QStringList keptFiles(const QString &theme = "night-city") const {
        QStringList files = QDir(keepFolder(theme)).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        return files;
    }
};

#define WAIT_FOR_KEEPER(rig) QTRY_VERIFY_WITH_TIMEOUT((rig).run.over(), 30000)
#define KEEP_AND_WAIT(rig, settings) \
    (rig).keepNow(settings);         \
    if (QTest::currentTestFailed())  \
        return;                      \
    WAIT_FOR_KEEPER(rig)

// The calls that upscale, not the probe's --version and --list-devices.
QList<QStringList> upscaleCalls(const QString &log) {
    QList<QStringList> calls;
    for (const QStringList &call : fakeCalls(log))
        if (call.contains("-M"))
            calls << call;
    return calls;
}

int jpegLumaDc(const QByteArray &jpeg) {
    const int at = jpeg.indexOf("\xFF\xDB");
    return at < 0 ? -1 : quint8(jpeg.at(at + 5));
}

QStringList cacheEntries(const Sandbox &sb) {
    return QDir(sb.fixture.cache + "/limgen").entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
}

} // namespace

void LimgenTests::keeper_fileNames_data() {
    QTest::addColumn<QString>("subject");
    QTest::addColumn<QString>("format");
    QTest::addColumn<QString>("expected");
    const QString id = "2026-11-20-a3f9c1";
    QTest::newRow("plain words") << "oil rig at dusk" << "png" << "limgen-oil-rig-at-dusk-a3f9c1.png";
    QTest::newRow("upper case") << "Oil Rig" << "png" << "limgen-oil-rig-a3f9c1.png";
    QTest::newRow("jpeg is .jpg") << "oil rig" << "jpeg" << "limgen-oil-rig-a3f9c1.jpg";
    QTest::newRow("accents come off") << QString::fromUtf8("Forêt brumeuse à l'aube") << "png"
                                      << "limgen-foret-brumeuse-a-l-aube-a3f9c1.png";
    QTest::newRow("other scripts leave nothing") << QString::fromUtf8("夕焼けの海") << "png" << "limgen-background-a3f9c1.png";
    QTest::newRow("other script between words") << QString::fromUtf8("Paysage 日本 fuji") << "png"
                                                 << "limgen-paysage-fuji-a3f9c1.png";
    QTest::newRow("full-width letters fold to ASCII") << QString::fromUtf8("ＡＢＣ day") << "png" << "limgen-abc-day-a3f9c1.png";
    QTest::newRow("punctuation") << "!!!rock & roll??" << "png" << "limgen-rock-roll-a3f9c1.png";
    QTest::newRow("digits") << "4k wallpaper" << "png" << "limgen-4k-wallpaper-a3f9c1.png";
    QTest::newRow("empty") << "" << "png" << "limgen-background-a3f9c1.png";
    QTest::newRow("only spaces") << "   " << "png" << "limgen-background-a3f9c1.png";
    QTest::newRow("only punctuation") << "?!?" << "jpeg" << "limgen-background-a3f9c1.jpg";
    QTest::newRow("slashes cannot make a path") << "../../etc/passwd" << "png" << "limgen-etc-passwd-a3f9c1.png";
    // 40 characters at most, and the cut never leaves a trailing dash.
    QTest::newRow("long") << "a very long description of an oil rig standing in the sea at dusk" << "png"
                          << "limgen-a-very-long-description-of-an-oil-rig-st-a3f9c1.png";
    QTest::newRow("cut at a dash") << "abcdefghijklmnopqrstuvwxyz0123456789abc defgh" << "png"
                                   << "limgen-abcdefghijklmnopqrstuvwxyz0123456789abc-a3f9c1.png";
    Q_UNUSED(id);
}

void LimgenTests::keeper_fileNames() {
    QFETCH(QString, subject);
    QFETCH(QString, format);
    QFETCH(QString, expected);
    const QString name = Keeper::fileName(subject, "2026-11-20-a3f9c1", format);
    QCOMPARE(name, expected);
    // Whatever the subject, it is one plain file name that starts with limgen-.
    QVERIFY(name.startsWith("limgen-"));
    QVERIFY(!name.contains('/') && !name.contains(".."));
    QVERIFY(name.size() <= 7 + 40 + 1 + 6 + 4);
}

void LimgenTests::keeper_parametersText() {
    const QJsonObject metadata{{"prompt", "oil rig at dusk, cinematic"},
                               {"negative", "blurry, text"},
                               {"steps", 8},
                               {"sampler", "euler"},
                               {"cfg", 1.0},
                               {"seed", 4000000000.0},
                               {"size", QJsonArray{2560, 1440}},
                               {"model", QJsonObject{{"id", "z-image-turbo"}}}};
    QCOMPARE(Keeper::parameters(metadata),
             QString("oil rig at dusk, cinematic\nNegative prompt: blurry, text\n"
                     "Steps: 8, Sampler: euler, CFG scale: 1, Seed: 4000000000, Size: 2560x1440, Model: z-image-turbo"));
    // No negative prompt, no line for it.
    QJsonObject plain = metadata;
    plain["negative"] = "";
    QCOMPARE(Keeper::parameters(plain).split('\n').size(), 2);
    QVERIFY(!Keeper::parameters(plain).contains("Negative"));
    QVERIFY(Keeper::parameters(plain).startsWith("oil rig at dusk, cinematic\nSteps: 8"));
}

void LimgenTests::keeper_prepareOutcomesInOrder() {
    KeepRig rig;
    QString message;
    // A draft that is not in the library, however the id is written.
    KeepSettings settings = rig.settings();
    settings.draftId = "2026-11-20-ffffff";
    QCOMPARE(int(rig.prepare(settings, &message)), int(Keeper::Outcome::UnknownDraft));
    QVERIFY2(message.contains("2026-11-20-ffffff"), qPrintable(message));
    settings.draftId = "nope";
    QCOMPARE(int(rig.prepare(settings)), int(Keeper::Outcome::UnknownDraft));
    // A final is not a draft.
    const QString png = rig.sb.fixture.root.path() + "/f.png";
    QVERIFY(savePng(solidImage(QSize(8, 8), Qt::red), png));
    const LibraryEntry final = Library(rig.sb.library()).addFinal(png, {{"subject", "x"}}, fixedTime);
    settings.draftId = final.id;
    QCOMPARE(int(rig.prepare(settings, &message)), int(Keeper::Outcome::UnknownDraft));
    QVERIFY2(message.contains("not a draft"), qPrintable(message));
    // The draft is checked before the theme, the theme before the upscaler, the upscaler before the engine.
    KeepSettings noDraftNoTheme = rig.settings("no-such-theme");
    noDraftNoTheme.draftId = "2026-11-20-ffffff";
    QCOMPARE(int(rig.prepare(noDraftNoTheme)), int(Keeper::Outcome::UnknownDraft));
    QCOMPARE(int(rig.prepare(rig.settings("no-such-theme"), &message)), int(Keeper::Outcome::UnknownTheme));
    QVERIFY2(message.contains("no-such-theme"), qPrintable(message));
    QCOMPARE(int(rig.prepare(rig.settings("../x"))), int(Keeper::Outcome::UnknownTheme));
    EngineProbe bad;
    bad.error = {EngineError::NoGpu, "the engine says no", {}};
    const CatalogueFile &upscaler = *rig.catalogue.upscaler();
    QVERIFY(QFile::remove(Catalogue::path(upscaler, rig.sb.fixture.models)));
    rig.probe = bad;
    QCOMPARE(int(rig.prepare(rig.settings("no-such-theme"))), int(Keeper::Outcome::UnknownTheme));
    QCOMPARE(int(rig.prepare(rig.settings(), &message)), int(Keeper::Outcome::NotRunnable));
    QVERIFY2(message.contains(upscaler.name), qPrintable(message)); // names the missing upscaler, not the engine
    QVERIFY(!message.contains("the engine says no"));
    QVERIFY(makeSparse(Catalogue::path(upscaler, rig.sb.fixture.models), upscaler.bytes - 1)); // wrong size counts as missing
    QCOMPARE(int(rig.prepare(rig.settings())), int(Keeper::Outcome::NotRunnable));
    QVERIFY(makeSparse(Catalogue::path(upscaler, rig.sb.fixture.models), upscaler.bytes));
    QCOMPARE(int(rig.prepare(rig.settings(), &message)), int(Keeper::Outcome::NotRunnable));
    QCOMPARE(message, QString("the engine says no"));
    rig.probe = SdCppEngine(fakeEngine).probe();
    // A draft whose image cannot be read.
    QVERIFY(writeFile(rig.draft.image, "not an image"));
    QCOMPARE(int(rig.prepare(rig.settings(), &message)), int(Keeper::Outcome::Failed));
    QVERIFY2(message.contains(".png"), qPrintable(message));
    QVERIFY(!rig.keeper->isRunning());
}

void LimgenTests::keeper_prepareFillsTheResult() {
    KeepRig rig;
    QString message;
    QVERIFY2(rig.prepare(rig.settings(), &message) == Keeper::Outcome::Ok, qPrintable(message));
    const KeepResult &result = rig.keeper->result();
    QCOMPARE(result.draft.id, rig.draft.id);
    QCOMPARE(result.theme, QString("night-city"));
    QCOMPARE(result.installed, rig.keepFolder() + "/" + Keeper::fileName("oil rig at dusk", rig.draft.id, "png"));
    QVERIFY(!result.alreadyKept);
    QVERIFY(!result.final.isValid());
    QCOMPARE(rig.keeper->finalSize(), QSize(320, 180));
    // Nothing is made or written by preparing.
    QVERIFY(!QFileInfo::exists(rig.keepFolder()));
    QCOMPARE(upscaleCalls(rig.sb.fakeLog()).size(), 0);
    // With no size given, the largest monitor's: a rotated one counts turned.
    KeepSettings noSize = rig.settings();
    noSize.size = QSize();
    OmarchyPaths paths = rig.sb.paths();
    const QString script = rig.sb.fixture.root.path() + "/two-monitors";
    QVERIFY(writeScript(script, R"(echo '[{"name":"A","width":1920,"height":1080,"transform":0},
                                          {"name":"B","width":1080,"height":2560,"transform":1}]'
)"));
    paths.hyprctl = script;
    rig.remake(paths);
    QVERIFY(rig.prepare(noSize) == Keeper::Outcome::Ok);
    QCOMPARE(rig.keeper->finalSize(), QSize(2560, 1080));
    // The size in the settings wins over any monitor.
    rig.remake(paths);
    QVERIFY(rig.prepare(rig.settings()) == Keeper::Outcome::Ok);
    QCOMPARE(rig.keeper->finalSize(), QSize(320, 180));
}

void LimgenTests::keeper_alreadyKeptIsFoundInEitherFormat() {
    KeepRig rig;
    const QString png = rig.keepFolder() + "/" + Keeper::fileName("oil rig at dusk", rig.draft.id, "png");
    const QString jpg = rig.keepFolder() + "/" + Keeper::fileName("oil rig at dusk", rig.draft.id, "jpeg");
    QVERIFY(writeFile(png, "old png"));
    KeepSettings settings = rig.settings();
    QVERIFY(rig.prepare(settings) == Keeper::Outcome::Ok);
    QVERIFY(rig.keeper->result().alreadyKept);
    QCOMPARE(rig.keeper->result().installed, png);
    // Asked for JPEG while the PNG is there: still kept.
    settings.format = "jpeg";
    QVERIFY(rig.prepare(settings) == Keeper::Outcome::Ok);
    QVERIFY(rig.keeper->result().alreadyKept);
    QCOMPARE(rig.keeper->result().installed, png);
    // And the other way round.
    QVERIFY(QFile::remove(png));
    QVERIFY(writeFile(jpg, "old jpg"));
    settings.format = "png";
    QVERIFY(rig.prepare(settings) == Keeper::Outcome::Ok);
    QVERIFY(rig.keeper->result().alreadyKept);
    QCOMPARE(rig.keeper->result().installed, jpg);
    // A file of another draft's name is no match.
    QVERIFY(QFile::remove(jpg));
    QVERIFY(writeFile(rig.keepFolder() + "/limgen-oil-rig-at-dusk-000000.png", "other"));
    QVERIFY(rig.prepare(settings) == Keeper::Outcome::Ok);
    QVERIFY(!rig.keeper->result().alreadyKept);
    QCOMPARE(rig.keeper->result().installed, png);
}

void LimgenTests::keeper_fullKeepWritesEverythingTheDocsSay() {
    KeepRig rig;
    const QByteArray draftImage = readFile(rig.draft.image), draftSidecar = readFile(rig.draft.sidecar);
    KEEP_AND_WAIT(rig, rig.settings());
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    const KeepResult &result = rig.run.result;
    QVERIFY(!result.alreadyKept);
    QCOMPARE(result.theme, QString("night-city"));
    // Installed under the theme's folder with the documented name, at the final size.
    const QString name = Keeper::fileName("oil rig at dusk", rig.draft.id, "png");
    QCOMPARE(result.installed, rig.keepFolder() + "/" + name);
    QCOMPARE(QImageReader(result.installed).size(), QSize(320, 180));
    QCOMPARE(rig.keptFiles(), QStringList{name}); // and nothing else: no .part, no upscaled.png
    // The final is in the library, and the installed file is a copy of it.
    QVERIFY(result.final.isValid());
    QVERIFY(result.final.id != rig.draft.id);
    const LibraryEntry found = Library(rig.sb.library()).find(result.final.id);
    QVERIFY(found.isValid());
    QCOMPARE(readFile(result.installed), readFile(found.image));
    QVERIFY(found.image.endsWith(".png"));
    // The sidecar follows docs/technical.md section 8.
    const QJsonObject meta = found.metadata;
    QCOMPARE(meta["id"].toString(), result.final.id);
    QCOMPARE(meta["kind"].toString(), QString("final"));
    QVERIFY(QDateTime::fromString(meta["created"].toString(), Qt::ISODate).isValid());
    QCOMPARE(meta["size"].toArray()[0].toInt(), 320);
    QCOMPARE(meta["size"].toArray()[1].toInt(), 180);
    const QJsonObject draftMeta = rig.draft.metadata;
    for (const char *key : {"theme", "subject", "prompt", "negative", "style", "model", "engine", "seed", "steps", "cfg", "sampler"})
        QCOMPARE(meta[key], draftMeta[key]);
    const QJsonObject finish = meta["finish"].toObject();
    QCOMPARE(finish["from"].toString(), rig.draft.id);
    QCOMPARE(finish["upscaler"].toString(), QString("realesrgan-x4plus"));
    QCOMPARE(finish["colorPass"].toDouble(), 0.3);
    QCOMPARE(finish["colorTarget"].toString(), QString("palette")); // the theme has colors.toml and no backgrounds
    QCOMPARE(finish["theme"].toString(), QString("night-city"));
    QCOMPARE(finish["size"].toArray()[0].toInt(), 320);
    QCOMPARE(finish["format"].toString(), QString("png"));
    QVERIFY(!finish.contains("quality"));
    // The prompt travels in the file.
    // (Qt's reader folds the line breaks to spaces; the file is written with them.)
    QCOMPARE(QImageReader(result.installed).text("parameters"), Keeper::parameters(meta).simplified());
    // The draft is untouched; the work folder is gone; the upscale ran at tile size 256.
    QCOMPARE(readFile(rig.draft.image), draftImage);
    QCOMPARE(readFile(rig.draft.sidecar), draftSidecar);
    QCOMPARE(cacheEntries(rig.sb), QStringList());
    QCOMPARE(valueAfter(upscaleCalls(rig.sb.fakeLog()).first(), "--upscale-tile-size"), QString("256"));
    QCOMPARE(valueAfter(upscaleCalls(rig.sb.fakeLog()).first(), "-i"), rig.draft.image);
    // Progress rose to 1.
    QVERIFY(!rig.run.progress.isEmpty());
    QCOMPARE(rig.run.progress.last(), 1.0);
    for (int i = 1; i < rig.run.progress.size(); ++i)
        QVERIFY(rig.run.progress[i] >= rig.run.progress[i - 1]);
    QVERIFY(!rig.keeper->isRunning());
}

void LimgenTests::keeper_colorTargets_data() {
    QTest::addColumn<int>("scenario");
    QTest::addColumn<double>("strength");
    QTest::addColumn<QString>("target"); // "" means null
    QTest::addColumn<double>("recorded");
    QTest::newRow("the theme's own backgrounds") << 0 << 0.5 << "backgrounds" << 0.5;
    QTest::newRow("its palette when it has no backgrounds") << 1 << 0.4 << "palette" << 0.4;
    QTest::newRow("the draft's recorded palette when the theme has no colors.toml") << 2 << 0.6 << "palette" << 0.6;
    QTest::newRow("nothing to aim at") << 3 << 0.5 << "" << 0.0;
    QTest::newRow("strength 0 turns it off") << 4 << 0.0 << "" << 0.0;
    QTest::newRow("no theme") << 5 << 0.5 << "" << 0.0;
}

void LimgenTests::keeper_colorTargets() {
    QFETCH(int, scenario);
    QFETCH(double, strength);
    QFETCH(QString, target);
    QFETCH(double, recorded);
    KeepRig rig;
    KeepSettings settings = rig.settings();
    settings.colorPass = strength;
    const QString themeDir = rig.sb.fixture.themes + "/night-city";
    switch (scenario) {
    case 0:
        QVERIFY(savePng(solidImage(QSize(32, 32), QColor(230, 140, 60)), themeDir + "/backgrounds/orange.png"));
        break;
    case 2: // a theme folder with no colors.toml, made for by this draft
        QVERIFY(QFile::remove(themeDir + "/colors.toml"));
        break;
    case 3: // no colors, and the draft was made for another theme
        QVERIFY(QFile::remove(themeDir + "/colors.toml"));
        rig.draft = rig.makeDraft("oil rig", "some-other-theme");
        settings.draftId = rig.draft.id;
        break;
    case 4:
        QVERIFY(savePng(solidImage(QSize(32, 32), QColor(230, 140, 60)), themeDir + "/backgrounds/orange.png"));
        break;
    case 5:
        settings.theme = QString();
        break;
    default:
        break;
    }
    KEEP_AND_WAIT(rig, settings);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    const QJsonObject finish = rig.run.result.final.metadata["finish"].toObject();
    if (target.isEmpty())
        QVERIFY2(finish["colorTarget"].isNull(), qPrintable(finish["colorTarget"].toString()));
    else
        QCOMPARE(finish["colorTarget"].toString(), target);
    QCOMPARE(finish["colorPass"].toDouble(), recorded);
    if (scenario == 5)
        QVERIFY(finish["theme"].isNull());
    else
        QCOMPARE(finish["theme"].toString(), QString("night-city"));
}

void LimgenTests::keeper_colorPassMovesTheColourTowardTheBackgrounds() {
    // The fake upscaler paints (90, 110, 140). Orange backgrounds pull it toward orange; off, it stays.
    auto keepWith = [](double strength, QColor *centre) {
        KeepRig rig;
        QVERIFY(savePng(solidImage(QSize(32, 32), QColor(230, 140, 60)),
                        rig.sb.fixture.themes + "/night-city/backgrounds/orange.png"));
        KeepSettings settings = rig.settings();
        settings.colorPass = strength;
        KEEP_AND_WAIT(rig, settings);
        QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
        *centre = QImage(rig.run.result.installed).pixelColor(160, 90);
    };
    QColor off, half, full;
    keepWith(0.0, &off);
    keepWith(0.5, &half);
    keepWith(1.0, &full);
    QVERIFY2(qAbs(off.red() - 90) <= 3 && qAbs(off.green() - 110) <= 3 && qAbs(off.blue() - 140) <= 3,
             qPrintable(off.name()));
    // Full strength: close to the backgrounds' colour (a flat image takes the target's means).
    QVERIFY2(qAbs(full.red() - 230) <= 14 && qAbs(full.green() - 140) <= 14 && qAbs(full.blue() - 60) <= 14,
             qPrintable(full.name()));
    // Half way between, on every channel.
    QVERIFY2(half.red() > off.red() + 30 && half.red() < full.red() - 30, qPrintable(half.name()));
    QVERIFY(half.blue() < off.blue() - 20 && half.blue() > full.blue() + 20);
}

void LimgenTests::keeper_jpegQualityAndComment() {
    QByteArray low, high;
    QString installedLow;
    for (const int quality : {50, 95}) {
        KeepRig rig;
        KeepSettings settings = rig.settings();
        settings.format = "jpeg";
        settings.jpegQuality = quality;
        KEEP_AND_WAIT(rig, settings);
        QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
        const KeepResult &result = rig.run.result;
        const QString name = Keeper::fileName("oil rig at dusk", rig.draft.id, "jpeg");
        QCOMPARE(result.installed, rig.keepFolder() + "/" + name);
        QVERIFY(name.endsWith(".jpg"));
        QVERIFY(result.final.image.endsWith(".jpg"));
        QCOMPARE(QImageReader(result.installed).size(), QSize(320, 180));
        QCOMPARE(rig.keptFiles(), QStringList{name});
        const QByteArray bytes = readFile(result.installed);
        QVERIFY(bytes.startsWith("\xFF\xD8")); // a JPEG
        QCOMPARE(bytes, readFile(result.final.image));
        // The prompt is in the file as a comment.
        QVERIFY2(bytes.contains(rig.draft.metadata["prompt"].toString().toUtf8()), "the prompt travels in the JPEG");
        const QJsonObject finish = result.final.metadata["finish"].toObject();
        QCOMPARE(finish["format"].toString(), QString("jpeg"));
        QCOMPARE(finish["quality"].toInt(), quality);
        (quality == 50 ? low : high) = bytes;
    }
    // The quality reached the encoder: the quantisation table is coarser at 50 than at 95.
    const int coarse = jpegLumaDc(low), fine = jpegLumaDc(high);
    QVERIFY2(coarse > fine && coarse >= 8 && fine <= 4, qPrintable(QString("%1 vs %2").arg(coarse).arg(fine)));
}

void LimgenTests::keeper_alreadyKeptIsReportedAsynchronously() {
    KeepRig rig;
    const QString existing = rig.keepFolder() + "/" + Keeper::fileName("oil rig at dusk", rig.draft.id, "png");
    QVERIFY(writeFile(existing, "kept before"));
    QVERIFY(rig.prepare(rig.settings()) == Keeper::Outcome::Ok);
    QSignalSpy finished(rig.keeper.get(), &Keeper::finished);
    rig.keeper->start();
    QVERIFY(rig.keeper->isRunning());
    QCOMPARE(finished.count(), 0); // not from inside start(): a caller waiting in an event loop must hear it
    WAIT_FOR_KEEPER(rig);
    QVERIFY(rig.run.finished);
    QVERIFY(!rig.keeper->isRunning());
    QVERIFY(rig.run.result.alreadyKept);
    QVERIFY(!rig.run.result.final.isValid()); // no new final
    QCOMPARE(rig.run.result.installed, existing);
    QCOMPARE(readFile(existing), QByteArray("kept before"));
    QCOMPARE(upscaleCalls(rig.sb.fakeLog()).size(), 0); // the engine was not asked
    QCOMPARE(filesUnder(rig.sb.library()).size(), 2); // just the draft and its sidecar
    QCOMPARE(rig.run.progress.last(), 1.0);
}

void LimgenTests::keeper_cancelDuringTheUpscale() {
    KeepRig rig;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.keepNow(rig.settings());
    QTRY_VERIFY_WITH_TIMEOUT(!upscaleCalls(rig.sb.fakeLog()).isEmpty(), 15000);
    QTest::qWait(300);
    QVERIFY(rig.keeper->isRunning());
    rig.keeper->cancel();
    WAIT_FOR_KEEPER(rig);
    QVERIFY(rig.run.failed);
    QCOMPARE(int(rig.run.outcome), int(Keeper::Outcome::Cancelled));
    QVERIFY(!rig.keeper->isRunning());
    QCOMPARE(cacheEntries(rig.sb), QStringList());
    QVERIFY(!QFileInfo::exists(rig.keepFolder()) || rig.keptFiles().isEmpty());
    QCOMPARE(filesUnder(rig.sb.library()).size(), 2); // no final
    // Cancelling again, or when idle, does nothing.
    rig.run = KeeperRun();
    rig.keeper->cancel();
    QTest::qWait(100);
    QVERIFY(!rig.run.over());
}

void LimgenTests::keeper_destroyingARunningKeeperIsSilent() {
    KeepRig rig;
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.keepNow(rig.settings());
    QTRY_VERIFY_WITH_TIMEOUT(!upscaleCalls(rig.sb.fakeLog()).isEmpty(), 15000);
    QTest::qWait(300);
    QCOMPARE(QProcess::execute("pgrep", {"-f", rig.sb.fixture.cache}), 0); // the engine is running
    rig.keeper.reset();
    QTest::qWait(200);
    QVERIFY2(!rig.run.over(), "a destroyed keeper reports nothing");
    QCOMPARE(QProcess::execute("pgrep", {"-f", rig.sb.fixture.cache}), 1); // and is gone
    QCOMPARE(cacheEntries(rig.sb), QStringList());
    QVERIFY(!QFileInfo::exists(rig.keepFolder()) || rig.keptFiles().isEmpty());
    QCOMPARE(filesUnder(rig.sb.library()).size(), 2);
}

void LimgenTests::keeper_failuresRemoveTheWorkFolder_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("outcome");
    QTest::addColumn<int>("calls");
    QTest::newRow("out of memory, after one retry") << "oom" << int(Keeper::Outcome::NotRunnable) << 2;
    QTest::newRow("engine fails") << "fail" << int(Keeper::Outcome::Failed) << 1;
    QTest::newRow("wrong size") << "wrong-size" << int(Keeper::Outcome::Failed) << 1;
}

void LimgenTests::keeper_failuresRemoveTheWorkFolder() {
    QFETCH(QString, mode);
    QFETCH(int, outcome);
    QFETCH(int, calls);
    KeepRig rig;
    qputenv("FAKE_SD_CLI_MODE", mode.toUtf8());
    KEEP_AND_WAIT(rig, rig.settings());
    QVERIFY(rig.run.failed);
    QVERIFY(!rig.run.finished);
    QCOMPARE(int(rig.run.outcome), outcome);
    QVERIFY(!rig.run.message.isEmpty());
    if (mode == "oom")
        QVERIFY2(rig.run.message.toLower().contains("memory"), qPrintable(rig.run.message));
    QCOMPARE(upscaleCalls(rig.sb.fakeLog()).size(), calls);
    QCOMPARE(cacheEntries(rig.sb), QStringList());
    QVERIFY(!QFileInfo::exists(rig.keepFolder()) || rig.keptFiles().isEmpty());
    QCOMPARE(filesUnder(rig.sb.library()).size(), 2); // nothing filed
    QVERIFY(!rig.keeper->isRunning());
}

void LimgenTests::keeper_aFileThatAppearedMeanwhileIsNeverOverwritten() {
    KeepRig rig;
    QString message;
    QVERIFY(rig.prepare(rig.settings(), &message) == Keeper::Outcome::Ok);
    // Another limgen kept the same draft after prepare() looked.
    const QString target = rig.keeper->result().installed;
    QVERIFY(writeFile(target, "someone else's"));
    rig.keeper->start();
    WAIT_FOR_KEEPER(rig);
    QVERIFY(rig.run.failed);
    QCOMPARE(int(rig.run.outcome), int(Keeper::Outcome::Failed));
    QVERIFY2(rig.run.message.contains(target), qPrintable(rig.run.message));
    QCOMPARE(readFile(target), QByteArray("someone else's"));
    QCOMPARE(rig.keptFiles().size(), 1); // no .part left beside it
    QCOMPARE(cacheEntries(rig.sb), QStringList());
}

void LimgenTests::keeper_noThemeGoesToThePicturesFolder() {
    KeepRig rig;
    rig.draft = rig.makeDraft("a quiet harbour", QString());
    KeepSettings settings = rig.settings(QString());
    settings.draftId = rig.draft.id;
    KEEP_AND_WAIT(rig, settings);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    const QString name = Keeper::fileName("a quiet harbour", rig.draft.id, "png");
    QCOMPARE(rig.run.result.installed, rig.sb.fixture.pictures() + "/" + name);
    QVERIFY(QFileInfo(rig.run.result.installed).isFile());
    QCOMPARE(rig.run.result.theme, QString());
    // Omarchy's backgrounds are not touched.
    QCOMPARE(filesUnder(rig.sb.fixture.backgrounds), QStringList());
    const QJsonObject finish = rig.run.result.final.metadata["finish"].toObject();
    QVERIFY(finish["theme"].isNull());
    QVERIFY(finish["colorTarget"].isNull());
    QVERIFY(rig.run.result.final.metadata["theme"].isNull());
}

void LimgenTests::keeper_withoutAMonitorTheFinalIs2560x1440() {
    KeepRig rig;
    OmarchyPaths paths = rig.sb.paths();
    paths.hyprctl = QString(); // no Hyprland to ask
    rig.remake(paths);
    KeepSettings settings = rig.settings();
    settings.size = QSize();
    settings.colorPass = 0;
    KEEP_AND_WAIT(rig, settings);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(QImageReader(rig.run.result.installed).size(), QSize(2560, 1440));
    QCOMPARE(rig.run.result.final.metadata["size"].toArray()[0].toInt(), 2560);
}

void LimgenTests::keeper_keepFolderHoldsOnlyLimgenFiles() {
    KeepRig rig;
    // Two drafts, one as PNG and one as JPEG, into the same theme folder.
    KeepSettings png = rig.settings();
    KEEP_AND_WAIT(rig, png);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    rig.draft = rig.makeDraft("a lighthouse in fog");
    rig.remake();
    KeepSettings jpeg = rig.settings();
    jpeg.format = "jpeg";
    KEEP_AND_WAIT(rig, jpeg);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    const QStringList files = rig.keptFiles();
    QCOMPARE(files.size(), 2);
    for (const QString &file : files)
        QVERIFY2(file.startsWith("limgen-") && (file.endsWith(".png") || file.endsWith(".jpg")), qPrintable(file));
    QVERIFY(files[0].contains("lighthouse") != files[1].contains("lighthouse"));
    QCOMPARE(cacheEntries(rig.sb), QStringList());
}

// ============================== L5: CLI keep and set ======================

namespace {

// A fixture with the upscaler installed, themes "night-city" and "other", a
// 64x48 draft made for night-city in the library the CLI reads, and the
// environment that points limgen at all of it (and at a 320x180 monitor).
struct CliKeep {
    Fixture fixture;
    Catalogue catalogue = Catalogue::builtIn();
    LibraryEntry draft;
    QHash<QString, QString> env;

    explicit CliKeep(const QString &currentTheme = "night-city") {
        makeSparse(Catalogue::path(*catalogue.upscaler(), fixture.models), catalogue.upscaler()->bytes);
        fixture.addTheme("night-city", stockToml);
        fixture.addTheme("other", tomlWith("#fafafa", "#111111", "#ff0000"));
        if (!currentTheme.isEmpty())
            fixture.switchTo(currentTheme);
        env = engineEnvironment(fixture);
        draft = makeDraft();
    }
    QString libraryDir() const { return fixture.data + "/limgen/library"; }
    LibraryEntry makeDraft(const QString &subject = "oil rig at dusk", const QString &slug = "night-city") {
        Library library(libraryDir());
        DraftRecord record = fullRecord();
        record.subject = subject;
        record.themeSlug = slug;
        if (slug.isEmpty()) {
            record.themeMode.clear();
            record.palette = QJsonObject();
        }
        record.width = 64;
        record.height = 48;
        return library.addDraft(makePng(fixture.root.path() + "/src-" + QString::number(QRandomGenerator::global()->generate()) + ".png",
                                        64, 48),
                                record, QDateTime::currentDateTimeUtc());
    }
    QString keepPath(const QString &folder, const QString &format = "png") const {
        return folder + "/" + Keeper::fileName("oil rig at dusk", draft.id, format);
    }
    QString inTheme(const QString &theme) const { return fixture.backgrounds + "/" + theme; }
    QString log() const { return fixture.root.path() + "/fake.log"; }
    int libraryFiles() const { return filesUnder(libraryDir()).size(); }
    int upscales() const { return upscaleCalls(log()).size(); }
    QJsonObject sidecarOf(const QString &id) const { return Library(libraryDir()).find(id).metadata; }
};

} // namespace

void LimgenTests::cli_keepGoesToTheThemeTheDraftWasMadeFor() {
    REQUIRE_BINARY();
    CliKeep rig("other"); // the theme has changed since the draft was made for night-city
    const CliResult result = runCli(rig.fixture, {"keep", rig.draft.id}, rig.env);
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 0);
    const QString expected = rig.keepPath(rig.inTheme("night-city"));
    QCOMPARE(result.out.trimmed(), expected);
    QCOMPARE(QImageReader(expected).size(), QSize(320, 180));
    QVERIFY2(result.err.contains(rig.draft.id), qPrintable(result.err)); // says what it is finishing
    QCOMPARE(rig.fixture.omarchyCalls(), QStringList()); // not set as the background
    QCOMPARE(filesUnder(rig.fixture.backgrounds), QStringList{expected});
    QCOMPARE(rig.libraryFiles(), 4); // the draft and its final, each with a sidecar
    QCOMPARE(filesUnder(rig.fixture.cache), QStringList());
    QCOMPARE(rig.upscales(), 1);
}

void LimgenTests::cli_keepThemeOptionOverridesAndChecksTheTheme() {
    REQUIRE_BINARY();
    CliKeep rig;
    CliResult result = runCli(rig.fixture, {"keep", rig.draft.id, "--theme", "other"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.out.trimmed(), rig.keepPath(rig.inTheme("other")));
    QVERIFY(QFileInfo(rig.keepPath(rig.inTheme("other"))).isFile());
    QCOMPARE(rig.sidecarOf(QFileInfo(Library(rig.libraryDir()).find(rig.draft.id).image).baseName()).value("kind").toString(), QString("draft"));
    // A theme that does not exist is a usage error: nothing is made.
    const int upscalesBefore = rig.upscales();
    result = runCli(rig.fixture, {"keep", rig.draft.id, "--theme", "nosuch"}, rig.env);
    QCOMPARE(result.exitCode, 2);
    QCOMPARE(result.out, QString());
    QVERIFY2(result.err.contains("nosuch"), qPrintable(result.err));
    QCOMPARE(rig.upscales(), upscalesBefore);
    QVERIFY(!QFileInfo::exists(rig.inTheme("nosuch")));
    result = runCli(rig.fixture, {"keep", rig.draft.id, "--theme", "../x"}, rig.env);
    QCOMPARE(result.exitCode, 2);
}

void LimgenTests::cli_keepDraftWithoutAThemeUsesTheCurrentThenPictures() {
    REQUIRE_BINARY();
    {
        CliKeep rig("other");
        rig.draft = rig.makeDraft("a quiet harbour", QString());
        const CliResult result = runCli(rig.fixture, {"keep", rig.draft.id}, rig.env);
        QCOMPARE(result.exitCode, 0);
        QCOMPARE(result.out.trimmed(), rig.inTheme("other") + "/" + Keeper::fileName("a quiet harbour", rig.draft.id, "png"));
    }
    {
        CliKeep rig("");
        QVERIFY(QFile::remove(rig.fixture.state + "/theme.name") || true);
        rig.draft = rig.makeDraft("a quiet harbour", QString());
        const CliResult result = runCli(rig.fixture, {"keep", rig.draft.id}, rig.env);
        QCOMPARE(result.exitCode, 0);
        QCOMPARE(result.out.trimmed(), rig.fixture.pictures() + "/" + Keeper::fileName("a quiet harbour", rig.draft.id, "png"));
        QCOMPARE(filesUnder(rig.fixture.backgrounds), QStringList());
    }
}

void LimgenTests::cli_keepJson() {
    REQUIRE_BINARY();
    CliKeep rig;
    const CliResult result = runCli(rig.fixture, {"keep", rig.draft.id, "--json"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.err, QString()); // JSON mode says nothing else
    const QJsonObject json = jsonObject(result.out);
    QCOMPARE(json.keys(), (QStringList{"alreadyKept", "draft", "final", "path", "set", "theme"}));
    QCOMPARE(json["draft"].toString(), rig.draft.id);
    QCOMPARE(json["path"].toString(), rig.keepPath(rig.inTheme("night-city")));
    QCOMPARE(json["theme"].toString(), QString("night-city"));
    QCOMPARE(json["alreadyKept"].toBool(), false);
    QCOMPARE(json["set"].toBool(), false);
    const QString finalId = json["final"].toString();
    QVERIFY(Library::isValidId(finalId));
    QCOMPARE(rig.sidecarOf(finalId)["kind"].toString(), QString("final"));
    QCOMPARE(rig.sidecarOf(finalId)["finish"].toObject()["from"].toString(), rig.draft.id);
}

void LimgenTests::cli_keepSetShowsItAndFailsWhenOmarchyFails() {
    REQUIRE_BINARY();
    CliKeep rig;
    CliResult result = runCli(rig.fixture, {"keep", rig.draft.id, "--set", "--json"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    const QString path = rig.keepPath(rig.inTheme("night-city"));
    QCOMPARE(jsonObject(result.out)["set"].toBool(), true);
    QCOMPARE(rig.fixture.omarchyCalls(), QStringList{"theme bg set " + path});
    QCOMPARE(QFileInfo(rig.fixture.state + "/background").symLinkTarget(), path);
    // Omarchy fails: exit 1 with its message, and the image is kept all the same.
    CliKeep failing;
    QHash<QString, QString> env = failing.env;
    env["FAKE_OMARCHY_FAIL"] = "1";
    result = runCli(failing.fixture, {"keep", failing.draft.id, "--set"}, env);
    QCOMPARE(result.exitCode, 1);
    QVERIFY2(result.err.contains("boom"), qPrintable(result.err));
    QVERIFY(QFileInfo(failing.keepPath(failing.inTheme("night-city"))).isFile());
}

void LimgenTests::cli_keepAlreadyKeptMakesNothingNew() {
    REQUIRE_BINARY();
    CliKeep rig;
    CliResult first = runCli(rig.fixture, {"keep", rig.draft.id}, rig.env);
    QCOMPARE(first.exitCode, 0);
    const int files = rig.libraryFiles();
    const QString path = rig.keepPath(rig.inTheme("night-city"));
    const QByteArray bytes = readFile(path);
    const CliResult again = runCli(rig.fixture, {"keep", rig.draft.id}, rig.env);
    QCOMPARE(again.exitCode, 0);
    QCOMPARE(again.out.trimmed(), path);
    QVERIFY(!again.err.isEmpty()); // says so
    QCOMPARE(rig.upscales(), 1);
    QCOMPARE(rig.libraryFiles(), files);
    QCOMPARE(readFile(path), bytes);
    const QJsonObject json = jsonObject(runCli(rig.fixture, {"keep", rig.draft.id, "--json"}, rig.env).out);
    QCOMPARE(json["alreadyKept"].toBool(), true);
    QVERIFY(json["final"].isNull());
    // Asking for JPEG now still finds the PNG.
    const CliResult jpeg = runCli(rig.fixture, {"keep", rig.draft.id, "--format", "jpeg"}, rig.env);
    QCOMPARE(jpeg.exitCode, 0);
    QCOMPARE(jpeg.out.trimmed(), path);
    QCOMPARE(rig.upscales(), 1);
    QCOMPARE(filesUnder(rig.fixture.backgrounds), QStringList{path});
}

void LimgenTests::cli_keepArgumentErrors_data() {
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("no id") << QStringList{"keep"};
    QTest::newRow("two ids") << QStringList{"keep", "@ID@", "@ID@"};
    QTest::newRow("colour pass not a number") << QStringList{"keep", "@ID@", "--color-pass", "x"};
    QTest::newRow("colour pass below 0") << QStringList{"keep", "@ID@", "--color-pass", "-0.1"};
    QTest::newRow("colour pass above 1") << QStringList{"keep", "@ID@", "--color-pass", "1.5"};
    QTest::newRow("colour pass nan") << QStringList{"keep", "@ID@", "--color-pass", "nan"};
    QTest::newRow("format bmp") << QStringList{"keep", "@ID@", "--format", "bmp"};
    QTest::newRow("format PNG in capitals") << QStringList{"keep", "@ID@", "--format", "PNG"};
    QTest::newRow("unknown option") << QStringList{"keep", "@ID@", "--bogus"};
    QTest::newRow("unknown draft") << QStringList{"keep", "2026-11-20-ffffff"};
    QTest::newRow("not an id") << QStringList{"keep", "nope"};
    QTest::newRow("a final, not a draft") << QStringList{"keep", "@FINAL@"};
    QTest::newRow("unknown theme") << QStringList{"keep", "@ID@", "--theme", "nosuch"};
}

void LimgenTests::cli_keepArgumentErrors() {
    REQUIRE_BINARY();
    QFETCH(QStringList, arguments);
    CliKeep rig;
    const QString png = rig.fixture.root.path() + "/final.png";
    QVERIFY(savePng(solidImage(QSize(8, 8), Qt::red), png));
    const LibraryEntry final = Library(rig.libraryDir()).addFinal(png, {{"subject", "x"}}, fixedTime);
    const int files = rig.libraryFiles();
    for (QString &argument : arguments)
        argument.replace("@ID@", rig.draft.id).replace("@FINAL@", final.id);
    const CliResult result = runCli(rig.fixture, arguments, rig.env);
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 2);
    QCOMPARE(result.out, QString());
    QVERIFY(!result.err.isEmpty());
    QCOMPARE(rig.upscales(), 0);
    QCOMPARE(filesUnder(rig.fixture.backgrounds), QStringList());
    QCOMPARE(rig.libraryFiles(), files);
}

void LimgenTests::cli_keepNotRunnableOrFailing_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("exitCode");
    QTest::newRow("upscaler not installed") << "no-upscaler" << 3;
    QTest::newRow("engine sees no GPU") << "no-gpu" << 3;
    QTest::newRow("another engine release") << "old-version" << 3;
    QTest::newRow("out of video memory") << "oom" << 3;
    QTest::newRow("the engine fails") << "fail" << 1;
    QTest::newRow("the engine writes the wrong size") << "wrong-size" << 1;
}

void LimgenTests::cli_keepNotRunnableOrFailing() {
    REQUIRE_BINARY();
    QFETCH(QString, mode);
    QFETCH(int, exitCode);
    CliKeep rig;
    QHash<QString, QString> env = rig.env;
    if (mode == "no-upscaler")
        QVERIFY(QFile::remove(Catalogue::path(*rig.catalogue.upscaler(), rig.fixture.models)));
    else
        env["FAKE_SD_CLI_MODE"] = mode;
    const int files = rig.libraryFiles();
    const CliResult result = runCli(rig.fixture, {"keep", rig.draft.id}, env);
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, exitCode);
    QCOMPARE(result.out, QString());
    QVERIFY(!result.err.isEmpty());
    if (mode == "no-upscaler")
        QVERIFY2(result.err.contains(rig.catalogue.upscaler()->name), qPrintable(result.err));
    QCOMPARE(filesUnder(rig.fixture.backgrounds), QStringList());
    QCOMPARE(rig.libraryFiles(), files);
    QCOMPARE(filesUnder(rig.fixture.cache), QStringList());
}

void LimgenTests::cli_keepTakesColourPassAndFormatFromTheConfigAndTheOptions() {
    REQUIRE_BINARY();
    CliKeep rig;
    QVERIFY(writeFile(rig.fixture.root.path() + "/config/limgen/config.json",
                      R"({"colorPass": 0, "format": "jpeg", "jpegQuality": 60})"));
    // From the settings: JPEG at quality 60 with the colour pass off.
    CliResult result = runCli(rig.fixture, {"keep", rig.draft.id, "--json"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    QJsonObject json = jsonObject(result.out);
    QVERIFY(json["path"].toString().endsWith(".jpg"));
    QJsonObject finish = rig.sidecarOf(json["final"].toString())["finish"].toObject();
    QCOMPARE(finish["format"].toString(), QString("jpeg"));
    QCOMPARE(finish["quality"].toInt(), 60);
    QCOMPARE(finish["colorPass"].toDouble(), 0.0);
    QVERIFY(finish["colorTarget"].isNull());
    // The options win over the settings.
    const LibraryEntry second = rig.makeDraft("a lighthouse in fog");
    result = runCli(rig.fixture, {"keep", second.id, "--format", "png", "--color-pass", "0.5", "--json"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    json = jsonObject(result.out);
    QVERIFY(json["path"].toString().endsWith(".png"));
    finish = rig.sidecarOf(json["final"].toString())["finish"].toObject();
    QCOMPARE(finish["format"].toString(), QString("png"));
    QVERIFY(!finish.contains("quality"));
    QCOMPARE(finish["colorPass"].toDouble(), 0.5);
    QCOMPARE(finish["colorTarget"].toString(), QString("palette"));
}

void LimgenTests::cli_keepInterruptedExits130AndKeepsNothing() {
    REQUIRE_BINARY();
    CliKeep rig;
    QHash<QString, QString> env = rig.env;
    env["FAKE_SD_CLI_MODE"] = "hang";
    const int files = rig.libraryFiles();
    auto process = startProcess(cliEnvironment(rig.fixture, env), qEnvironmentVariable("LIMGEN_BINARY"),
                                {"keep", rig.draft.id});
    QTRY_VERIFY_WITH_TIMEOUT(rig.upscales() > 0, 20000);
    QTest::qWait(300);
    QCOMPARE(process->state(), QProcess::Running);
    QVERIFY(::kill(pid_t(process->processId()), SIGINT) == 0);
    WAIT_FOR_PROCESS(process);
    QCOMPARE(process->exitStatus(), QProcess::NormalExit);
    QCOMPARE(process->exitCode(), 130);
    QCOMPARE(QString::fromUtf8(process->readAllStandardOutput()), QString());
    QVERIFY(QString::fromUtf8(process->readAllStandardError()).contains("Interrupted"));
    QCOMPARE(filesUnder(rig.fixture.backgrounds), QStringList());
    QCOMPARE(rig.libraryFiles(), files);
    QCOMPARE(filesUnder(rig.fixture.cache), QStringList());
    QCOMPARE(QProcess::execute("pgrep", {"-f", rig.fixture.cache}), 1); // the engine was stopped too
}

void LimgenTests::cli_setAFile() {
    REQUIRE_BINARY();
    CliKeep rig;
    const QString file = rig.fixture.root.path() + "/pics/mine.png";
    QVERIFY(savePng(solidImage(QSize(16, 9), Qt::green), file));
    CliResult result = runCli(rig.fixture, {"set", file}, rig.env);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.out.trimmed(), file);
    QCOMPARE(rig.fixture.omarchyCalls(), QStringList{"theme bg set " + file});
    QCOMPARE(rig.upscales(), 0); // a plain file is not finished
    result = runCli(rig.fixture, {"set", file, "--json"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    const QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["path"].toString(), file);
    QCOMPARE(json["set"].toBool(), true);
    // A relative path is made absolute for Omarchy.
    const QString relative = QFileInfo(file).fileName();
    QProcessEnvironment environment = cliEnvironment(rig.fixture, rig.env);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(QFileInfo(file).absolutePath());
    process.start(qEnvironmentVariable("LIMGEN_BINARY"), {"set", relative});
    QVERIFY(process.waitForFinished(20000));
    QCOMPARE(process.exitCode(), 0);
    QCOMPARE(rig.fixture.omarchyCalls().last(), "theme bg set " + file);
}

void LimgenTests::cli_setADraftKeepsItIntoTheCurrentTheme() {
    REQUIRE_BINARY();
    CliKeep rig("other"); // the draft was made for night-city
    const CliResult result = runCli(rig.fixture, {"set", rig.draft.id, "--json"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    const QJsonObject json = jsonObject(result.out);
    const QString path = rig.keepPath(rig.inTheme("other"));
    QCOMPARE(json["path"].toString(), path);
    QCOMPARE(json["theme"].toString(), QString("other"));
    QCOMPARE(json["draft"].toString(), rig.draft.id);
    QVERIFY(Library::isValidId(json["final"].toString()));
    QCOMPARE(json["alreadyKept"].toBool(), false);
    QCOMPARE(json["set"].toBool(), true);
    QCOMPARE(rig.fixture.omarchyCalls(), QStringList{"theme bg set " + path});
    QCOMPARE(QFileInfo(rig.fixture.state + "/background").symLinkTarget(), path);
    QVERIFY(!QFileInfo::exists(rig.inTheme("night-city"))); // not into the theme it was made for
    // Text mode prints the path.
    CliKeep text("other");
    const CliResult plain = runCli(text.fixture, {"set", text.draft.id}, text.env);
    QCOMPARE(plain.exitCode, 0);
    QCOMPARE(plain.out.trimmed(), text.keepPath(text.inTheme("other")));
}

void LimgenTests::cli_setAFinalGoesThroughItsDraft() {
    REQUIRE_BINARY();
    CliKeep rig; // current theme night-city
    const QJsonObject kept = jsonObject(runCli(rig.fixture, {"keep", rig.draft.id, "--json"}, rig.env).out);
    const QString finalId = kept["final"].toString();
    QVERIFY(Library::isValidId(finalId));
    const int files = rig.libraryFiles();
    // Same theme: the draft is already there, so nothing new is made, and it is set.
    CliResult result = runCli(rig.fixture, {"set", finalId, "--json"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["alreadyKept"].toBool(), true);
    QCOMPARE(json["path"].toString(), kept["path"].toString());
    QCOMPARE(rig.libraryFiles(), files);
    QCOMPARE(rig.upscales(), 1);
    QCOMPARE(rig.fixture.omarchyCalls(), QStringList{"theme bg set " + kept["path"].toString()});
    // Another theme: its draft is kept into the current one.
    QVERIFY(rig.fixture.switchTo("other"));
    result = runCli(rig.fixture, {"set", finalId, "--json"}, rig.env);
    QCOMPARE(result.exitCode, 0);
    json = jsonObject(result.out);
    QCOMPARE(json["alreadyKept"].toBool(), false);
    QCOMPARE(json["path"].toString(), rig.keepPath(rig.inTheme("other")));
    QCOMPARE(rig.upscales(), 2);
}

void LimgenTests::cli_setAFinalWhoseDraftIsGone() {
    REQUIRE_BINARY();
    CliKeep rig;
    const QJsonObject kept = jsonObject(runCli(rig.fixture, {"keep", rig.draft.id, "--json"}, rig.env).out);
    const QString finalId = kept["final"].toString();
    QVERIFY(Library::isValidId(finalId));
    const LibraryEntry draft = Library(rig.libraryDir()).find(rig.draft.id);
    QVERIFY(QFile::remove(draft.image) && QFile::remove(draft.sidecar));
    const int calls = rig.fixture.omarchyCalls().size();
    const CliResult result = runCli(rig.fixture, {"set", finalId}, rig.env);
    // The id was right; what is missing is its draft, and the message says
    // where the final itself is.
    QCOMPARE(result.exitCode, 1);
    const LibraryEntry final = Library(rig.libraryDir()).find(finalId);
    QVERIFY2(result.err.contains(final.image), qPrintable(result.err));
    QCOMPARE(rig.fixture.omarchyCalls().size(), calls);
}

void LimgenTests::cli_setErrors() {
    REQUIRE_BINARY();
    CliKeep rig;
    CliResult result = runCli(rig.fixture, {"set"}, rig.env);
    QCOMPARE(result.exitCode, 2);
    result = runCli(rig.fixture, {"set", "2026-11-20-ffffff"}, rig.env); // an id that is not in the library
    QCOMPARE(result.exitCode, 2);
    QCOMPARE(result.out, QString());
    result = runCli(rig.fixture, {"set", rig.fixture.root.path() + "/no-such-file.png"}, rig.env);
    QCOMPARE(result.exitCode, 2);
    result = runCli(rig.fixture, {"set", "a", "b"}, rig.env);
    QCOMPARE(result.exitCode, 2);
    QCOMPARE(rig.fixture.omarchyCalls(), QStringList()); // nothing reached Omarchy
    // Omarchy fails, or is not there.
    const QString file = rig.fixture.root.path() + "/mine.png";
    QVERIFY(savePng(solidImage(QSize(16, 9), Qt::green), file));
    QHash<QString, QString> failing = rig.env;
    failing["FAKE_OMARCHY_FAIL"] = "1";
    result = runCli(rig.fixture, {"set", file}, failing);
    QCOMPARE(result.exitCode, 1);
    QVERIFY2(result.err.contains("boom"), qPrintable(result.err));
    QCOMPARE(result.out, QString());
    QHash<QString, QString> missing = rig.env;
    missing["LIMGEN_OMARCHY_BIN"] = rig.fixture.root.path() + "/no-such-omarchy";
    result = runCli(rig.fixture, {"set", file}, missing);
    QCOMPARE(result.exitCode, 1);
    // A draft needs the upscaler: without it, exit 3 and Omarchy is not called.
    QVERIFY(QFile::remove(Catalogue::path(*rig.catalogue.upscaler(), rig.fixture.models)));
    const int calls = rig.fixture.omarchyCalls().size();
    result = runCli(rig.fixture, {"set", rig.draft.id}, rig.env);
    QCOMPARE(result.exitCode, 3);
    QCOMPARE(rig.fixture.omarchyCalls().size(), calls);
}

void LimgenTests::cli_helpListsKeepAndSet() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    const CliResult summary = runCli(fixture, {"help"});
    QCOMPARE(summary.exitCode, 0);
    QVERIFY(summary.out.contains("keep"));
    QVERIFY(summary.out.contains("set"));
    const CliResult keep = runCli(fixture, {"help", "keep"});
    QCOMPARE(keep.exitCode, 0);
    for (const QString &option : QStringList{"--theme", "--color-pass", "--format", "--set", "--json"})
        QVERIFY2(keep.out.contains(option), qPrintable(option));
    const CliResult set = runCli(fixture, {"help", "set"});
    QCOMPARE(set.exitCode, 0);
    QVERIFY(set.out.contains("--json"));
}

// ============================== L5: Backend, keeping ======================

namespace {

// A Backend on the small test catalogue (model "m": 64x64 drafts, one step),
// with the model and the upscaler installed, the theme "night-city" current
// and "other" available, and a 320x180 monitor, so batches and keeps are quick.
struct KeepBackendRig {
    Sandbox sb;
    World world;
    std::unique_ptr<Omarchy> omarchy;
    std::unique_ptr<Backend> backend;

    // `steps` is the model's step count: variations need a few to run fewer of.
    explicit KeepBackendRig(bool theme = true, int steps = 1) : world(makeWorld(steps)) {
        const QString catalogueFile = sb.fixture.root.path() + "/catalogue.json";
        writeFile(catalogueFile, world.json);
        qputenv("LIMGEN_CATALOGUE", catalogueFile.toUtf8());
        for (const QString &key : QStringList{"a", "c", "b", "u"})
            makeSparse(Catalogue::path(world.file(key), sb.fixture.models), world.file(key).bytes);
        if (theme) {
            stockTheme(sb.fixture);
            sb.fixture.addTheme("other", tomlWith("#fafafa", "#111111", "#ff0000"));
        }
    }
    ~KeepBackendRig() {
        settleBackend(backend.get());
        backend.reset();
        omarchy.reset();
    }
    void create(const QString &engine = fakeEngine) {
        settleBackend(backend.get());
        backend.reset();
        omarchy = std::make_unique<Omarchy>(sb.paths());
        backend = std::make_unique<Backend>(omarchy.get(), engine);
        for (int i = 0; i < 400 && !(backend->engineReady() || !backend->engineProblem().isEmpty()); ++i)
            QTest::qWait(50);
    }
    QVariantMap draft(int index) const { return backend->drafts().value(index).toMap(); }
    // Makes a batch and waits for it; true when all four drafts are done.
    bool makeBatch(const QString &subject = "oil rig") {
        backend->setSubject(subject);
        backend->generate();
        for (int i = 0; i < 800 && (backend->running() || backend->batchWaiting() || backend->drafts().isEmpty()); ++i)
            QTest::qWait(50);
        if (backend->drafts().size() != 4)
            return false;
        for (int i = 0; i < 4; ++i)
            if (draft(i)["state"].toString() != "done")
                return false;
        return true;
    }
    QString keepDir(const QString &theme = "night-city") const { return sb.fixture.backgrounds + "/" + theme; }
    QString nameFor(int index, const QString &subject = "oil rig") const {
        return keepDir() + "/" + Keeper::fileName(subject, draft(index)["id"].toString(), "png");
    }
    QStringList calls() const { return sb.fixture.omarchyCalls(); }
    Library library() const { return Library(sb.library()); }
    int upscalesDone() const { return upscaleCalls(sb.fakeLog()).size(); }
};

#define WAIT_KEPT(rig, i) QTRY_VERIFY_WITH_TIMEOUT((rig).draft(i)["kept"].toBool(), 30000)
#define WAIT_NOT_KEEPING(rig) QTRY_VERIFY_WITH_TIMEOUT(!(rig).backend->keeping(), 30000)

QStringList binFiles(const Sandbox &sb) { return QDir(sb.library() + "/.discarded").entryList(QDir::Files, QDir::Name); }

} // namespace

void LimgenTests::backend_keepShowsTileStatesAndInstallsTheImage() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    QCOMPARE(rig.draft(0)["theme"].toString(), QString("night-city"));
    QVERIFY(!rig.backend->canUndo());
    QList<double> fractions;
    QObject::connect(rig.backend.get(), &Backend::draftsChanged, rig.backend.get(),
                     [&] { fractions << rig.draft(0)["keepFraction"].toDouble(); });
    rig.backend->keep(0);
    // At once: waiting its turn or started; the tile says it is being kept.
    QVERIFY(rig.draft(0)["keeping"].toBool());
    QVERIFY(rig.backend->keeping());
    QVERIFY(rig.backend->busy());
    QVERIFY(rig.backend->canUndo());
    WAIT_KEPT(rig, 0);
    QVERIFY(!rig.draft(0)["keeping"].toBool());
    QVERIFY(!rig.draft(0)["set"].toBool());
    QTRY_VERIFY(!rig.backend->keeping());
    const QString path = rig.draft(0)["keptPath"].toString();
    QCOMPARE(path, rig.nameFor(0));
    QCOMPARE(QImageReader(path).size(), QSize(320, 180));
    QVERIFY2(rig.backend->status().contains("Kept as"), qPrintable(rig.backend->status()));
    QVERIFY(!rig.backend->statusIsError());
    // The fraction went from waiting (-1) to a figure from 0 to 1.
    QVERIFY2(fractions.contains(-1.0) || fractions.contains(0.0), "the tile showed waiting or started");
    QVERIFY(std::any_of(fractions.begin(), fractions.end(), [](double f) { return f >= 0 && f <= 1; }));
    // Only this draft was touched; nothing was set as the background.
    for (int i = 1; i < 4; ++i)
        QVERIFY(!rig.draft(i)["kept"].toBool() && !rig.draft(i)["keeping"].toBool());
    QCOMPARE(rig.calls(), QStringList());
    QCOMPARE(filesUnder(rig.sb.fixture.backgrounds), QStringList{path});
    QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
    // The final is in the library beside the draft's batch (4 drafts and 1 final, with sidecars).
    QCOMPARE(filesUnder(rig.sb.library()).size(), 10);
}

void LimgenTests::backend_keepAndSetSetsTheBackgroundOnce() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    rig.backend->keepAndSet(1);
    WAIT_KEPT(rig, 1);
    const QString path = rig.draft(1)["keptPath"].toString();
    QVERIFY(rig.draft(1)["set"].toBool());
    QCOMPARE(rig.calls(), QStringList{"theme bg set " + path});
    QCOMPARE(rig.omarchy->currentBackground(), path);
    QVERIFY2(rig.backend->status().contains("background"), qPrintable(rig.backend->status()));
    // K on a kept draft does nothing; S on one that is already the background neither.
    const int files = filesUnder(rig.sb.library()).size();
    rig.backend->keep(1);
    rig.backend->keepAndSet(1);
    QTest::qWait(300);
    QCOMPARE(rig.calls().size(), 1);
    QCOMPARE(filesUnder(rig.sb.library()).size(), files);
    QVERIFY(!rig.backend->keeping());
    // K first, S later: S shows the kept image without keeping it again.
    rig.backend->keep(2);
    WAIT_KEPT(rig, 2);
    QVERIFY(!rig.draft(2)["set"].toBool());
    QCOMPARE(rig.calls().size(), 1);
    const int afterK = filesUnder(rig.sb.library()).size();
    rig.backend->keepAndSet(2);
    QTRY_VERIFY(rig.draft(2)["set"].toBool());
    QCOMPARE(rig.calls().size(), 2);
    QCOMPARE(rig.calls().last(), "theme bg set " + rig.draft(2)["keptPath"].toString());
    QCOMPARE(filesUnder(rig.sb.library()).size(), afterK);
    QCOMPARE(rig.upscalesDone(), 2);
}

void LimgenTests::backend_keepOnlyAppliesToDoneDrafts() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    for (const int index : {-1, 4, 99}) {
        rig.backend->keep(index);
        rig.backend->keepAndSet(index);
        rig.backend->discard(index);
    }
    QVERIFY(!rig.backend->keeping());
    QVERIFY(!rig.backend->canUndo());
    // Drafts that are still being made: a new batch that hangs.
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.backend->setSubject("a lighthouse");
    rig.backend->generate();
    QVERIFY(rig.backend->running());
    QTRY_VERIFY_WITH_TIMEOUT(imageCalls(rig.sb.fakeLog()).size() >= 2, 15000);
    rig.backend->keep(0);
    rig.backend->keepAndSet(0);
    rig.backend->discard(0);
    QVERIFY(!rig.backend->keeping());
    QVERIFY(!rig.backend->canUndo());
    QVERIFY(rig.draft(0)["state"].toString() != "done");
    rig.backend->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running(), 15000);
}

void LimgenTests::backend_themeChoiceWhenTheThemeChangedThenKeepInto() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    QCOMPARE(rig.draft(0)["theme"].toString(), QString("night-city"));
    QVERIFY(rig.sb.fixture.switchTo("other"));
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->themeName(), QString("other"), 5000);
    QSignalSpy asked(rig.backend.get(), &Backend::themeChoiceNeeded);
    rig.backend->keep(0);
    // Nothing is made until the user chooses; the window is asked.
    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.at(0).at(0).toInt(), 0);
    QCOMPARE(asked.at(0).at(1).toString(), QString("night-city"));
    QCOMPARE(asked.at(0).at(2).toString(), QString("other"));
    QCOMPARE(asked.at(0).at(3).toBool(), false);
    QVERIFY(!rig.backend->keeping());
    QVERIFY(!rig.draft(0)["keeping"].toBool());
    QCOMPARE(rig.upscalesDone(), 0);
    rig.backend->keepAndSet(1);
    QCOMPARE(asked.count(), 2);
    QCOMPARE(asked.at(1).at(3).toBool(), true); // S asked, so the answer is set
    // The answers: into the theme it was made for, and into the current one (and set).
    rig.backend->keepInto(0, "night-city", false);
    rig.backend->keepInto(1, "other", true);
    WAIT_KEPT(rig, 0);
    WAIT_KEPT(rig, 1);
    QCOMPARE(rig.draft(0)["keptPath"].toString(), rig.nameFor(0));
    QCOMPARE(rig.draft(1)["keptPath"].toString(), rig.keepDir("other") + "/" + Keeper::fileName("oil rig", rig.draft(1)["id"].toString(), "png"));
    QVERIFY(QFileInfo(rig.draft(0)["keptPath"].toString()).isFile());
    QVERIFY(QFileInfo(rig.draft(1)["keptPath"].toString()).isFile());
    QVERIFY(!rig.draft(0)["set"].toBool());
    QVERIFY(rig.draft(1)["set"].toBool());
    QCOMPARE(rig.calls(), QStringList{"theme bg set " + rig.draft(1)["keptPath"].toString()});
    // The finals say where they went.
    QCOMPARE(rig.library().find(rig.draft(1)["id"].toString()).metadata["kind"].toString(), QString("draft"));
    // When the theme is the one it was made for, there is no question.
    QVERIFY(rig.sb.fixture.switchTo("night-city"));
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->themeName(), QString("night-city"), 5000);
    rig.backend->keep(2);
    QCOMPARE(asked.count(), 2);
    WAIT_KEPT(rig, 2);
}

void LimgenTests::backend_discardUndoAndRedo() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    const QString id0 = rig.draft(0)["id"].toString(), id1 = rig.draft(1)["id"].toString();
    const QByteArray image0 = readFile(rig.library().find(id0).image);
    rig.backend->discard(0);
    QVERIFY(rig.draft(0)["discarded"].toBool());
    QVERIFY(rig.backend->canUndo());
    QVERIFY(!rig.backend->canRedo());
    QVERIFY(!rig.library().find(id0).isValid()); // moved out of the library
    QVERIFY(binFiles(rig.sb).contains(id0 + ".png"));
    rig.backend->discard(0); // twice: one undo step only
    rig.backend->keep(0);    // and a discarded draft cannot be kept
    QVERIFY(!rig.backend->keeping());
    rig.backend->undo();
    QVERIFY(!rig.draft(0)["discarded"].toBool());
    QVERIFY(rig.library().find(id0).isValid());
    QCOMPARE(readFile(rig.library().find(id0).image), image0);
    QVERIFY(!rig.backend->canUndo());
    QVERIFY(rig.backend->canRedo());
    QVERIFY(binFiles(rig.sb).isEmpty());
    rig.backend->undo(); // nothing left to undo
    QVERIFY(!rig.draft(0)["discarded"].toBool());
    rig.backend->redo();
    QVERIFY(rig.draft(0)["discarded"].toBool());
    QVERIFY(!rig.library().find(id0).isValid());
    QVERIFY(rig.backend->canUndo());
    QVERIFY(!rig.backend->canRedo());
    // Two discards undo in reverse order; a new action clears what could be redone.
    rig.backend->discard(1);
    rig.backend->undo();
    QVERIFY(!rig.draft(1)["discarded"].toBool());
    QVERIFY(rig.draft(0)["discarded"].toBool());
    QVERIFY(rig.backend->canRedo());
    rig.backend->discard(2);
    QVERIFY(!rig.backend->canRedo());
    Q_UNUSED(id1);
    // A kept draft cannot be discarded.
    rig.backend->keep(3);
    WAIT_KEPT(rig, 3);
    rig.backend->discard(3);
    QVERIFY(!rig.draft(3)["discarded"].toBool());
    QVERIFY(rig.library().find(rig.draft(3)["id"].toString()).isValid());
}

void LimgenTests::backend_undoAndRedoOfAKeep() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    // Omarchy shows an earlier background.
    const QString previous = rig.sb.fixture.root.path() + "/previous.png";
    QVERIFY(writeFile(previous, "previous"));
    QVERIFY(QFile::link(previous, rig.sb.fixture.state + "/background"));
    rig.backend->keepAndSet(0);
    WAIT_KEPT(rig, 0);
    const QString installed = rig.draft(0)["keptPath"].toString();
    const QByteArray bytes = readFile(installed);
    QCOMPARE(rig.omarchy->currentBackground(), installed);
    const int libraryFiles = filesUnder(rig.sb.library()).size();
    // Undo: the file goes, the earlier background comes back, the final stays in the library.
    rig.backend->undo();
    QVERIFY(!QFileInfo::exists(installed));
    QVERIFY(!rig.draft(0)["kept"].toBool());
    QVERIFY(!rig.draft(0)["set"].toBool());
    QCOMPARE(rig.draft(0)["keptPath"].toString(), QString());
    QCOMPARE(rig.omarchy->currentBackground(), previous);
    QCOMPARE(rig.calls(), (QStringList{"theme bg set " + installed, "theme bg set " + previous}));
    QCOMPARE(filesUnder(rig.sb.library()).size(), libraryFiles);
    QVERIFY(!rig.backend->canUndo());
    QVERIFY(rig.backend->canRedo());
    // Redo: the same file is copied back and shown again.
    rig.backend->redo();
    QVERIFY(QFileInfo(installed).isFile());
    QCOMPARE(readFile(installed), bytes);
    QVERIFY(rig.draft(0)["kept"].toBool());
    QVERIFY(rig.draft(0)["set"].toBool());
    QCOMPARE(rig.draft(0)["keptPath"].toString(), installed);
    QCOMPARE(rig.omarchy->currentBackground(), installed);
    QVERIFY(rig.backend->canUndo());
    QVERIFY(!rig.backend->canRedo());
    QCOMPARE(QDir(rig.keepDir()).entryList(QDir::Files).size(), 1); // no .part
    // A keep without set: undo does not call Omarchy at all.
    const int calls = rig.calls().size();
    rig.backend->keep(1);
    WAIT_KEPT(rig, 1);
    rig.backend->undo();
    QVERIFY(!QFileInfo::exists(rig.draft(1)["keptPath"].toString()) && !rig.draft(1)["kept"].toBool());
    QCOMPARE(rig.calls().size(), calls);
    // A draft kept before is not this keep's file to remove.
    const QString mine = rig.nameFor(2);
    QVERIFY(writeFile(mine, "kept before"));
    rig.backend->keep(2);
    WAIT_KEPT(rig, 2);
    QVERIFY2(rig.backend->status().contains("Already kept"), qPrintable(rig.backend->status()));
    QCOMPARE(readFile(mine), QByteArray("kept before"));
    const bool undoable = rig.backend->canUndo();
    if (undoable)
        rig.backend->undo();
    QVERIFY(QFileInfo::exists(mine)); // never deleted
}

void LimgenTests::backend_undoWhileKeepingCancelsIt() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.backend->keep(0);
    QTRY_VERIFY_WITH_TIMEOUT(rig.upscalesDone() > 0, 15000);
    QTest::qWait(300);
    QVERIFY(rig.backend->keeping());
    rig.backend->undo();
    WAIT_NOT_KEEPING(rig);
    QVERIFY(!rig.draft(0)["keeping"].toBool());
    QVERIFY(!rig.draft(0)["kept"].toBool());
    QVERIFY(!QFileInfo::exists(rig.keepDir()) || QDir(rig.keepDir()).entryList(QDir::Files).isEmpty());
    QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
    QVERIFY(rig.backend->canRedo());
    QVERIFY(!rig.backend->canUndo());
    // Redo keeps it again, now that the engine works.
    qunsetenv("FAKE_SD_CLI_MODE");
    rig.backend->redo();
    QVERIFY(rig.draft(0)["keeping"].toBool());
    WAIT_KEPT(rig, 0);
    QVERIFY(QFileInfo(rig.draft(0)["keptPath"].toString()).isFile());
    QVERIFY(rig.backend->canUndo());
}

void LimgenTests::backend_undoThenRedoAtOnceKeepsTheTileKeeping() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.backend->keep(0);
    QTRY_VERIFY_WITH_TIMEOUT(rig.upscalesDone() > 0, 15000);
    QTest::qWait(200);
    // Undo, then redo before the cancelled engine has ended: the late report
    // of the cancelled keep must not reset the tile of the new one.
    rig.backend->undo();
    qunsetenv("FAKE_SD_CLI_MODE");
    rig.backend->redo();
    QElapsedTimer timer;
    timer.start();
    while (!rig.draft(0)["kept"].toBool() && timer.elapsed() < 20000) {
        QVERIFY2(rig.draft(0)["keeping"].toBool(), "the tile looked idle while its keep was queued or running");
        QTest::qWait(20);
    }
    QVERIFY(rig.draft(0)["kept"].toBool());
    QVERIFY(!rig.draft(0)["keeping"].toBool());
}

void LimgenTests::backend_undoOfTheOnlyBackgroundIsRefused() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    // No earlier background to go back to.
    QVERIFY(!QFileInfo(rig.sb.fixture.state + "/background").isSymLink());
    rig.backend->keepAndSet(0);
    WAIT_KEPT(rig, 0);
    const QString installed = rig.draft(0)["keptPath"].toString();
    QCOMPARE(rig.omarchy->currentBackground(), installed);
    rig.backend->undo();
    // Removing it would leave the desktop pointing at nothing, so it stays.
    QVERIFY(QFileInfo(installed).isFile());
    QVERIFY(rig.draft(0)["kept"].toBool());
    QVERIFY(rig.backend->statusIsError());
    QVERIFY(rig.backend->canUndo());
    QVERIFY(!rig.backend->canRedo());
}

void LimgenTests::backend_generateWhileKeepingWaitsThenStarts() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    const QString firstId = rig.draft(0)["id"].toString();
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.backend->keep(0);
    QTRY_VERIFY_WITH_TIMEOUT(rig.upscalesDone() > 0, 15000);
    rig.backend->setSubject("a lighthouse in fog");
    QVERIFY(rig.backend->canGenerate()); // a keep does not stop a new batch asking
    rig.backend->generate();
    QVERIFY(rig.backend->batchWaiting());
    QVERIFY(!rig.backend->running());
    QVERIFY(rig.backend->busy());
    QVERIFY(!rig.backend->canGenerate()); // already asked
    QTest::qWait(300);
    QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 1); // only the first batch ever ran
    QCOMPARE(rig.draft(0)["id"].toString(), firstId); // the tiles are still the first batch's
    // The keep ends (undone), and the batch starts by itself; the engine works again.
    qunsetenv("FAKE_SD_CLI_MODE");
    rig.backend->undo();
    QTRY_VERIFY_WITH_TIMEOUT(rig.backend->running() || imageCalls(rig.sb.fakeLog()).size() == 2, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running() && !rig.backend->batchWaiting(), 40000);
    QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 2);
    QCOMPARE(rig.draft(0)["state"].toString(), QString("done"));
    QVERIFY(rig.draft(0)["id"].toString() != firstId);
    QVERIFY(!rig.backend->canUndo()); // a new batch starts with a clean undo
    QVERIFY(!rig.backend->canRedo());
    QCOMPARE(rig.library().find(rig.draft(0)["id"].toString()).metadata["subject"].toString(), QString("a lighthouse in fog"));
}

void LimgenTests::backend_cancelOfAWaitingBatchAndStopAll() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.backend->keep(0);
    rig.backend->keep(1); // queued behind the first
    QTRY_VERIFY_WITH_TIMEOUT(rig.upscalesDone() > 0, 15000);
    rig.backend->generate();
    QVERIFY(rig.backend->batchWaiting());
    // Cancel (Esc) ends the waiting, not the keeps.
    rig.backend->cancel();
    QVERIFY(!rig.backend->batchWaiting());
    QVERIFY2(rig.backend->status().toLower().contains("cancelled"), qPrintable(rig.backend->status()));
    QVERIFY(rig.backend->keeping());
    QTest::qWait(300);
    QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 1); // the batch never started
    // stopAll (before quitting) ends everything: waiting batch, queue and the running keep.
    rig.backend->generate();
    QVERIFY(rig.backend->batchWaiting());
    rig.backend->stopAll();
    QVERIFY(!rig.backend->batchWaiting());
    WAIT_NOT_KEEPING(rig);
    QVERIFY(!rig.draft(0)["keeping"].toBool());
    // The keep that was only queued is dropped, and its tile is told.
    QVERIFY(!rig.draft(1)["keeping"].toBool());
    QVERIFY(!rig.backend->busy());
    QTest::qWait(300);
    QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 1);
    QVERIFY(!QFileInfo::exists(rig.keepDir()) || QDir(rig.keepDir()).entryList(QDir::Files).isEmpty());
    QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
}

void LimgenTests::backend_aNewBatchClearsUndoAndPurgesDiscards() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    rig.backend->keep(1);
    WAIT_KEPT(rig, 1);
    const QString kept = rig.draft(1)["keptPath"].toString();
    rig.backend->discard(2);
    const QString discardedId = rig.draft(2)["id"].toString();
    QVERIFY(binFiles(rig.sb).contains(discardedId + ".png"));
    QVERIFY(rig.backend->canUndo());
    rig.backend->setSubject("a lighthouse");
    rig.backend->generate();
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running() && rig.draft(0)["state"].toString() == "done", 40000);
    QVERIFY(!rig.backend->canUndo());
    QVERIFY(!rig.backend->canRedo());
    QVERIFY2(binFiles(rig.sb).isEmpty(), "the discarded draft is deleted for good");
    QVERIFY(QFileInfo(kept).isFile()); // what was kept stays
    QVERIFY(!rig.library().find(discardedId).isValid());
    rig.backend->undo(); // nothing to undo, and nothing comes back
    QVERIFY(!rig.library().find(discardedId).isValid());
}

void LimgenTests::backend_destroyingPurgesDiscardsAndStopsKeeps() {
    {
        KeepBackendRig rig;
        rig.create();
        QVERIFY(rig.makeBatch());
        rig.backend->discard(0);
        rig.backend->keep(1);
        WAIT_KEPT(rig, 1);
        const QString kept = rig.draft(1)["keptPath"].toString();
        QVERIFY(!binFiles(rig.sb).isEmpty());
        rig.backend.reset(); // the window closes
        QVERIFY2(binFiles(rig.sb).isEmpty(), "undo ends with the window, and the discarded draft with it");
        QVERIFY(QFileInfo(kept).isFile());
    }
    {
        KeepBackendRig rig;
        rig.create();
        QVERIFY(rig.makeBatch());
        qputenv("FAKE_SD_CLI_MODE", "hang");
        rig.backend->keep(0);
        QTRY_VERIFY_WITH_TIMEOUT(rig.upscalesDone() > 0, 15000);
        QTest::qWait(300);
        QCOMPARE(QProcess::execute("pgrep", {"-f", rig.sb.fixture.cache}), 0);
        rig.backend.reset(); // a keep is still running
        QVERIFY(QProcess::execute("pgrep", {"-f", rig.sb.fixture.cache}) == 1);
        QProcess::execute("pkill", {"-f", rig.sb.fixture.cache});
        QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
        QVERIFY(!QFileInfo::exists(rig.keepDir()) || QDir(rig.keepDir()).entryList(QDir::Files).isEmpty());
    }
}

void LimgenTests::backend_purgeDiscardedAtStartLeavesRecentOnes() {
    KeepBackendRig rig;
    Library library(rig.sb.library());
    auto discarded = [&](bool old) {
        DraftRecord record = fullRecord();
        const LibraryEntry draft = library.addDraft(makePng(rig.sb.fixture.root.path() + "/s-" + QString::number(QRandomGenerator::global()->generate()) + ".png"),
                                                    record, QDateTime::currentDateTimeUtc());
        library.discard(draft.id);
        if (old)
            for (const char *suffix : {".png", ".json"})
                setAge(rig.sb.library() + "/.discarded/" + draft.id + suffix, QDateTime::currentDateTime().addDays(-3));
        return draft.id;
    };
    const QString oldId = discarded(true), recentId = discarded(false);
    QCOMPARE(binFiles(rig.sb).size(), 4);
    rig.create();
    // What a window that did not close properly left is purged; a recent discard (another window's undo) stays.
    QVERIFY(!binFiles(rig.sb).contains(oldId + ".png") && !binFiles(rig.sb).contains(oldId + ".json"));
    QVERIFY(binFiles(rig.sb).contains(recentId + ".png") && binFiles(rig.sb).contains(recentId + ".json"));
}

void LimgenTests::backend_finishSettingsAreSaved() {
    KeepBackendRig rig;
    QVERIFY(writeFile(rig.sb.configFile(), R"({"future": {"x": 1}})"));
    rig.create();
    QCOMPARE(rig.backend->colorPass(), 0.3);
    QCOMPARE(rig.backend->formatIndex(), 0);
    QSignalSpy changed(rig.backend.get(), &Backend::settingsChanged);
    rig.backend->setColorPass(0.6);
    QCOMPARE(rig.backend->colorPass(), 0.6);
    QCOMPARE(changed.count(), 1);
    rig.backend->setColorPass(0.6); // the same again: nothing
    QCOMPARE(changed.count(), 1);
    rig.backend->setFormatIndex(1);
    QCOMPARE(rig.backend->formatIndex(), 1);
    QCOMPARE(changed.count(), 2);
    rig.backend->setFormatIndex(7); // out of range: ignored
    rig.backend->setFormatIndex(-1);
    QCOMPARE(rig.backend->formatIndex(), 1);
    QCOMPARE(changed.count(), 2);
    QJsonObject config = QJsonDocument::fromJson(readFile(rig.sb.configFile())).object();
    QCOMPARE(config["colorPass"].toDouble(), 0.6);
    QCOMPARE(config["format"].toString(), QString("jpeg"));
    QCOMPARE(config["future"].toObject()["x"].toInt(), 1); // other keys stay
    rig.backend->setColorPass(3); // above 1 is 1
    QCOMPARE(rig.backend->colorPass(), 1.0);
    rig.backend->setColorPass(0.6);
    // A new window starts with them, and keeps in that format.
    rig.create();
    QCOMPARE(rig.backend->colorPass(), 0.6);
    QCOMPARE(rig.backend->formatIndex(), 1);
    QVERIFY(rig.makeBatch());
    rig.backend->keep(0);
    WAIT_KEPT(rig, 0);
    QVERIFY(rig.draft(0)["keptPath"].toString().endsWith(".jpg"));
    const LibraryEntry final = [&] {
        for (const QString &file : QDir(rig.sb.library() + "/" + QDate::currentDate().toString("yyyy-MM")).entryList({"*.jpg"}))
            return Library(rig.sb.library()).find(QFileInfo(file).baseName());
        return LibraryEntry();
    }();
    QVERIFY(final.isValid());
    QCOMPARE(final.metadata["finish"].toObject()["format"].toString(), QString("jpeg"));
    QCOMPARE(final.metadata["finish"].toObject()["colorPass"].toDouble(), 0.6);
}

void LimgenTests::backend_keepMessageSurvivesTheThemeReloadItCauses() {
    KeepBackendRig rig;
    rig.create();
    QVERIFY(rig.makeBatch());
    QSignalSpy palette(rig.backend.get(), &Backend::paletteChanged);
    rig.backend->keep(0);
    WAIT_KEPT(rig, 0);
    QVERIFY2(rig.backend->status().contains("Kept as"), qPrintable(rig.backend->status()));
    // The new file in the theme's folder makes Omarchy's watcher fire, and the
    // window reloads the theme; that must not wipe the message about the keep.
    QTRY_VERIFY_WITH_TIMEOUT(palette.count() >= 1, 5000);
    QTest::qWait(600);
    QVERIFY2(rig.backend->status().contains("Kept as"), qPrintable(rig.backend->status()));
    QVERIFY(!rig.backend->statusIsError());
}

void LimgenTests::backend_keepFailuresAreShown() {
    {
        KeepBackendRig rig;
        rig.create();
        QVERIFY(rig.makeBatch());
        QVERIFY(QFile::remove(Catalogue::path(*rig.world.catalogue.upscaler(), rig.sb.fixture.models)));
        rig.backend->keep(0);
        WAIT_NOT_KEEPING(rig);
        QVERIFY(rig.backend->statusIsError());
        QVERIFY2(rig.backend->status().contains(rig.world.catalogue.upscaler()->name), qPrintable(rig.backend->status()));
        QVERIFY(!rig.draft(0)["kept"].toBool());
        QVERIFY(!rig.draft(0)["keeping"].toBool());
        QVERIFY(!rig.backend->canUndo()); // a keep that failed leaves nothing to undo
    }
    {
        KeepBackendRig rig;
        rig.create();
        QVERIFY(rig.makeBatch());
        qputenv("FAKE_SD_CLI_MODE", "oom");
        rig.backend->keep(1);
        WAIT_NOT_KEEPING(rig);
        QVERIFY(rig.backend->statusIsError());
        QVERIFY2(rig.backend->status().toLower().contains("memory"), qPrintable(rig.backend->status()));
        QVERIFY(!rig.draft(1)["kept"].toBool());
        QVERIFY(!rig.backend->canUndo());
        QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
        qunsetenv("FAKE_SD_CLI_MODE"); // and it can be tried again
        rig.backend->keep(1);
        WAIT_KEPT(rig, 1);
    }
}

void LimgenTests::backend_keepWithoutAThemeGoesToPictures() {
    KeepBackendRig rig(false); // no Omarchy theme at all
    rig.create();
    QVERIFY(rig.makeBatch());
    QCOMPARE(rig.draft(0)["theme"].toString(), QString());
    rig.backend->keep(0);
    WAIT_KEPT(rig, 0);
    const QString path = rig.draft(0)["keptPath"].toString();
    QCOMPARE(path, rig.sb.fixture.pictures() + "/" + Keeper::fileName("oil rig", rig.draft(0)["id"].toString(), "png"));
    QVERIFY(QFileInfo(path).isFile());
    QCOMPARE(filesUnder(rig.sb.fixture.backgrounds), QStringList());
}

// ============================== L5: the window, keeping ===================

namespace {

// Makes a batch in the window's backend and waits for it.
bool qmlBatch(QmlRig &q, const QString &subject = "oil rig") {
    for (int i = 0; i < 400 && !q.backend().engineReady(); ++i)
        QTest::qWait(50);
    q.backend().setSubject(subject);
    q.backend().generate();
    for (int i = 0; i < 800 && (q.backend().running() || q.backend().drafts().isEmpty()); ++i)
        QTest::qWait(50);
    if (q.backend().drafts().size() != 4)
        return false;
    for (int i = 0; i < 4; ++i)
        if (q.backend().drafts().at(i).toMap()["state"].toString() != "done")
            return false;
    return true;
}

QVariantMap qmlDraft(QmlRig &q, int index) { return q.backend().drafts().value(index).toMap(); }

QList<QQuickItem *> qmlTiles(QmlRig &q) {
    QList<QQuickItem *> tiles;
    for (QQuickItem *item : q.items())
        if (QString(item->metaObject()->className()).startsWith("DraftTile"))
            tiles << item;
    return tiles;
}

QQuickItem *buttonOf(QObject *popup, const QString &text) {
    QList<QQuickItem *> queue{qvariant_cast<QQuickItem *>(popup->property("footer"))};
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (!item)
            continue;
        if (item->inherits("QQuickAbstractButton") && item->property("text").toString() == text)
            return item;
        queue << item->childItems();
    }
    return nullptr;
}

QObject *themeDialogOf(const QmlRig &q) {
    return q.popup([](QObject *o) { return o->property("title").toString() == "Which theme should it go to?"; });
}

} // namespace

void LimgenTests::qml_keysKeepSetDiscardUndoRedo() {
    QmlRig q(true, fakeEngine, true);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    auto press = [&](Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { QTest::keyClick(q.window, key, modifiers); };
    // K keeps the selected draft; its tile shows the badge state.
    QTest::keyClick(q.window, 'k');
    QTRY_VERIFY_WITH_TIMEOUT(qmlDraft(q, 0)["kept"].toBool(), 30000);
    QVERIFY(QFileInfo(qmlDraft(q, 0)["keptPath"].toString()).isFile());
    QVERIFY(q.rig.sb.fixture.omarchyCalls().isEmpty());
    const QList<QQuickItem *> tiles = qmlTiles(q);
    QCOMPARE(tiles.size(), 4);
    QVERIFY(tiles[0]->property("kept").toBool());
    QVERIFY(!tiles[1]->property("kept").toBool());
    // S on the next one keeps it and shows it. Omarchy shows an earlier
    // background, so the keep can be undone.
    const QString previous = q.rig.sb.fixture.root.path() + "/previous.png";
    QVERIFY(writeFile(previous, "previous"));
    QVERIFY(QFile::link(previous, q.rig.sb.fixture.state + "/background"));
    press(Qt::Key_Right);
    QTRY_COMPARE(q.prop("selected").toInt(), 1);
    QTest::keyClick(q.window, 's');
    QTRY_VERIFY_WITH_TIMEOUT(qmlDraft(q, 1)["kept"].toBool() && qmlDraft(q, 1)["set"].toBool(), 30000);
    QCOMPARE(q.rig.sb.fixture.omarchyCalls(), QStringList{"theme bg set " + qmlDraft(q, 1)["keptPath"].toString()});
    // D discards the selected one, Ctrl+Z brings it back, Ctrl+Shift+Z discards it again.
    press(Qt::Key_Down);
    QTRY_COMPARE(q.prop("selected").toInt(), 3);
    QTest::keyClick(q.window, 'd');
    QTRY_VERIFY(qmlDraft(q, 3)["discarded"].toBool());
    QTRY_VERIFY(tiles[3]->property("discarded").toBool());
    press(Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(!qmlDraft(q, 3)["discarded"].toBool());
    press(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(qmlDraft(q, 3)["discarded"].toBool());
    // Two more undos: the discard, then the keep of draft 1, whose file goes.
    const QString installed1 = qmlDraft(q, 1)["keptPath"].toString();
    press(Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(!qmlDraft(q, 3)["discarded"].toBool());
    press(Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(!qmlDraft(q, 1)["kept"].toBool());
    QVERIFY(!QFileInfo::exists(installed1));
    QVERIFY(qmlDraft(q, 0)["kept"].toBool()); // the older keep is still there
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_actionKeysDoNothingWhileTypingOrUnderHelpOrADialog() {
    QmlRig q(true, fakeEngine, true);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    // Something to undo, so a stray Ctrl+Z would show.
    q.backend().discard(1);
    QVERIFY(qmlDraft(q, 1)["discarded"].toBool());
    // In the subject field the letters are text, and Ctrl+Z undoes the text.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    typeText(q.window, "kds");
    QVERIFY(q.backend().subject().endsWith("kds"));
    QTest::keyClick(q.window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(100);
    QVERIFY2(!q.backend().subject().endsWith("kds"), qPrintable(q.backend().subject())); // the text was undone
    QVERIFY(qmlDraft(q, 1)["discarded"].toBool());                                     // the discard was not
    QVERIFY(!qmlDraft(q, 0)["kept"].toBool());
    QVERIFY(!qmlDraft(q, 0)["discarded"].toBool());
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.typing());
    // Under the help overlay.
    QTest::keyClick(q.window, '?');
    QTRY_COMPARE(q.prop("helpVisible").toBool(), true);
    for (const char key : {'k', 's', 'd'})
        QTest::keyClick(q.window, key);
    QTest::keyClick(q.window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(150);
    QVERIFY(!qmlDraft(q, 0)["kept"].toBool() && !qmlDraft(q, 0)["keeping"].toBool());
    QVERIFY(!qmlDraft(q, 0)["discarded"].toBool());
    QVERIFY(qmlDraft(q, 1)["discarded"].toBool());
    QTest::keyClick(q.window, '?');
    QTRY_COMPARE(q.prop("helpVisible").toBool(), false);
    // Under a dialog (the model picker).
    QTest::keyClick(q.window, 'm');
    QTRY_VERIFY(q.isOpen(q.modelPicker()));
    for (const char key : {'k', 's', 'd'})
        QTest::keyClick(q.window, key);
    QTest::keyClick(q.window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(150);
    QVERIFY(!qmlDraft(q, 0)["kept"].toBool() && !qmlDraft(q, 0)["keeping"].toBool());
    QVERIFY(!qmlDraft(q, 0)["discarded"].toBool());
    QVERIFY(qmlDraft(q, 1)["discarded"].toBool());
    QVERIFY(q.rig.sb.fixture.omarchyCalls().isEmpty());
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.isOpen(q.modelPicker()));
    // And with the keys free again, they act.
    QTest::keyClick(q.window, 'd');
    QTRY_VERIFY(qmlDraft(q, 0)["discarded"].toBool());
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_themeDialogKeepsIntoTheChosenTheme() {
    QmlRig q(true, fakeEngine, true);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q)); // made for night-city
    QVERIFY(q.rig.sb.fixture.switchTo("other"));
    QTRY_COMPARE_WITH_TIMEOUT(q.backend().themeName(), QString("other"), 5000);
    QObject *dialog = themeDialogOf(q);
    QVERIFY(dialog);
    QVERIFY(!q.isOpen(dialog));
    // K asks, and nothing is made while it is open. Esc closes it.
    QTest::keyClick(q.window, 'k');
    QTRY_VERIFY(q.isOpen(dialog));
    QCOMPARE(q.prop("dialogOpen").toBool(), true);
    QVERIFY(!q.backend().keeping());
    QVERIFY(buttonOf(dialog, "night-city") && buttonOf(dialog, "other") && buttonOf(dialog, "Cancel"));
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(!q.isOpen(dialog));
    QVERIFY(!qmlDraft(q, 0)["kept"].toBool() && !q.backend().keeping());
    QCOMPARE(upscaleCalls(q.rig.sb.fakeLog()).size(), 0);
    // The current theme's button keeps it there.
    QTest::keyClick(q.window, 'k');
    QTRY_VERIFY(q.isOpen(dialog));
    QMetaObject::invokeMethod(buttonOf(dialog, "other"), "clicked");
    QTRY_VERIFY(!q.isOpen(dialog));
    QTRY_VERIFY_WITH_TIMEOUT(qmlDraft(q, 0)["kept"].toBool(), 30000);
    QCOMPARE(QFileInfo(qmlDraft(q, 0)["keptPath"].toString()).absolutePath(), q.rig.sb.fixture.backgrounds + "/other");
    // The theme it was made for: the other button, on the next draft.
    QTest::keyClick(q.window, Qt::Key_Right);
    QTRY_COMPARE(q.prop("selected").toInt(), 1);
    QTest::keyClick(q.window, 's');
    QTRY_VERIFY(q.isOpen(dialog));
    QMetaObject::invokeMethod(buttonOf(dialog, "night-city"), "clicked");
    QTRY_VERIFY_WITH_TIMEOUT(qmlDraft(q, 1)["kept"].toBool() && qmlDraft(q, 1)["set"].toBool(), 30000);
    QCOMPARE(QFileInfo(qmlDraft(q, 1)["keptPath"].toString()).absolutePath(), q.rig.sb.fixture.backgrounds + "/night-city");
    // Cancel closes it and keeps nothing.
    QTest::keyClick(q.window, Qt::Key_Right);
    QTest::keyClick(q.window, Qt::Key_Down);
    QTest::keyClick(q.window, 'k');
    QTRY_VERIFY(q.isOpen(dialog));
    QMetaObject::invokeMethod(buttonOf(dialog, "Cancel"), "clicked");
    QTRY_VERIFY(!q.isOpen(dialog));
    QVERIFY(!qmlDraft(q, q.prop("selected").toInt())["kept"].toBool());
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_quitWhileKeepingAsksFirst() {
    QmlRig q(true, fakeEngine, true);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    QSignalSpy aboutToQuit(QCoreApplication::instance(), &QCoreApplication::aboutToQuit);
    qputenv("FAKE_SD_CLI_MODE", "hang");
    QTest::keyClick(q.window, 'k');
    QTRY_VERIFY_WITH_TIMEOUT(!upscaleCalls(q.rig.sb.fakeLog()).isEmpty(), 15000);
    QVERIFY(q.backend().keeping());
    QVERIFY(!q.backend().running());
    QObject *dialog = q.quitDialog();
    QVERIFY(dialog);
    QTest::keyClick(q.window, 'q');
    QTRY_VERIFY(q.isOpen(dialog)); // work would be lost: it asks
    QCOMPARE(aboutToQuit.count(), 0);
    QVERIFY(q.backend().keeping());
    QTest::keyClick(q.window, Qt::Key_Escape); // keep working
    QTRY_VERIFY(!q.isOpen(dialog));
    QVERIFY(q.backend().keeping());
    QCOMPARE(q.prop("quitting").toBool(), false);
    q.backend().stopAll();
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().keeping(), 15000);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_finishSettingsUnderTheFullPrompt() {
    QmlRig q(true, fakeEngine, true);
    LOAD_WINDOW(q);
    QTRY_VERIFY_WITH_TIMEOUT(q.backend().engineReady(), 20000);
    // The colour pass slider; the variation strength has one too.
    QQuickItem *slider = nullptr;
    for (QQuickItem *item : q.items())
        if (item->objectName() == "colorPassSlider")
            slider = item;
    QVERIFY(slider);
    QList<QQuickItem *> combos;
    for (QQuickItem *item : q.items())
        if (item->inherits("QQuickComboBox"))
            combos << item;
    QCOMPARE(combos.size(), 2); // the style and the format
    // Folded away until P.
    QVERIFY(!slider->isVisible());
    QTest::keyClick(q.window, 'p');
    QTRY_COMPARE(q.prop("promptVisible").toBool(), true);
    QTRY_VERIFY(slider->isVisible());
    QVERIFY(combos[1]->isVisible());
    QVERIFY2(qAbs(slider->property("value").toDouble() - 0.3) < 1e-6, "the default colour pass");
    QCOMPARE(combos[1]->property("currentIndex").toInt(), 0);
    // The controls follow the settings, whoever changes them.
    q.backend().setColorPass(0.8);
    QTRY_VERIFY(qAbs(slider->property("value").toDouble() - 0.8) < 1e-6);
    q.backend().setFormatIndex(1);
    QTRY_COMPARE(combos[1]->property("currentIndex").toInt(), 1);
    QTest::qWait(200);
    NO_QML_WARNINGS(q);
    QTest::keyClick(q.window, 'p');
    QTRY_VERIFY(!slider->isVisible());
}

// ============================== L6: image to image ========================

namespace {

QDateTime at(int month, int day, int hour, int minute = 0) {
    return QDateTime(QDate(2026, month, day), QTime(hour, minute), QTimeZone::UTC);
}

// A draft with a given creation time and theme, for library listings.
LibraryEntry draftAt(Library &library, const QString &directory, const QDateTime &created,
                     const QString &theme = "night-city", const QString &subject = "oil rig") {
    DraftRecord record = fullRecord();
    record.subject = subject;
    record.themeSlug = theme;
    if (theme.isEmpty()) {
        record.themeMode.clear();
        record.palette = QJsonObject();
    }
    return library.addDraft(makePng(directory + "/s-" + QString::number(QRandomGenerator::global()->generate()) + ".png"),
                            record, created);
}

LibraryEntry finalAt(Library &library, const QString &directory, const QDateTime &created, const QString &from,
                     const QString &theme = "night-city", const QString &format = "png") {
    const QString file = directory + "/f-" + QString::number(QRandomGenerator::global()->generate()) + "." + format;
    if (format == "png")
        savePng(solidImage(QSize(8, 8), Qt::blue), file);
    else
        solidImage(QSize(8, 8), Qt::blue).save(file, "JPEG");
    QJsonObject metadata{{"subject", "oil rig"},
                         {"finish", QJsonObject{{"from", from}}},
                         {"theme", theme.isEmpty() ? QJsonValue() : QJsonValue(QJsonObject{{"slug", theme}})}};
    return library.addFinal(file, metadata, created);
}

} // namespace

void LimgenTests::engine_samplingStepsFollowTheEnginesFloatMaths_data() {
    QTest::addColumn<int>("steps");
    QTest::addColumn<double>("strength");
    QTest::addColumn<bool>("variation");
    QTest::addColumn<int>("expected");
    // (int)(steps x strength) + 1, the product in single precision, as the pinned release runs it.
    QTest::newRow("Z-Image 8 x 0.3") << 8 << 0.3 << true << 3;
    QTest::newRow("8 x 0.5") << 8 << 0.5 << true << 5;
    QTest::newRow("10 x 0.7") << 10 << 0.7 << true << 8;
    QTest::newRow("klein 4 x 0.25") << 4 << 0.25 << true << 2;
    QTest::newRow("8 x 0.1 runs one step") << 8 << 0.1 << true << 1;
    QTest::newRow("8 x 0.9") << 8 << 0.9 << true << 8;
    QTest::newRow("4 x 0.1 runs one step") << 4 << 0.1 << true << 1;
    QTest::newRow("4 x 0.5") << 4 << 0.5 << true << 3;
    QTest::newRow("a plain batch ignores the strength") << 8 << 0.3 << false << 8;
    QTest::newRow("a plain batch ignores 0.9") << 4 << 0.9 << false << 4;
}

void LimgenTests::engine_samplingStepsFollowTheEnginesFloatMaths() {
    QFETCH(int, steps);
    QFETCH(double, strength);
    QFETCH(bool, variation);
    QFETCH(int, expected);
    EngineRequest request;
    request.steps = steps;
    request.strength = strength;
    if (variation)
        request.initImage = "/cache/batch-1/source.png";
    QCOMPARE(request.samplingSteps(), expected);
}

void LimgenTests::engine_argumentsForAVariation() {
    EngineRequest request = fullRequest();
    QVERIFY(!SdCppEngine::arguments(request).contains("--init-img"));
    QVERIFY(!SdCppEngine::arguments(request).contains("--strength"));
    request.initImage = "/cache/batch-1/source.png";
    request.strength = 0.3;
    QStringList arguments = SdCppEngine::arguments(request);
    QCOMPARE(valueAfter(arguments, "--init-img"), QString("/cache/batch-1/source.png"));
    QCOMPARE(valueAfter(arguments, "--strength"), QString("0.3"));
    // After the output option, with the rest of a batch's options unchanged.
    QVERIFY(arguments.indexOf("--init-img") > arguments.indexOf("--output"));
    QStringList withoutInit = arguments;
    withoutInit.removeAt(withoutInit.indexOf("--init-img") + 1);
    withoutInit.removeAll("--init-img");
    withoutInit.removeAt(withoutInit.indexOf("--strength") + 1);
    withoutInit.removeAll("--strength");
    EngineRequest plain = request;
    plain.initImage.clear();
    QCOMPARE(withoutInit, SdCppEngine::arguments(plain));
    // Both options are in the pinned engine's --help.
    const QString help = QString::fromUtf8(dataFile("sd-cli-help.txt"));
    for (const QString &option : QStringList{"--init-img", "--strength"}) {
        const QRegularExpression word("(^|[\\s,])" + QRegularExpression::escape(option) + "([\\s,]|$)",
                                      QRegularExpression::MultilineOption);
        QVERIFY2(word.match(help).hasMatch(), qPrintable(option + " is not an option of the pinned sd-cli"));
    }
    request.strength = 0.55;
    QCOMPARE(valueAfter(SdCppEngine::arguments(request), "--strength"), QString("0.55"));
}

void LimgenTests::progress_targetLineSetsTheStepsPerImage() {
    // The engine says how many steps it runs; that wins over the count worked out before.
    ProgressParser parser(2, 8);
    QCOMPARE(parser.steps(), 8);
    QVERIFY(parser.feed("[I] target t_enc is 2 steps --- image.cpp:245\n").isEmpty());
    QCOMPARE(parser.steps(), 3);
    // Then the engine encodes the source; with tiling its tile lines look
    // like steps, even with the same total, and are not counted.
    QVERIFY(parser.feed("  |=| 1/3 - 9.00it/s\n  |===| 3/3 - 9.00it/s\n").isEmpty());
    QVERIFY(parser.feed("[I] encode_first_stage completed, taking 5.38s --- image.cpp:509\n").isEmpty());
    QList<ProgressEvent> events = parser.feed("  |=| 1/3 - 1.00s/it\n  |==| 2/3 - 1.00s/it\n  |===| 3/3 - 1.00s/it\n"
                                              "[I] sampling completed, taking 3s\n"
                                              "  |=| 1/3 - 1.00s/it\n  |==| 2/3 - 1.00s/it\n  |===| 3/3 - 1.00s/it\n"
                                              "[I] sampling completed, taking 3s\n");
    const auto steps = ofKind(events, ProgressEvent::Step);
    QCOMPARE(steps.size(), 6);
    for (int k = 0; k < 6; ++k) {
        QCOMPARE(steps[k].image, k / 3);
        QCOMPARE(steps[k].step, k % 3 + 1);
        QCOMPARE(steps[k].steps, 3);
    }
    QCOMPARE(ofKind(events, ProgressEvent::Sampled).size(), 2);
    // A line counted for the old total is not a step any more.
    ProgressParser again(1, 8);
    again.feed("[I] target t_enc is 2 steps\n");
    QVERIFY(again.feed("  |=| 1/8 - 1.00s/it\n").isEmpty());
    // The same line in chunks of one byte.
    ProgressParser bytes(1, 8);
    QList<ProgressEvent> byteEvents;
    for (const char c : QByteArray("[I] target t_enc is 4 steps\n[I] encode_first_stage completed\n"
                                   "  |=| 1/5 - 1.00s/it\n"))
        byteEvents += bytes.feed(QByteArray(1, c));
    QCOMPARE(bytes.steps(), 5);
    QCOMPARE(byteEvents.size(), 1);
    // An upscale counts tiles of any total and keeps doing so.
    ProgressParser tiles(1, 0);
    tiles.feed("[I] target t_enc is 2 steps\n");
    QCOMPARE(tiles.steps(), 0);
    QCOMPARE(tiles.feed("  |=| 7/112 - 1.00s/it\n").size(), 1);
}

void LimgenTests::progress_vaeTileLinesAreNotStepsInAVariation() {
    ProgressParser parser(1, 5);
    // Encoding the source with --vae-tiling prints tile progress in the same format.
    QList<ProgressEvent> events = parser.feed("  |==| 3/9 - 0.50s/it\n  |====| 9/9 - 0.50s/it\n"
                                              "[I] target t_enc is 2 steps --- image.cpp:245\n"
                                              "[I] encode_first_stage completed, taking 0.40s\n"
                                              "  |=| 1/3 - 1.00s/it\n  |==| 2/3 - 1.00s/it\n  |===| 3/3 - 1.00s/it\n"
                                              "[I] sampling completed, taking 3s\n");
    const auto steps = ofKind(events, ProgressEvent::Step);
    QCOMPARE(steps.size(), 3); // only the sampling steps
    QCOMPARE(steps.last().steps, 3);
    QCOMPARE(ofKind(events, ProgressEvent::Sampled).size(), 1);
}

void LimgenTests::engine_imageToImageRunWithTheFake() {
    Sandbox sb;
    EngineRequest request = runnableRequest(sb.fixture.root.path(), 2, 8);
    request.initImage = sb.fixture.root.path() + "/init.png";
    QVERIFY(savePng(solidImage(QSize(128, 96), QColor(10, 120, 200)), request.initImage));
    request.strength = 0.3;
    QCOMPARE(request.samplingSteps(), 3);
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY2(run.finished, qPrintable(run.error.message + run.error.detail));
    QCOMPARE(run.drafts.size(), 2);
    for (const QString &draft : run.drafts)
        QCOMPARE(QImageReader(draft).size(), QSize(128, 96));
    // The engine ran 3 steps per draft, not 8, and said so for the progress.
    QCOMPARE(run.progress.size(), 6);
    for (int k = 0; k < 6; ++k) {
        QCOMPARE(run.progress[k].image, k / 3);
        QCOMPARE(run.progress[k].step, k % 3 + 1);
        QCOMPARE(run.progress[k].steps, 3);
    }
    QCOMPARE(run.sampled, (QList<int>{0, 1}));
    const QStringList arguments = imageCalls(sb.fakeLog()).first();
    QCOMPARE(valueAfter(arguments, "--init-img"), request.initImage);
    QCOMPARE(valueAfter(arguments, "--strength"), QString("0.3"));
    QCOMPARE(arguments, SdCppEngine::arguments(request));
}

void LimgenTests::engine_imageToImagePreviewsFollowTheShortRun() {
    Sandbox sb;
    EngineRequest request = runnableRequest(sb.fixture.root.path(), 2, 8);
    request.initImage = sb.fixture.root.path() + "/init.png";
    QVERIFY(savePng(solidImage(QSize(128, 96), Qt::red), request.initImage));
    request.strength = 0.3;
    request.previews = true;
    request.previewInterval = 1;
    SdCppEngine engine(fakeEngine);
    EngineRun run;
    RUN_ENGINE(engine, run, request, 20000);
    QVERIFY2(run.finished, qPrintable(run.error.message + run.error.detail));
    // 3 steps and a preview at each: 3 per draft, so the first three belong to draft 0.
    QCOMPARE(run.previews.size(), 6);
    QList<int> images;
    for (const auto &preview : run.previews)
        images << preview.first;
    QCOMPARE(images, (QList<int>{0, 0, 0, 1, 1, 1}));
}

void LimgenTests::engine_imageToImageNeedsAReadableSource() {
    Sandbox sb;
    for (const QString &kind : QStringList{"missing", "not an image"}) {
        EngineRequest request = runnableRequest(sb.fixture.root.path(), 1, 4);
        request.initImage = sb.fixture.root.path() + "/init-" + kind.left(3) + ".png";
        if (kind != "missing")
            QVERIFY(writeFile(request.initImage, "this is not a PNG"));
        request.strength = 0.5;
        SdCppEngine engine(fakeEngine);
        EngineRun run;
        RUN_ENGINE(engine, run, request, 20000);
        QVERIFY2(run.failed, qPrintable(kind));
        QVERIFY(!run.finished);
        QCOMPARE(int(run.error.kind), int(EngineError::Failed));
        QVERIFY2(run.error.detail.contains("load image"), qPrintable(run.error.detail));
    }
}

// ============================== L6: Library list ==========================

void LimgenTests::library_listIsNewestFirstAcrossMonths() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    const QString dir = fixture.root.path();
    // Created out of order, across two months, two of them in the same second.
    const LibraryEntry oct = draftAt(library, dir, at(10, 31, 23));
    const LibraryEntry newer = draftAt(library, dir, at(11, 2, 10));
    const LibraryEntry tie = draftAt(library, dir, at(11, 2, 10));
    const LibraryEntry final = finalAt(library, dir, at(11, 15, 9), newer.id);
    const LibraryEntry latest = draftAt(library, dir, at(11, 20, 8));
    QVERIFY(oct.isValid() && newer.isValid() && tie.isValid() && final.isValid() && latest.isValid());
    const QList<LibraryEntry> list = library.list();
    QStringList ids;
    for (const LibraryEntry &entry : list)
        ids << entry.id;
    // Same second: the larger id comes first.
    const QString first = newer.id > tie.id ? newer.id : tie.id, second = newer.id > tie.id ? tie.id : newer.id;
    QCOMPARE(ids, (QStringList{latest.id, final.id, first, second, oct.id}));
    // Each entry is complete: image, sidecar and metadata, whichever its kind.
    for (const LibraryEntry &entry : list) {
        QVERIFY(QFileInfo(entry.image).isFile());
        QVERIFY(QFileInfo(entry.sidecar).isFile());
        QCOMPARE(entry.metadata["id"].toString(), entry.id);
    }
    QCOMPARE(list[1].metadata["kind"].toString(), QString("final"));
    QCOMPARE(list[0].metadata["kind"].toString(), QString("draft"));
    // The same, listed again, with an entry added later at the top.
    const LibraryEntry newest = draftAt(library, dir, at(11, 21, 1));
    QCOMPARE(library.list().first().id, newest.id);
    QCOMPARE(library.list().size(), 6);
}

void LimgenTests::library_listSkipsWhatIsNotAnEntry() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    const QString root = fixture.data + "/library", dir = fixture.root.path();
    Library library(root);
    const LibraryEntry good = draftAt(library, dir, at(11, 2, 10));
    const LibraryEntry jpg = finalAt(library, dir, at(11, 3, 10), good.id, "night-city", "jpg");
    const LibraryEntry discarded = draftAt(library, dir, at(11, 4, 10));
    QVERIFY(library.discard(discarded.id));
    // An image whose sidecar was never written, and a sidecar whose image is gone.
    const QString month = root + "/2026-11";
    QVERIFY(QFile::copy(good.image, month + "/2026-11-05-aaaaaa.png"));
    QVERIFY(writeFile(month + "/2026-11-06-bbbbbb.json", R"({"id": "2026-11-06-bbbbbb", "kind": "draft"})"));
    // A sidecar that names another id, one that is not JSON, and stray files.
    QVERIFY(QFile::copy(good.image, month + "/2026-11-07-cccccc.png"));
    QVERIFY(writeFile(month + "/2026-11-07-cccccc.json", R"({"id": "2026-11-07-dddddd"})"));
    QVERIFY(QFile::copy(good.image, month + "/2026-11-08-eeeeee.png"));
    QVERIFY(writeFile(month + "/2026-11-08-eeeeee.json", "not json"));
    QVERIFY(writeFile(month + "/2026-11-09-ffffff.png.part", "partial"));
    QVERIFY(writeFile(month + "/notes.json", R"({"id": "notes"})"));
    // Folders that are not months: with a perfectly good entry inside.
    for (const QString &folder : QStringList{"notes", "2026-1", "2026-11-extra", ".hidden"}) {
        QVERIFY(QDir().mkpath(root + "/" + folder));
        QVERIFY(QFile::copy(good.image, root + "/" + folder + "/" + good.id + ".png"));
        QVERIFY(QFile::copy(good.sidecar, root + "/" + folder + "/" + good.id + ".json"));
    }
    QVERIFY(writeFile(root + "/stray-file.json", "{}"));
    QStringList ids;
    for (const LibraryEntry &entry : library.list())
        ids << entry.id;
    QCOMPARE(ids, (QStringList{jpg.id, good.id})); // the jpg final and the draft, newest first
    QVERIFY(jpg.image.endsWith(".jpg"));
    QCOMPARE(library.list()[0].image, jpg.image);
    // Nothing there at all.
    QCOMPARE(Library(fixture.root.path() + "/nowhere").list().size(), 0);
    QCOMPARE(Library(fixture.root.path() + "/empty").list().size(), 0);
}

void LimgenTests::library_sidecarRecordsTheSource() {
    Fixture fixture;
    QVERIFY(fixture.ok());
    Library library(fixture.data + "/library");
    DraftRecord record = fullRecord();
    const QString png = makePng(fixture.root.path() + "/a.png");
    const LibraryEntry plain = library.addDraft(png, record, fixedTime);
    QVERIFY(plain.metadata.contains("source"));
    QVERIFY(plain.metadata["source"].isNull());
    record.source = QJsonObject{{"id", "2026-11-20-abcdef"}, {"strength", 0.5}};
    const LibraryEntry fromId = library.addDraft(png, record, fixedTime);
    record.source = QJsonObject{{"path", "/themes/x/1.png"}, {"strength", 0.3}};
    const LibraryEntry fromPath = library.addDraft(png, record, fixedTime);
    // As written to disk, not only as returned.
    const QJsonObject a = QJsonDocument::fromJson(readFile(fromId.sidecar)).object()["source"].toObject();
    QCOMPARE(a["id"].toString(), QString("2026-11-20-abcdef"));
    QCOMPARE(a["strength"].toDouble(), 0.5);
    QVERIFY(!a.contains("path"));
    const QJsonObject b = QJsonDocument::fromJson(readFile(fromPath.sidecar)).object()["source"].toObject();
    QCOMPARE(b["path"].toString(), QString("/themes/x/1.png"));
    QCOMPARE(b["strength"].toDouble(), 0.3);
    QVERIFY(QJsonDocument::fromJson(readFile(plain.sidecar)).object()["source"].isNull());
}

// ============================== L6: Batch variations ======================

void LimgenTests::batch_variationFromAFileRecordsItsSource() {
    BatchRig rig;
    const QString source = rig.sb.fixture.root.path() + "/themes-bg/night.png";
    QVERIFY(savePng(solidImage(QSize(100, 50), QColor(30, 80, 160)), source));
    BatchSettings settings = BatchRig::settings();
    settings.source = source;
    settings.strength = 0.5;
    settings.seed = 9;
    QString message;
    QVERIFY2(rig.prepare(settings, &message) == Batch::Outcome::Ok, qPrintable(message));
    QCOMPARE(rig.batch->source(), (QJsonObject{{"path", source}, {"strength", 0.5}}));
    const EngineRequest &request = rig.batch->request();
    QCOMPARE(request.initImage, request.outputDirectory + "/source.png");
    QCOMPARE(request.strength, 0.5);
    QCOMPARE(request.samplingSteps(), 5);
    QVERIFY(!QFileInfo::exists(request.initImage)); // made at start
    const QString folder = request.outputDirectory;
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.run.entries.size(), 4);
    QCOMPARE(rig.run.progress, 20); // 4 drafts x 5 steps, not 8
    for (const LibraryEntry &entry : rig.run.entries) {
        const QJsonObject recorded = entry.metadata["source"].toObject();
        QCOMPARE(recorded["path"].toString(), source);
        QCOMPARE(recorded["strength"].toDouble(), 0.5);
        QVERIFY(!recorded.contains("id"));
    }
    const QStringList arguments = imageCalls(rig.sb.fakeLog()).first();
    QCOMPARE(valueAfter(arguments, "--init-img"), folder + "/source.png");
    QCOMPARE(valueAfter(arguments, "--strength"), QString("0.5"));
    QVERIFY(!QFileInfo::exists(folder)); // the prepared source goes with the batch folder
    // The source file is not touched.
    QCOMPARE(QImageReader(source).size(), QSize(100, 50));
    // A plain batch has neither.
    rig.remake();
    QVERIFY(rig.prepare(BatchRig::settings()) == Batch::Outcome::Ok);
    QVERIFY(rig.batch->source().isEmpty());
    QVERIFY(rig.batch->request().initImage.isEmpty());
    QCOMPARE(rig.batch->request().samplingSteps(), 8);
}

void LimgenTests::batch_retriedVariationCountsOnlySamplingSteps() {
    BatchRig rig;
    qputenv("FAKE_SD_CLI_MODE", "oom-once");
    const QString source = rig.sb.fixture.root.path() + "/source.png";
    QVERIFY(savePng(solidImage(QSize(100, 50), QColor(30, 80, 160)), source));
    BatchSettings settings = BatchRig::settings();
    settings.source = source;
    settings.strength = 0.5;
    QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::Ok));
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    // The retry tiles the source's encoding, whose tile lines look like
    // steps (here with the same total); only the sampling counts.
    QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 2);
    QCOMPARE(rig.run.progress, 20);
}

void LimgenTests::batch_variationFromALibraryIdAndWhenItsPromptIsReused() {
    BatchRig rig;
    Library library(rig.sb.library());
    const LibraryEntry draft = addTestDraft(library, rig.sb.fixture.root.path(), "oil rig at dusk");
    const QString reused = fullRecord().prompt; // what the draft was made from
    QVERIFY(!reused.isEmpty());
    BatchSettings settings = BatchRig::settings();
    settings.source = draft.id;
    // Nothing typed: the image's prompt again.
    settings.subject.clear();
    QVERIFY(rig.prepare(settings) == Batch::Outcome::Ok);
    QCOMPARE(rig.batch->request().prompt, reused);
    QCOMPARE(rig.batch->source(), (QJsonObject{{"id", draft.id}, {"strength", 0.5}}));
    QCOMPARE(rig.batch->request().strength, 0.5);
    settings.subject = "   ";
    QVERIFY(rig.prepare(settings) == Batch::Outcome::Ok);
    QCOMPARE(rig.batch->request().prompt, reused);
    // A subject typed: the prompt follows it.
    settings.subject = "a lighthouse";
    QVERIFY(rig.prepare(settings) == Batch::Outcome::Ok);
    QVERIFY2(rig.batch->request().prompt.startsWith("a lighthouse, cinematic wide shot"), qPrintable(rig.batch->request().prompt));
    // A full prompt wins over both.
    settings.subject.clear();
    settings.fullPrompt = "only this";
    QVERIFY(rig.prepare(settings) == Batch::Outcome::Ok);
    QCOMPARE(rig.batch->request().prompt, QString("only this"));
    // A file has no prompt to reuse: nothing typed gives the built prompt of no subject.
    const QString file = rig.sb.fixture.root.path() + "/bg.png";
    QVERIFY(savePng(solidImage(QSize(32, 18), Qt::green), file));
    BatchSettings fromFile = BatchRig::settings();
    fromFile.subject.clear();
    fromFile.source = file;
    QVERIFY(rig.prepare(fromFile) == Batch::Outcome::Ok);
    QVERIFY2(rig.batch->request().prompt.startsWith("cinematic wide shot"), qPrintable(rig.batch->request().prompt));
    // And it files with the source, then the draft's sidecar says which draft it came from.
    settings.fullPrompt.clear();
    settings.subject.clear();
    QVERIFY(rig.prepare(settings) == Batch::Outcome::Ok);
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    QCOMPARE(rig.run.entries.first().metadata["source"].toObject()["id"].toString(), draft.id);
    QCOMPARE(rig.run.entries.first().metadata["prompt"].toString(), reused);
}

void LimgenTests::batch_variationStrengthIsClampedAndRaised() {
    BatchRig rig;
    const QString source = rig.sb.fixture.root.path() + "/bg.png";
    QVERIFY(savePng(solidImage(QSize(32, 18), Qt::green), source));
    QVERIFY(installModel(rig.catalogue, "flux2-klein-4b", rig.sb.fixture.models));
    struct Row { QString model; double asked; double used; };
    const double raised8 = 1.0001 / 8, raised4 = 1.0001 / 4;
    const QList<Row> rows{{"z-image-turbo", 0.5, 0.5},    {"z-image-turbo", 0.95, 0.9},    {"z-image-turbo", 1.5, 0.9},
                          {"z-image-turbo", 0.2, 0.2},    {"z-image-turbo", 0.13, 0.13},   // 8 x 0.13 = 1.04: runs 2 steps
                          {"z-image-turbo", 0.1, raised8}, {"z-image-turbo", 0.05, raised8}, {"z-image-turbo", 0.12, raised8},
                          {"flux2-klein-4b", 0.25, 0.25},  {"flux2-klein-4b", 0.1, raised4},  {"flux2-klein-4b", 0.2, raised4}};
    for (const Row &row : rows) {
        BatchSettings settings = BatchRig::settings();
        settings.modelId = row.model;
        settings.source = source;
        settings.strength = row.asked;
        QString message;
        QVERIFY2(rig.prepare(settings, &message) == Batch::Outcome::Ok, qPrintable(message));
        const QString what = QString("%1 at %2").arg(row.model).arg(row.asked);
        QVERIFY2(qAbs(rig.batch->request().strength - row.used) < 1e-9, qPrintable(what + ": " + QString::number(rig.batch->request().strength)));
        QVERIFY2(rig.batch->request().samplingSteps() >= 2, qPrintable(what));
        // The sidecar records the strength that was used.
        QVERIFY2(qAbs(rig.batch->source()["strength"].toDouble() - row.used) < 1e-9, qPrintable(what));
    }
}

void LimgenTests::batch_variationSourceProblemsComeInOrder() {
    EngineProbe bad;
    bad.error = {EngineError::NoGpu, "the engine says no", {}};
    {
        BatchRig rig;
        BatchSettings settings = BatchRig::settings();
        QString message;
        settings.source = "2026-11-20-ffffff"; // looks like an id, and is not in the library
        QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::UnknownSource));
        QVERIFY2(message.contains("2026-11-20-ffffff"), qPrintable(message));
        QVERIFY(!rig.batch->isRunning());
        settings.source = rig.sb.fixture.root.path() + "/no-such.png";
        QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::UnknownSource));
        QVERIFY2(message.contains("no-such.png"), qPrintable(message));
        QVERIFY(writeFile(rig.sb.fixture.root.path() + "/text.png", "this is not an image"));
        settings.source = rig.sb.fixture.root.path() + "/text.png";
        QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::UnknownSource));
        // Before the engine is asked about: a bad source is reported even when the engine is broken.
        rig.probe = bad;
        QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::UnknownSource));
        QVERIFY(!message.contains("the engine says no"));
        // And the other way: with a good source the engine's problem is what is said.
        const QString good = rig.sb.fixture.root.path() + "/good.png";
        QVERIFY(savePng(solidImage(QSize(8, 8), Qt::red), good));
        settings.source = good;
        QCOMPARE(int(rig.prepare(settings, &message)), int(Batch::Outcome::NotRunnable));
        QCOMPARE(message, QString("the engine says no"));
    }
    {
        // The model and its files come first.
        BatchRig rig(false);
        BatchSettings settings = BatchRig::settings();
        settings.source = "2026-11-20-ffffff";
        QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::NotRunnable));
        settings.modelId = "nope";
        QCOMPARE(int(rig.prepare(settings)), int(Batch::Outcome::UnknownModel));
    }
}

void LimgenTests::batch_variationSourceIsCoveredToTheDraftSize() {
    // A wide source (2:1) for 16:9 drafts crops the sides; a tall one crops top and bottom.
    // In both the centre band is green, so the centre of the prepared image must be green.
    for (const bool wide : {true, false}) {
        BatchRig rig;
        qputenv("FAKE_SD_CLI_MODE", "hang");
        QImage image(wide ? QSize(100, 50) : QSize(50, 100), QImage::Format_RGB32);
        image.fill(QColor(220, 30, 30));
        QPainter painter(&image);
        painter.fillRect(wide ? QRect(40, 0, 20, 50) : QRect(0, 40, 50, 20), QColor(30, 220, 30));
        painter.end();
        const QString source = rig.sb.fixture.root.path() + "/source.png";
        QVERIFY(savePng(image, source));
        BatchSettings settings = BatchRig::settings();
        settings.source = source;
        QVERIFY(rig.prepare(settings) == Batch::Outcome::Ok);
        const QString initImage = rig.batch->request().initImage;
        rig.batch->start();
        QTRY_VERIFY_WITH_TIMEOUT(!imageCalls(rig.sb.fakeLog()).isEmpty(), 15000);
        // The engine takes it at the drafts' size, with the centre kept.
        const QImage prepared(initImage);
        QCOMPARE(prepared.size(), QSize(1344, 768));
        const QColor centre = prepared.pixelColor(672, 384), corner = prepared.pixelColor(3, 3);
        QVERIFY2(centre.green() > 180 && centre.red() < 80, qPrintable(centre.name()));
        QVERIFY2(corner.red() > 180 && corner.green() < 80, qPrintable(corner.name()));
        // Cropped, not squeezed: the band keeps its size, so a point 145 (or 200) pixels out is still inside it.
        const QColor out = prepared.pixelColor(672 + (wide ? 145 : 0), 384 + (wide ? 0 : 200));
        QVERIFY2(out.green() > 180 && out.red() < 80, qPrintable(out.name()));
        rig.batch->cancel();
        WAIT_FOR_BATCH(rig);
        QCOMPARE(int(rig.run.outcome), int(Batch::Outcome::Cancelled));
        QVERIFY(!QFileInfo::exists(initImage)); // gone with the folder
        QCOMPARE(QImageReader(source).size(), image.size());
    }
}

void LimgenTests::batch_variationRecordsNoTimingAndEstimatesItsSteps() {
    BatchRig rig;
    QVERIFY(rig.prepare(BatchRig::settings()) == Batch::Outcome::Ok);
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    const Timing timing = Timings(rig.sb.benchFile()).get("z-image-turbo/q4_k/vulkan");
    QVERIFY(timing.isKnown());
    const QByteArray bench = readFile(rig.sb.benchFile());
    // A variation of 5 steps is estimated from the same timing, with 5 steps.
    const QString source = rig.sb.fixture.root.path() + "/bg.png";
    QVERIFY(savePng(solidImage(QSize(32, 18), Qt::green), source));
    BatchSettings settings = BatchRig::settings();
    settings.source = source;
    rig.remake();
    QVERIFY(rig.prepare(settings) == Batch::Outcome::Ok);
    QCOMPARE(rig.batch->estimate(), timing.estimate(4, 5));
    QVERIFY(rig.batch->estimate() < timing.estimate(4, 8));
    rig.batch->start();
    WAIT_FOR_BATCH(rig);
    QVERIFY2(rig.run.finished, qPrintable(rig.run.message));
    // It is not a measure of a batch: the timings are as they were.
    QCOMPARE(readFile(rig.sb.benchFile()), bench);
}

void LimgenTests::batch_variationSourceThatCannotBePrepared() {
    BatchRig rig;
    // A PNG that is cut off after its header: it looks like an image until it is read.
    QImage image(40, 30, QImage::Format_RGB32);
    image.fill(Qt::red);
    const QString whole = rig.sb.fixture.root.path() + "/whole.png", cut = rig.sb.fixture.root.path() + "/cut.png";
    QVERIFY(savePng(image, whole));
    QVERIFY(writeFile(cut, readFile(whole).left(33)));
    BatchSettings settings = BatchRig::settings();
    settings.source = cut;
    QString message;
    QVERIFY2(rig.prepare(settings, &message) == Batch::Outcome::Ok, qPrintable(message));
    const QString folder = rig.batch->request().outputDirectory;
    QSignalSpy failed(rig.batch.get(), &Batch::failed);
    rig.batch->start();
    QCOMPARE(failed.count(), 0); // reported from the event loop, like an engine failure
    WAIT_FOR_BATCH(rig);
    QVERIFY(rig.run.failed);
    QCOMPARE(int(rig.run.outcome), int(Batch::Outcome::Failed));
    QVERIFY2(rig.run.message.contains("cut.png"), qPrintable(rig.run.message));
    QVERIFY(!rig.batch->isRunning());
    QVERIFY(!QFileInfo::exists(folder));
    QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), 0); // the engine was never asked
    QCOMPARE(filesUnder(rig.sb.library()), QStringList());
    QCOMPARE(filesUnder(rig.sb.fixture.cache), QStringList());
}

// ============================== L6: CLI ===================================

void LimgenTests::cli_generateFromAFileAndALibraryImage() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const QHash<QString, QString> env = engineEnvironment(fixture);
    const QString file = fixture.root.path() + "/themes/night.png";
    QVERIFY(savePng(solidImage(QSize(160, 90), QColor(30, 80, 160)), file));
    const QString library = fixture.data + "/limgen/library";
    // From a file, with no subject at all.
    CliResult result = runCli(fixture, {"generate", "--from", file, "--count", "2", "--seed", "5", "--json"}, env);
    QVERIFY2(result.exitCode == 0, qPrintable(result.err));
    QJsonObject json = jsonObject(result.out);
    QCOMPARE(json["source"].toObject(), (QJsonObject{{"path", file}, {"strength", 0.5}}));
    QVERIFY2(json["prompt"].toString().startsWith("cinematic wide shot"), qPrintable(json["prompt"].toString()));
    QCOMPARE(json["drafts"].toArray().size(), 2);
    QStringList call = imageCalls(fixture.root.path() + "/fake.log").last();
    QVERIFY(call.contains("--init-img"));
    QCOMPARE(valueAfter(call, "--strength"), QString("0.5"));
    const QString id = json["drafts"].toArray()[0].toObject()["id"].toString();
    QCOMPARE(Library(library).find(id).metadata["source"].toObject()["path"].toString(), file);
    // A strength of its own.
    result = runCli(fixture, {"generate", "--from", file, "--strength", "0.3", "--count", "1", "--json"}, env);
    QCOMPARE(result.exitCode, 0);
    json = jsonObject(result.out);
    QCOMPARE(json["source"].toObject()["strength"].toDouble(), 0.3);
    call = imageCalls(fixture.root.path() + "/fake.log").last();
    QCOMPARE(valueAfter(call, "--strength"), QString("0.3"));
    // From a library image: its prompt when nothing is typed, the subject's when something is.
    Library lib(library);
    const LibraryEntry draft = addTestDraft(lib, fixture.root.path(), "oil rig at dusk");
    result = runCli(fixture, {"generate", "--from", draft.id, "--count", "1", "--json"}, env);
    QCOMPARE(result.exitCode, 0);
    json = jsonObject(result.out);
    QCOMPARE(json["prompt"].toString(), fullRecord().prompt);
    QCOMPARE(json["source"].toObject(), (QJsonObject{{"id", draft.id}, {"strength", 0.5}}));
    result = runCli(fixture, {"generate", "a lighthouse", "--from", draft.id, "--count", "1", "--json"}, env);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(jsonObject(result.out)["prompt"].toString().startsWith("a lighthouse, cinematic"));
    // A plain batch says it has no source.
    result = runCli(fixture, {"generate", "a fox", "--count", "1", "--json"}, env);
    QCOMPARE(result.exitCode, 0);
    json = jsonObject(result.out);
    QVERIFY(json.contains("source"));
    QVERIFY(json["source"].isNull());
    QVERIFY(!imageCalls(fixture.root.path() + "/fake.log").last().contains("--init-img"));
    // Text mode prints the drafts as before.
    result = runCli(fixture, {"generate", "--from", file, "--count", "1"}, env);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.out.split('\n', Qt::SkipEmptyParts).size(), 1);
}

void LimgenTests::cli_generateFromArgumentErrors_data() {
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("strength without a source") << QStringList{"generate", "a fox", "--strength", "0.5"};
    QTest::newRow("strength below 0.1") << QStringList{"generate", "--from", "@FILE@", "--strength", "0.05"};
    QTest::newRow("strength above 0.9") << QStringList{"generate", "--from", "@FILE@", "--strength", "0.95"};
    QTest::newRow("strength 1") << QStringList{"generate", "--from", "@FILE@", "--strength", "1"};
    QTest::newRow("strength not a number") << QStringList{"generate", "--from", "@FILE@", "--strength", "x"};
    QTest::newRow("strength missing its value") << QStringList{"generate", "--from", "@FILE@", "--strength"};
    QTest::newRow("a file that is not there") << QStringList{"generate", "--from", "@DIR@/nope.png"};
    QTest::newRow("an id that is not in the library") << QStringList{"generate", "--from", "2026-11-20-ffffff"};
    QTest::newRow("a file that is not an image") << QStringList{"generate", "--from", "@TEXT@"};
    QTest::newRow("nothing to go on") << QStringList{"generate"};
    QTest::newRow("a blank subject and no source") << QStringList{"generate", "   "};
}

void LimgenTests::cli_generateFromArgumentErrors() {
    REQUIRE_BINARY();
    QFETCH(QStringList, arguments);
    Fixture fixture;
    QVERIFY(fixture.ok());
    QVERIFY(installModel(Catalogue::builtIn(), "z-image-turbo", fixture.models));
    const QString file = fixture.root.path() + "/ok.png", text = fixture.root.path() + "/text.png";
    QVERIFY(savePng(solidImage(QSize(32, 18), Qt::green), file));
    QVERIFY(writeFile(text, "this is not an image"));
    for (QString &argument : arguments)
        argument.replace("@FILE@", file).replace("@TEXT@", text).replace("@DIR@", fixture.root.path());
    const CliResult result = runCli(fixture, arguments, engineEnvironment(fixture));
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 2);
    QCOMPARE(result.out, QString());
    QVERIFY(!result.err.isEmpty());
    QCOMPARE(imageCalls(fixture.root.path() + "/fake.log").size(), 0);
    QCOMPARE(filesUnder(fixture.data), QStringList());
}

namespace {
struct LibraryFixture {
    Fixture fixture;
    LibraryEntry d1, d2, d3, f1;
    QString dir() const { return fixture.data + "/limgen/library"; }
    LibraryFixture() {
        Library library(dir());
        d1 = draftAt(library, fixture.root.path(), at(11, 2, 10), "night-city", "first rig");
        d2 = draftAt(library, fixture.root.path(), at(11, 4, 10), "other", "second rig");
        d3 = draftAt(library, fixture.root.path(), at(11, 6, 10), QString(), "third rig");
        f1 = finalAt(library, fixture.root.path(), at(11, 8, 10), d1.id, "night-city");
    }
};
} // namespace

void LimgenTests::cli_libraryListsNewestFirst() {
    REQUIRE_BINARY();
    LibraryFixture rig;
    const CliResult result = runCli(rig.fixture, {"library"}, engineEnvironment(rig.fixture));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.err, QString());
    const QStringList lines = result.out.split('\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 4);
    // id, kind, the theme it was made for ("-" for none), and the path.
    QCOMPARE(lines[0], (QStringList{rig.f1.id, "final", "night-city", rig.f1.image}.join("  ")));
    QCOMPARE(lines[1], (QStringList{rig.d3.id, "draft", "-", rig.d3.image}.join("  ")));
    QCOMPARE(lines[2], (QStringList{rig.d2.id, "draft", "other", rig.d2.image}.join("  ")));
    QCOMPARE(lines[3], (QStringList{rig.d1.id, "draft", "night-city", rig.d1.image}.join("  ")));
    // Reading changes nothing.
    QCOMPARE(filesUnder(rig.dir()).size(), 8);
}

void LimgenTests::cli_libraryJsonAndTheme() {
    REQUIRE_BINARY();
    LibraryFixture rig;
    CliResult result = runCli(rig.fixture, {"library", "--json"}, engineEnvironment(rig.fixture));
    QCOMPARE(result.exitCode, 0);
    QJsonParseError parseError;
    QJsonArray list = QJsonDocument::fromJson(result.out.toUtf8(), &parseError).array();
    QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(result.out));
    QCOMPARE(list.size(), 4);
    QJsonObject final = list[0].toObject(), noTheme = list[1].toObject(), draft = list[3].toObject();
    QCOMPARE(final.keys(), (QStringList{"created", "from", "id", "kind", "path", "subject", "theme"}));
    QCOMPARE(final["from"].toString(), rig.d1.id);
    QCOMPARE(final["kind"].toString(), QString("final"));
    QCOMPARE(final["theme"].toString(), QString("night-city"));
    QCOMPARE(final["path"].toString(), rig.f1.image);
    // A draft has no "from"; one made for no theme has a null theme; the subject and time are there.
    QCOMPARE(draft.keys(), (QStringList{"created", "id", "kind", "path", "subject", "theme"}));
    QCOMPARE(draft["subject"].toString(), QString("first rig"));
    QCOMPARE(draft["created"].toString(), QString("2026-11-02T10:00:00Z"));
    QVERIFY(noTheme["theme"].isNull());
    QCOMPARE(noTheme["subject"].toString(), QString("third rig"));
    // --theme keeps what was made for that theme, finals included, and drops the rest, drafts for no theme too.
    result = runCli(rig.fixture, {"library", "--theme", "night-city", "--json"}, engineEnvironment(rig.fixture));
    QCOMPARE(result.exitCode, 0);
    list = QJsonDocument::fromJson(result.out.toUtf8()).array();
    QCOMPARE(list.size(), 2);
    QCOMPARE(list[0].toObject()["id"].toString(), rig.f1.id);
    QCOMPARE(list[1].toObject()["id"].toString(), rig.d1.id);
    result = runCli(rig.fixture, {"library", "--theme", "other"}, engineEnvironment(rig.fixture));
    QCOMPARE(result.out.split('\n', Qt::SkipEmptyParts).size(), 1);
    QVERIFY(result.out.contains(rig.d2.id));
    result = runCli(rig.fixture, {"library", "--theme", "nosuch"}, engineEnvironment(rig.fixture));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.out, QString());
}

void LimgenTests::cli_libraryEmptyAndErrors() {
    REQUIRE_BINARY();
    Fixture fixture;
    QVERIFY(fixture.ok());
    CliResult result = runCli(fixture, {"library"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.out, QString());
    result = runCli(fixture, {"library", "--json"}, engineEnvironment(fixture));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(QJsonDocument::fromJson(result.out.toUtf8()).array().size(), 0);
    QVERIFY(QJsonDocument::fromJson(result.out.toUtf8()).isArray());
    // Nothing is created by looking.
    QVERIFY(!QFileInfo::exists(fixture.data + "/limgen"));
    for (const QStringList &arguments : {QStringList{"library", "extra"}, QStringList{"library", "--bogus"},
                                         QStringList{"library", "--theme"}}) {
        result = runCli(fixture, arguments, engineEnvironment(fixture));
        QCOMPARE(result.exitCode, 2);
        QCOMPARE(result.out, QString());
    }
    const CliResult summary = runCli(fixture, {"help"});
    QVERIFY(summary.out.contains("library"));
    const CliResult help = runCli(fixture, {"help", "library"});
    QCOMPARE(help.exitCode, 0);
    QVERIFY(help.out.contains("--theme") && help.out.contains("--json"));
    const CliResult generate = runCli(fixture, {"help", "generate"});
    QVERIFY(generate.out.contains("--from") && generate.out.contains("--strength"));
}

// ============================== L6: Backend variations ====================

namespace {

void addThemeBackgrounds(const Fixture &fixture, const QStringList &names, const QString &theme = "night-city") {
    for (const QString &name : names)
        savePng(solidImage(QSize(64, 36), QColor(30 + name.at(0).unicode() % 100, 90, 150)),
                fixture.themes + "/" + theme + "/backgrounds/" + name);
}

void addUserBackground(const Fixture &fixture, const QString &name, const QString &theme = "night-city") {
    savePng(solidImage(QSize(64, 36), QColor(200, 90, 40)), fixture.backgrounds + "/" + theme + "/" + name);
}

QStringList namesOf(const QVariantList &list) {
    QStringList names;
    for (const QVariant &value : list)
        names << value.toMap()["name"].toString();
    return names;
}

// The sidecar of a draft the window shows.
QJsonObject sidecarOfDraft(const KeepBackendRig &rig, int index) {
    return rig.library().find(rig.draft(index)["id"].toString()).metadata;
}

} // namespace

void LimgenTests::backend_backgroundsAreTheThemesOwnThenTheUsers() {
    KeepBackendRig rig;
    addThemeBackgrounds(rig.sb.fixture, {"b.png", "a.png"});
    addUserBackground(rig.sb.fixture, "z.png");
    addUserBackground(rig.sb.fixture, "limgen-oil-rig-aaaaaa.png");
    rig.create();
    const QVariantList backgrounds = rig.backend->backgrounds();
    // The theme's own, sorted, then the ones added to it, sorted.
    QCOMPARE(namesOf(backgrounds), (QStringList{"a.png", "b.png", "limgen-oil-rig-aaaaaa.png", "z.png"}));
    const QStringList user{"limgen-oil-rig-aaaaaa.png", "z.png"};
    for (const QVariant &value : backgrounds) {
        const QVariantMap background = value.toMap();
        const QString name = background["name"].toString();
        QVERIFY(QFileInfo(background["path"].toString()).isFile());
        QCOMPARE(QUrl(background["url"].toString()).toLocalFile(), background["path"].toString());
        QCOMPARE(background["user"].toBool(), user.contains(name));
        QCOMPARE(background["limgen"].toBool(), name.startsWith("limgen-"));
    }
    QVERIFY(backgrounds[2].toMap()["path"].toString().startsWith(rig.sb.fixture.backgrounds + "/night-city/"));
    // No theme, no backgrounds.
    KeepBackendRig none(false);
    none.create();
    QVERIFY(none.backend->backgrounds().isEmpty());
}

void LimgenTests::backend_backgroundsRefreshAfterAKeepAndAThemeSwitch() {
    KeepBackendRig rig(true, 8);
    addThemeBackgrounds(rig.sb.fixture, {"a.png"});
    rig.create();
    QCOMPARE(namesOf(rig.backend->backgrounds()), QStringList{"a.png"});
    QVERIFY(rig.makeBatch());
    rig.backend->keep(0);
    WAIT_KEPT(rig, 0);
    // The kept image joined the theme's backgrounds, as one of limgen's own.
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->backgrounds().size(), 2, 5000);
    const QVariantMap added = rig.backend->backgrounds().last().toMap();
    QVERIFY(added["user"].toBool() && added["limgen"].toBool());
    QCOMPARE(added["path"].toString(), rig.draft(0)["keptPath"].toString());
    // Undoing the keep takes it away again.
    rig.backend->undo();
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->backgrounds().size(), 1, 5000);
    // Another theme has backgrounds of its own.
    addThemeBackgrounds(rig.sb.fixture, {"c.png", "d.png"}, "other");
    QVERIFY(rig.sb.fixture.switchTo("other"));
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->themeName(), QString("other"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(namesOf(rig.backend->backgrounds()), (QStringList{"c.png", "d.png"}), 5000);
}

void LimgenTests::backend_useBackgroundTogglesTheSource() {
    KeepBackendRig rig;
    addThemeBackgrounds(rig.sb.fixture, {"a.png", "b.png"});
    rig.create();
    QSignalSpy changed(rig.backend.get(), &Backend::sourceChanged);
    QVERIFY(rig.backend->source().isEmpty());
    const QString a = rig.backend->backgrounds()[0].toMap()["path"].toString(), b = rig.backend->backgrounds()[1].toMap()["path"].toString();
    rig.backend->useBackground(0);
    QCOMPARE(rig.backend->source()["kind"].toString(), QString("background"));
    QCOMPARE(rig.backend->source()["key"].toString(), a);
    QCOMPARE(rig.backend->source()["path"].toString(), a);
    QCOMPARE(rig.backend->source()["name"].toString(), QString("a.png"));
    QCOMPARE(QUrl(rig.backend->source()["url"].toString()).toLocalFile(), a);
    QCOMPARE(changed.count(), 1);
    // Another one replaces it; the same one again clears it.
    rig.backend->useBackground(1);
    QCOMPARE(rig.backend->source()["path"].toString(), b);
    QCOMPARE(changed.count(), 2);
    rig.backend->useBackground(1);
    QVERIFY(rig.backend->source().isEmpty());
    QCOMPARE(changed.count(), 3);
    // Out of range: nothing. Clearing an empty source: nothing either.
    rig.backend->useBackground(-1);
    rig.backend->useBackground(2);
    rig.backend->useBackground(99);
    rig.backend->clearSource();
    QVERIFY(rig.backend->source().isEmpty());
    QCOMPARE(changed.count(), 3);
    rig.backend->useBackground(0);
    rig.backend->clearSource();
    QVERIFY(rig.backend->source().isEmpty());
    QCOMPARE(changed.count(), 5);
}

void LimgenTests::backend_cycleBackgroundBothWaysAndWraps() {
    KeepBackendRig rig;
    addThemeBackgrounds(rig.sb.fixture, {"a.png", "b.png", "c.png"});
    rig.create();
    auto current = [&] { return rig.backend->source()["name"].toString(); };
    // B from nothing starts at the first; Shift+B at the last.
    rig.backend->cycleBackground(1);
    QCOMPARE(current(), QString("a.png"));
    rig.backend->cycleBackground(1);
    QCOMPARE(current(), QString("b.png"));
    rig.backend->cycleBackground(1);
    QCOMPARE(current(), QString("c.png"));
    rig.backend->cycleBackground(1); // wraps
    QCOMPARE(current(), QString("a.png"));
    rig.backend->cycleBackground(-1); // wraps the other way
    QCOMPARE(current(), QString("c.png"));
    rig.backend->cycleBackground(-1);
    QCOMPARE(current(), QString("b.png"));
    rig.backend->clearSource();
    rig.backend->cycleBackground(-1); // nothing chosen: the last
    QCOMPARE(current(), QString("c.png"));
    // One background only: it is chosen, and cycling does not clear it.
    KeepBackendRig single;
    addThemeBackgrounds(single.sb.fixture, {"only.png"});
    single.create();
    single.backend->cycleBackground(1);
    QCOMPARE(single.backend->source()["name"].toString(), QString("only.png"));
    single.backend->cycleBackground(1);
    QCOMPARE(single.backend->source()["name"].toString(), QString("only.png"));
    single.backend->cycleBackground(-1);
    QCOMPARE(single.backend->source()["name"].toString(), QString("only.png"));
    // None: nothing happens.
    KeepBackendRig none;
    none.create();
    none.backend->cycleBackground(1);
    none.backend->cycleBackground(-1);
    QVERIFY(none.backend->source().isEmpty());
}

void LimgenTests::backend_aBackgroundSourceGoesWithItsFile() {
    KeepBackendRig rig(true, 8);
    addThemeBackgrounds(rig.sb.fixture, {"a.png"});
    addThemeBackgrounds(rig.sb.fixture, {"c.png"}, "other");
    rig.create();
    rig.backend->useBackground(0);
    QCOMPARE(rig.backend->source()["name"].toString(), QString("a.png"));
    // Another theme: its backgrounds replace the old ones, and so does the
    // source, which belonged to the old theme.
    QVERIFY(rig.sb.fixture.switchTo("other"));
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->themeName(), QString("other"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(rig.backend->source().isEmpty(), 5000);
    // A kept image used as the source goes when its keep is undone.
    QVERIFY(rig.makeBatch());
    rig.backend->keep(0);
    WAIT_KEPT(rig, 0);
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->backgrounds().size(), 2, 5000);
    rig.backend->useBackground(1);
    QVERIFY(rig.backend->source()["name"].toString().startsWith("limgen-"));
    rig.backend->undo();
    QVERIFY(rig.backend->source().isEmpty());
    // A library source is not a background and stays.
    rig.backend->useLibrary(rig.draft(1)["id"].toString());
    QVERIFY(rig.sb.fixture.switchTo("night-city"));
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->themeName(), QString("night-city"), 5000);
    QCOMPARE(rig.backend->source()["kind"].toString(), QString("library"));
}

void LimgenTests::backend_libraryFollowsDiscardUndoAndRedo() {
    KeepBackendRig rig(true, 8);
    rig.create();
    QVERIFY(rig.makeBatch());
    rig.backend->loadLibrary();
    const QString id = rig.draft(0)["id"].toString();
    auto listed = [&] {
        for (const QVariant &item : rig.backend->library())
            if (item.toMap()["id"].toString() == id)
                return true;
        return false;
    };
    QVERIFY(listed());
    rig.backend->discard(0);
    QVERIFY(!listed());
    rig.backend->undo();
    QVERIFY(listed());
    rig.backend->redo();
    QVERIFY(!listed());
}

void LimgenTests::backend_varyDoesNothingWhenNoBatchCanStart() {
    KeepBackendRig rig(true, 8);
    rig.create();
    QVERIFY(rig.makeBatch());
    // A keep runs, and a batch already waits for it: V cannot start one.
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.backend->keep(0);
    QTRY_VERIFY_WITH_TIMEOUT(rig.upscalesDone() > 0, 15000);
    rig.backend->generate();
    QVERIFY(rig.backend->batchWaiting());
    rig.backend->vary(1);
    QVERIFY(rig.backend->source().isEmpty());
    rig.backend->varyLibrary(rig.draft(2)["id"].toString());
    QVERIFY(rig.backend->source().isEmpty());
    rig.backend->stopAll();
    WAIT_NOT_KEEPING(rig);
}

void LimgenTests::backend_fullPromptShowsTheReusedPrompt() {
    KeepBackendRig rig(true, 8);
    rig.create();
    QVERIFY(rig.makeBatch("a lighthouse in fog"));
    const QString used = rig.library().find(rig.draft(0)["id"].toString()).metadata["prompt"].toString();
    QVERIFY(used.startsWith("a lighthouse in fog"));
    // Nothing typed: the prompt shown is the one the variation will use.
    rig.backend->setSubject("");
    rig.backend->useLibrary(rig.draft(0)["id"].toString());
    QCOMPARE(rig.backend->fullPrompt(), used);
    // Typing a subject builds the prompt again.
    rig.backend->setSubject("a harbour at night");
    QVERIFY(rig.backend->fullPrompt().startsWith("a harbour at night"));
    rig.backend->setSubject("");
    QCOMPARE(rig.backend->fullPrompt(), used);
    rig.backend->clearSource();
    QVERIFY(!rig.backend->fullPrompt().startsWith("a lighthouse"));
}

void LimgenTests::batch_strengthForRaisesAndClamps() {
    QCOMPARE(Batch::strengthFor(8, 0.5), 0.5);
    // Below the minimum is the minimum, then raised for eight steps.
    QVERIFY(Batch::strengthFor(8, 0.05) > 0.125 && Batch::strengthFor(8, 0.05) < 0.126);
    QCOMPARE(Batch::strengthFor(8, 1.5), 0.9);
    // Four steps: 0.1 and 0.2 would run no step, so they are raised.
    QVERIFY(Batch::strengthFor(4, 0.1) > 0.25 && Batch::strengthFor(4, 0.1) < 0.26);
    QVERIFY(Batch::strengthFor(4, 0.2) > 0.25 && Batch::strengthFor(4, 0.2) < 0.26);
    QCOMPARE(Batch::strengthFor(4, 0.3), 0.3);
    // Never past the maximum, even for a single step.
    QCOMPARE(Batch::strengthFor(1, 0.5), 0.9);
}

void LimgenTests::backend_estimateUsesTheRaisedStrength() {
    KeepBackendRig rig(true, 4);
    rig.create();
    QVERIFY(rig.makeBatch());
    // The first batch measured this machine; the next estimate follows it.
    const CatalogueModel *model = rig.world.catalogue.defaultModel();
    const Timing timing =
        Timings(rig.sb.benchFile()).get(Timings::key(model->id, model->levels.first().id, "vulkan"));
    QVERIFY(timing.isKnown());
    rig.backend->setStrength(0.1);
    rig.backend->useLibrary(rig.draft(0)["id"].toString());
    // At 0.1 a four-step model runs no step, so the batch raises it to run
    // one, plus the one the engine adds: two steps, as the estimate says.
    EngineRequest request;
    request.steps = 4;
    request.initImage = "x";
    request.strength = Batch::strengthFor(4, 0.1);
    QCOMPARE(request.samplingSteps(), 2);
    const QString expected = Backend::tr("About %1 for %n drafts", "", 4).arg(Backend::durationText(timing.estimate(4, 2)));
    QTRY_COMPARE(rig.backend->estimate(), expected);
}

void LimgenTests::backend_varyAndVaryLibrary() {
    KeepBackendRig rig(true, 8);
    rig.create();
    QVERIFY(rig.makeBatch("oil rig"));
    const QString firstId = rig.draft(1)["id"].toString();
    const QString firstPrompt = sidecarOfDraft(rig, 1)["prompt"].toString();
    QVERIFY(!firstPrompt.isEmpty());
    // V on a draft: the batch is a variation of it, at the saved strength, following the typed subject.
    rig.backend->setSubject("a lighthouse");
    rig.backend->vary(1);
    QVERIFY(rig.backend->running());
    QCOMPARE(rig.backend->source()["kind"].toString(), QString("library"));
    QCOMPARE(rig.backend->source()["key"].toString(), firstId);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running() && rig.draft(0)["state"].toString() == "done", 40000);
    QJsonObject sidecar = sidecarOfDraft(rig, 0);
    QCOMPARE(sidecar["source"].toObject(), (QJsonObject{{"id", firstId}, {"strength", 0.5}}));
    QVERIFY(sidecar["prompt"].toString().startsWith("a lighthouse"));
    QVERIFY(rig.draft(0)["id"].toString() != firstId);
    QCOMPARE(imageCalls(rig.sb.fakeLog()).last().contains("--init-img"), true);
    // The source stays for the next batch until it is cleared.
    QCOMPARE(rig.backend->source()["key"].toString(), firstId);
    // With nothing typed, the source's prompt is used again, at a strength of its own.
    rig.backend->setSubject("");
    rig.backend->setStrength(0.3);
    rig.backend->vary(2);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running() && rig.draft(0)["state"].toString() == "done", 40000);
    sidecar = sidecarOfDraft(rig, 0);
    QCOMPARE(sidecar["source"].toObject()["strength"].toDouble(), 0.3);
    QCOMPARE(valueAfter(imageCalls(rig.sb.fakeLog()).last(), "--strength"), QString("0.3"));
    // varyLibrary starts from any image in the library, such as the first batch's draft.
    rig.backend->varyLibrary(firstId);
    QVERIFY(rig.backend->running());
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running() && rig.draft(0)["state"].toString() == "done", 40000);
    QCOMPARE(sidecarOfDraft(rig, 0)["source"].toObject()["id"].toString(), firstId);
    QCOMPARE(sidecarOfDraft(rig, 0)["prompt"].toString(), firstPrompt);
    // An id that is not there: said, and no batch.
    rig.backend->clearSource();
    const int calls = imageCalls(rig.sb.fakeLog()).size();
    rig.backend->varyLibrary("2026-11-20-ffffff");
    QVERIFY(!rig.backend->running());
    QVERIFY(rig.backend->statusIsError());
    QVERIFY(rig.backend->source().isEmpty());
    QCOMPARE(imageCalls(rig.sb.fakeLog()).size(), calls);
    // A discarded draft, or an index out of range, is not varied.
    rig.backend->discard(3);
    rig.backend->vary(3);
    rig.backend->vary(-1);
    rig.backend->vary(9);
    QVERIFY(!rig.backend->running());
    QVERIFY(rig.backend->source().isEmpty());
}

void LimgenTests::backend_rerunAndTheWaitingBatchKeepTheSource() {
    KeepBackendRig rig(true, 8);
    addThemeBackgrounds(rig.sb.fixture, {"a.png", "b.png"});
    rig.create();
    const QString a = rig.backend->backgrounds()[0].toMap()["path"].toString();
    rig.backend->useBackground(0);
    QVERIFY(rig.makeBatch("oil rig"));
    QCOMPARE(sidecarOfDraft(rig, 0)["source"].toObject(), (QJsonObject{{"path", a}, {"strength", 0.5}}));
    const QString firstSeed = QString::number(rig.draft(0)["seed"].toDouble());
    // Rerun after the source was cleared and the strength changed: the same variation, new seeds.
    rig.backend->clearSource();
    rig.backend->setStrength(0.7);
    QVERIFY(rig.backend->canRerun());
    rig.backend->rerun();
    QVERIFY(rig.backend->running());
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running() && rig.draft(0)["state"].toString() == "done", 40000);
    QCOMPARE(sidecarOfDraft(rig, 0)["source"].toObject(), (QJsonObject{{"path", a}, {"strength", 0.5}}));
    QVERIFY(QString::number(rig.draft(0)["seed"].toDouble()) != firstSeed);
    QVERIFY(rig.backend->source().isEmpty()); // rerun does not bring the source back
    // A batch that waits for a keep carries the source it was asked with.
    rig.backend->setStrength(0.5);
    rig.backend->useBackground(1);
    const QString b = rig.backend->source()["path"].toString();
    qputenv("FAKE_SD_CLI_MODE", "hang");
    rig.backend->keep(0);
    QTRY_VERIFY_WITH_TIMEOUT(rig.upscalesDone() > 0, 15000);
    rig.backend->generate();
    QVERIFY(rig.backend->batchWaiting());
    rig.backend->clearSource();
    rig.backend->setStrength(0.8);
    qunsetenv("FAKE_SD_CLI_MODE");
    rig.backend->undo(); // the keep ends and the batch starts
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running() && !rig.backend->batchWaiting() && imageCalls(rig.sb.fakeLog()).size() == 3, 40000);
    QTRY_COMPARE_WITH_TIMEOUT(rig.draft(0)["state"].toString(), QString("done"), 40000);
    QCOMPARE(sidecarOfDraft(rig, 0)["source"].toObject(), (QJsonObject{{"path", b}, {"strength", 0.5}}));
}

void LimgenTests::backend_canGenerateWithAnEmptySubjectOnlyWithASource() {
    KeepBackendRig rig(true, 8);
    addThemeBackgrounds(rig.sb.fixture, {"a.png"});
    rig.create();
    QVERIFY(!rig.backend->canGenerate()); // nothing typed, no source
    rig.backend->setSubject("   ");
    QVERIFY(!rig.backend->canGenerate());
    rig.backend->useBackground(0);
    QVERIFY(rig.backend->canGenerate());
    rig.backend->generate();
    QVERIFY(rig.backend->running());
    QTRY_VERIFY_WITH_TIMEOUT(!rig.backend->running() && rig.draft(0)["state"].toString() == "done", 40000);
    // The source stayed, so the next batch can start the same way.
    QVERIFY(!rig.backend->source().isEmpty());
    QVERIFY(rig.backend->canGenerate());
    QVERIFY(sidecarOfDraft(rig, 0)["prompt"].toString().startsWith("cinematic wide shot")); // a file brings no prompt
    rig.backend->clearSource();
    QVERIFY(!rig.backend->canGenerate());
    rig.backend->generate(); // does nothing
    QVERIFY(!rig.backend->running());
    // Not while the engine or the model is not ready, source or not.
    KeepBackendRig missing;
    addThemeBackgrounds(missing.sb.fixture, {"a.png"});
    missing.create(missing.sb.fixture.root.path() + "/no-such-engine");
    missing.backend->useBackground(0);
    QVERIFY(!missing.backend->canGenerate());
}

void LimgenTests::backend_loadLibraryFiltersByThemeAndFlagsKept() {
    KeepBackendRig rig(true, 8);
    rig.create();
    QVERIFY(rig.backend->library().isEmpty()); // not read until the window asks
    QVERIFY(rig.makeBatch("first rig"));
    QVERIFY(rig.makeBatch("second rig"));
    // Images made for another theme and for none.
    Library library(rig.sb.library());
    const LibraryEntry other = draftAt(library, rig.sb.fixture.root.path(), QDateTime::currentDateTimeUtc(), "other", "other rig");
    const LibraryEntry none = draftAt(library, rig.sb.fixture.root.path(), QDateTime::currentDateTimeUtc(), QString(), "no theme rig");
    QSignalSpy changed(rig.backend.get(), &Backend::libraryChanged);
    rig.backend->loadLibrary();
    QCOMPARE(changed.count(), 1);
    QVariantList list = rig.backend->library();
    QCOMPARE(list.size(), 8); // this theme's drafts only
    QStringList ids;
    for (const QVariant &value : list) {
        const QVariantMap entry = value.toMap();
        ids << entry["id"].toString();
        QCOMPARE(entry["kind"].toString(), QString("draft"));
        QCOMPARE(entry["state"].toString(), QString("done")); // so the full view takes it like a draft
        QVERIFY(QUrl(entry["image"].toString()).toLocalFile().endsWith(".png"));
        QVERIFY(QFileInfo(QUrl(entry["image"].toString()).toLocalFile()).isFile());
        QVERIFY(!entry["kept"].toBool());
        QVERIFY(entry["subject"].toString().endsWith("rig"));
        QVERIFY(entry["seed"].toDouble() >= 0);
        QVERIFY(QDateTime::fromString(entry["created"].toString(), Qt::ISODate).isValid());
    }
    QVERIFY(!ids.contains(other.id) && !ids.contains(none.id));
    // Newest first.
    for (int i = 1; i < list.size(); ++i)
        QVERIFY(list[i - 1].toMap()["created"].toString() >= list[i].toMap()["created"].toString());
    // Once read, it follows a keep by itself: the draft is kept, and its final is listed.
    const QString keptId = rig.draft(0)["id"].toString();
    changed.clear();
    rig.backend->keep(0);
    WAIT_KEPT(rig, 0);
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->library().size(), 9, 5000);
    int finals = 0, keptFlags = 0;
    for (const QVariant &value : rig.backend->library()) {
        const QVariantMap entry = value.toMap();
        finals += entry["kind"].toString() == "final";
        keptFlags += entry["kept"].toBool();
        if (entry["id"].toString() == keptId)
            QVERIFY(entry["kept"].toBool());
    }
    QCOMPARE(finals, 1);
    QCOMPARE(keptFlags, 1);
    QVERIFY(changed.count() >= 1);
    // ... and a new batch.
    QVERIFY(rig.makeBatch("third rig"));
    QTRY_COMPARE_WITH_TIMEOUT(rig.backend->library().size(), 13, 5000);
    // A discarded draft is not listed.
    rig.backend->discard(2);
    rig.backend->loadLibrary();
    QCOMPARE(rig.backend->library().size(), 12);
    // Without an Omarchy theme, everything is.
    KeepBackendRig noTheme(false, 8);
    noTheme.create();
    Library all(noTheme.sb.library());
    draftAt(all, noTheme.sb.fixture.root.path(), QDateTime::currentDateTimeUtc(), "other");
    draftAt(all, noTheme.sb.fixture.root.path(), QDateTime::currentDateTimeUtc(), QString());
    noTheme.backend->loadLibrary();
    QCOMPARE(noTheme.backend->library().size(), 2);
}

void LimgenTests::backend_strengthIsSavedToConfig() {
    KeepBackendRig rig;
    QVERIFY(writeFile(rig.sb.configFile(), R"({"future": {"x": 1}})"));
    rig.create();
    QCOMPARE(rig.backend->strength(), 0.5);
    QSignalSpy changed(rig.backend.get(), &Backend::settingsChanged);
    rig.backend->setStrength(0.7);
    QCOMPARE(rig.backend->strength(), 0.7);
    QCOMPARE(changed.count(), 1);
    rig.backend->setStrength(0.7); // the same: nothing
    QCOMPARE(changed.count(), 1);
    QJsonObject config = QJsonDocument::fromJson(readFile(rig.sb.configFile())).object();
    QCOMPARE(config["strength"].toDouble(), 0.7);
    QCOMPARE(config["future"].toObject()["x"].toInt(), 1);
    // The range is 0.1 to 0.9.
    rig.backend->setStrength(0.95);
    QCOMPARE(rig.backend->strength(), 0.9);
    rig.backend->setStrength(0.01);
    QCOMPARE(rig.backend->strength(), 0.1);
    QCOMPARE(QJsonDocument::fromJson(readFile(rig.sb.configFile())).object()["strength"].toDouble(), 0.1);
    rig.backend->setStrength(0.35);
    // A new window starts with it; a value out of range in the file is the default.
    rig.create();
    QCOMPARE(rig.backend->strength(), 0.35);
    QVERIFY(writeFile(rig.sb.configFile(), R"({"strength": 2})"));
    rig.create();
    QCOMPARE(rig.backend->strength(), 0.5);
    QVERIFY(writeFile(rig.sb.configFile(), R"({"strength": "strong"})"));
    rig.create();
    QCOMPARE(rig.backend->strength(), 0.5);
    QCOMPARE(Config::defaultStrength, 0.5);
}

// ============================== L6: the window ============================

namespace {

QQuickItem *abstractButton(const QmlRig &q, const QString &text) {
    for (QQuickItem *item : q.items())
        if (item->inherits("QQuickAbstractButton") && item->isVisible() && item->property("text").toString() == text)
            return item;
    return nullptr;
}

QQuickItem *itemByObjectName(const QmlRig &q, const QString &name) {
    for (QQuickItem *item : q.items())
        if (item->objectName() == name)
            return item;
    return nullptr;
}

// The point at the middle of an item, in the window.
QPoint centreOf(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

// Theme backgrounds for the window to list, then a reload so it does.
void giveTheWindowBackgrounds(QmlRig &q, const QStringList &names) {
    addThemeBackgrounds(q.rig.sb.fixture, names);
    q.backend().reload();
}

} // namespace

void LimgenTests::qml_keysBAndShiftBCycleTheBackgrounds() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    giveTheWindowBackgrounds(q, {"a.png", "b.png", "c.png"});
    QTRY_COMPARE(q.backend().backgrounds().size(), 3);
    auto current = [&] { return q.backend().source()["name"].toString(); };
    QTest::keyClick(q.window, 'b');
    QTRY_COMPARE(current(), QString("a.png"));
    QTest::keyClick(q.window, 'b');
    QTRY_COMPARE(current(), QString("b.png"));
    QTest::keyClick(q.window, Qt::Key_B, Qt::ShiftModifier);
    QTRY_COMPARE(current(), QString("a.png"));
    QTest::keyClick(q.window, Qt::Key_B, Qt::ShiftModifier); // wraps to the last
    QTRY_COMPARE(current(), QString("c.png"));
    // Esc, with nothing else open, goes back to plain batches.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_VERIFY(q.backend().source().isEmpty());
    // In the subject field B is a letter.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    typeText(q.window, "bb");
    QTest::keyClick(q.window, Qt::Key_B, Qt::ShiftModifier);
    QTest::qWait(100);
    QVERIFY(q.backend().source().isEmpty());
    QVERIFY(q.backend().subject().contains("bb"));
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_clickInTheLeftColumnChoosesAndClears() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    giveTheWindowBackgrounds(q, {"a.png", "b.png"});
    QTRY_COMPARE(q.backend().backgrounds().size(), 2);
    QQuickItem *column = q.itemNamed("BackgroundColumn");
    QVERIFY(column);
    QList<QQuickItem *> areas;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        areas.clear();
        for (QQuickItem *item : q.items())
            if (item->inherits("QQuickMouseArea") && item->isVisible() && item->width() > 0) {
                for (QQuickItem *parent = item->parentItem(); parent; parent = parent->parentItem())
                    if (parent == column) {
                        areas << item;
                        break;
                    }
            }
        return areas.size() == 2;
    })(), 5000);
    QTest::mouseClick(q.window, Qt::LeftButton, Qt::NoModifier, centreOf(areas[0]));
    QTRY_COMPARE(q.backend().source()["name"].toString(), QString("a.png"));
    QTest::mouseClick(q.window, Qt::LeftButton, Qt::NoModifier, centreOf(areas[1]));
    QTRY_COMPARE(q.backend().source()["name"].toString(), QString("b.png"));
    QTest::mouseClick(q.window, Qt::LeftButton, Qt::NoModifier, centreOf(areas[1])); // again: clear
    QTRY_VERIFY(q.backend().source().isEmpty());
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_vVariesTheSelectedDraftAndTheSourceRowShows() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QQuickItem *slider = itemByObjectName(q, "strengthSlider");
    QVERIFY(slider);
    QVERIFY(!slider->isVisible()); // no source, no source row
    QVERIFY(abstractButton(q, "Generate"));
    QVERIFY(qmlBatch(q));
    QTest::keyClick(q.window, Qt::Key_Right);
    QTRY_COMPARE(q.prop("selected").toInt(), 1);
    const QString id = qmlDraft(q, 1)["id"].toString();
    QTest::keyClick(q.window, 'v');
    QTRY_VERIFY(q.backend().running());
    QCOMPARE(q.backend().source()["key"].toString(), id);
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 40000);
    QCOMPARE(Library(q.rig.sb.library()).find(qmlDraft(q, 0)["id"].toString()).metadata["source"].toObject()["id"].toString(), id);
    // The source row shows, the main button says what it does, and the strength follows the settings.
    QTRY_VERIFY(slider->isVisible());
    QTRY_VERIFY(abstractButton(q, "Make variations"));
    QVERIFY(!abstractButton(q, "Generate"));
    QVERIFY(qAbs(slider->property("value").toDouble() - 0.5) < 1e-6);
    q.backend().setStrength(0.7);
    QTRY_VERIFY(qAbs(slider->property("value").toDouble() - 0.7) < 1e-6);
    QTest::qWait(150);
    NO_QML_WARNINGS(q);
    // Clear goes back to plain batches.
    QQuickItem *clear = abstractButton(q, "Clear");
    QVERIFY(clear);
    QTest::mouseClick(q.window, Qt::LeftButton, Qt::NoModifier, centreOf(clear));
    QTRY_VERIFY(q.backend().source().isEmpty());
    QTRY_VERIFY(abstractButton(q, "Generate"));
    QTRY_VERIFY(!slider->isVisible());
}

void LimgenTests::qml_strengthSliderSavesWhenLetGo() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    giveTheWindowBackgrounds(q, {"a.png"});
    QTRY_COMPARE(q.backend().backgrounds().size(), 1);
    QTest::keyClick(q.window, 'b');
    QTRY_VERIFY(!q.backend().source().isEmpty());
    QQuickItem *slider = itemByObjectName(q, "strengthSlider");
    QVERIFY(slider);
    QTRY_VERIFY(slider->isVisible());
    // A click a quarter of the way in from the right end, on the track.
    const QPoint where = slider->mapToScene(QPointF(slider->width() * 0.75, slider->height() / 2)).toPoint();
    QTest::mousePress(q.window, Qt::LeftButton, Qt::NoModifier, where);
    QTest::mouseRelease(q.window, Qt::LeftButton, Qt::NoModifier, where);
    QTRY_VERIFY2(q.backend().strength() > 0.6 && q.backend().strength() < 0.85, qPrintable(QString::number(q.backend().strength())));
    QCOMPARE(QJsonDocument::fromJson(readFile(q.rig.sb.configFile())).object()["strength"].toDouble(), q.backend().strength());
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_libraryOpensAndClosesWithL() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    QQuickItem *view = q.itemNamed("LibraryView");
    QVERIFY(view);
    QVERIFY(!view->isVisible());
    QVERIFY(q.backend().library().isEmpty());
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    QTRY_VERIFY(view->isVisible());
    QCOMPARE(q.backend().library().size(), 4); // read when it opened
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), false);
    QTRY_VERIFY(!view->isVisible());
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), false);
    // In the subject field L is a letter.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    typeText(q.window, "ll");
    QTest::qWait(100);
    QCOMPARE(q.prop("libraryVisible").toBool(), false);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_arrowsMoveByRowInTheLibrary() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q, "first rig"));
    QVERIFY(qmlBatch(q, "second rig"));
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    QQuickItem *view = q.itemNamed("LibraryView");
    QTRY_VERIFY(view->property("columns").toInt() >= 2);
    const int columns = view->property("columns").toInt();
    const int count = q.backend().library().size();
    QCOMPARE(count, 8);
    QVERIFY2(columns < count, "the test needs more than one row");
    auto selected = [&] { return q.prop("librarySelected").toInt(); };
    QCOMPARE(selected(), 0);
    QTest::keyClick(q.window, Qt::Key_Right);
    QTRY_COMPARE(selected(), 1);
    QTest::keyClick(q.window, Qt::Key_Down); // a whole row down
    QTRY_COMPARE(selected(), 1 + columns);
    QTest::keyClick(q.window, Qt::Key_Up);
    QTRY_COMPARE(selected(), 1);
    QTest::keyClick(q.window, Qt::Key_Left);
    QTRY_COMPARE(selected(), 0);
    QTest::keyClick(q.window, Qt::Key_Left); // the left end
    QTest::keyClick(q.window, Qt::Key_Up);   // the top
    QTest::qWait(60);
    QCOMPARE(selected(), 0);
    // Down to the last row and no further.
    for (int i = 0; i < 5; ++i)
        QTest::keyClick(q.window, Qt::Key_Down);
    QTest::qWait(60);
    QVERIFY(selected() + columns >= count); // the last row
    QVERIFY(selected() < count);
    // The grid of batch drafts is not what moved.
    QCOMPARE(q.prop("selected").toInt(), 0);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_theLibraryTakesTheClicks() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    // Beside the library's title, where the grid's second tile is hidden
    // underneath, a click and a double-click reach nothing.
    QQuickItem *library = q.itemNamed("LibraryView");
    QVERIFY(library);
    const QPoint besideTitle = library->mapToScene(QPointF(library->width() - 60, 26)).toPoint();
    QTest::mouseClick(q.window, Qt::LeftButton, Qt::NoModifier, besideTitle);
    QTest::mouseDClick(q.window, Qt::LeftButton, Qt::NoModifier, besideTitle);
    QTest::qWait(100);
    QCOMPARE(q.prop("selected").toInt(), 0);
    QCOMPARE(q.prop("fullView").toBool(), false);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_closingTheLibraryClosesItsFullView() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    QTest::keyClick(q.window, 'f');
    QTRY_COMPARE(q.prop("fullView").toBool(), true);
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), false);
    QCOMPARE(q.prop("fullView").toBool(), false);
    // The arrows work in the grid again.
    QTest::keyClick(q.window, Qt::Key_Right);
    QTRY_COMPARE(q.prop("selected").toInt(), 1);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_librarySelectionFollowsTheList() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    QTest::keyClick(q.window, Qt::Key_Right);
    QTest::keyClick(q.window, Qt::Key_Right);
    QTRY_COMPARE(q.prop("librarySelected").toInt(), 2);
    const QString id = q.backend().library()[2].toMap()["id"].toString();
    // An image ahead of it in the list goes: the selection stays on the
    // same image, now one place earlier.
    const QString first = q.backend().library()[0].toMap()["id"].toString();
    int tile = -1;
    for (int i = 0; i < q.backend().drafts().size(); ++i)
        if (qmlDraft(q, i)["id"].toString() == first)
            tile = i;
    QVERIFY(tile >= 0);
    q.backend().discard(tile);
    QTRY_COMPARE(q.prop("librarySelected").toInt(), 1);
    QCOMPARE(q.backend().library()[1].toMap()["id"].toString(), id);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_aKeptLibraryDraftNamesNoEmptyFile() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    q.backend().keep(0);
    QTRY_VERIFY_WITH_TIMEOUT(qmlDraft(q, 0)["kept"].toBool(), 30000);
    const QString id = qmlDraft(q, 0)["id"].toString();
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    int index = -1;
    for (int i = 0; i < q.backend().library().size(); ++i)
        if (q.backend().library()[i].toMap()["id"].toString() == id)
            index = i;
    QVERIFY(index >= 0);
    q.window->setProperty("librarySelected", index);
    QTest::keyClick(q.window, 'f');
    QTRY_COMPARE(q.prop("fullView").toBool(), true);
    QTest::qWait(100);
    for (QQuickItem *item : q.items())
        if (item->inherits("QQuickLabel") && item->isVisible())
            QVERIFY2(!item->property("text").toString().trimmed().endsWith("kept as"),
                     qPrintable(item->property("text").toString()));
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_fShowsTheLibraryImage() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    QTest::keyClick(q.window, Qt::Key_Right);
    QTRY_COMPARE(q.prop("librarySelected").toInt(), 1);
    const QString id = q.backend().library()[1].toMap()["id"].toString();
    QTest::keyClick(q.window, 'f');
    QTRY_COMPARE(q.prop("fullView").toBool(), true);
    QQuickItem *full = q.itemNamed("FullView");
    QVERIFY(full);
    QTRY_VERIFY(full->isVisible());
    QCOMPARE(full->property("draft").toMap()["id"].toString(), id);
    // Esc closes the full view first, then the library.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE(q.prop("fullView").toBool(), false);
    QCOMPARE(q.prop("libraryVisible").toBool(), true);
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), false);
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_vInTheLibraryVariesTheSelectedImage() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    QTest::keyClick(q.window, Qt::Key_Right);
    QTRY_COMPARE(q.prop("librarySelected").toInt(), 1);
    const QString id = q.backend().library()[1].toMap()["id"].toString();
    QTest::keyClick(q.window, 'v');
    // The library closes and the batch of variations starts, from that image.
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), false);
    QTRY_VERIFY(q.backend().running());
    QCOMPARE(q.backend().source()["key"].toString(), id);
    QTRY_VERIFY_WITH_TIMEOUT(!q.backend().running(), 40000);
    QCOMPARE(Library(q.rig.sb.library()).find(qmlDraft(q, 0)["id"].toString()).metadata["source"].toObject()["id"].toString(), id);
    // V in an empty library does nothing.
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_keepSetDiscardDoNothingWhileTheLibraryIsOpen() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    for (const char key : {'k', 's', 'd'})
        QTest::keyClick(q.window, key);
    QTest::qWait(200);
    // Checked before Ctrl+Z: that would undo (cancel) a keep that had started, and hide it.
    QVERIFY(!q.backend().keeping());
    QVERIFY(!q.backend().canUndo());
    QTest::keyClick(q.window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(100);
    QVERIFY(!q.backend().canRedo());
    for (int i = 0; i < 4; ++i)
        QVERIFY(!qmlDraft(q, i)["kept"].toBool() && !qmlDraft(q, i)["discarded"].toBool());
    QVERIFY(q.rig.sb.fixture.omarchyCalls().isEmpty());
    QCOMPARE(upscaleCalls(q.rig.sb.fakeLog()).size(), 0);
    // Closed, the same keys act on the grid.
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), false);
    QTest::keyClick(q.window, 'd');
    QTRY_VERIFY(qmlDraft(q, 0)["discarded"].toBool());
    NO_QML_WARNINGS(q);
}

void LimgenTests::qml_escChainOrder() {
    QmlRig q(true, fakeEngine, true, 8);
    LOAD_WINDOW(q);
    QVERIFY(qmlBatch(q));
    giveTheWindowBackgrounds(q, {"a.png"});
    QTRY_COMPARE(q.backend().backgrounds().size(), 1);
    // A variation running (it hangs), the library open with an image full size, and the help over it all.
    qputenv("FAKE_SD_CLI_MODE", "hang");
    QTest::keyClick(q.window, 'b');
    QTRY_VERIFY(!q.backend().source().isEmpty());
    QTest::keyClick(q.window, Qt::Key_Space);
    QTRY_VERIFY(q.backend().running());
    QTRY_VERIFY_WITH_TIMEOUT(imageCalls(q.rig.sb.fakeLog()).size() >= 2, 15000);
    QTest::keyClick(q.window, 'l');
    QTRY_COMPARE(q.prop("libraryVisible").toBool(), true);
    QTest::keyClick(q.window, 'f');
    QTRY_COMPARE(q.prop("fullView").toBool(), true);
    QTest::keyClick(q.window, '?');
    QTRY_COMPARE(q.prop("helpVisible").toBool(), true);
    auto state = [&] {
        return QString("help=%1 full=%2 library=%3 typing=%4 running=%5 source=%6")
            .arg(q.prop("helpVisible").toBool()).arg(q.prop("fullView").toBool()).arg(q.prop("libraryVisible").toBool())
            .arg(q.typing()).arg(q.backend().running()).arg(!q.backend().source().isEmpty());
    };
    // 1: the help.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(state(), QString("help=0 full=1 library=1 typing=0 running=1 source=1"), 3000);
    // 2: the full view.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(state(), QString("help=0 full=0 library=1 typing=0 running=1 source=1"), 3000);
    // 3: the library.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(state(), QString("help=0 full=0 library=0 typing=0 running=1 source=1"), 3000);
    // 4: a text field.
    q.focusSubject();
    QTRY_VERIFY(q.typing());
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(state(), QString("help=0 full=0 library=0 typing=0 running=1 source=1"), 3000);
    // 5: the running batch.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(state(), QString("help=0 full=0 library=0 typing=0 running=0 source=1"), 15000);
    // 6: the source, last.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(state(), QString("help=0 full=0 library=0 typing=0 running=0 source=0"), 3000);
    // Nothing left: Esc does nothing.
    QTest::keyClick(q.window, Qt::Key_Escape);
    QTest::qWait(100);
    QCOMPARE(state(), QString("help=0 full=0 library=0 typing=0 running=0 source=0"));
    NO_QML_WARNINGS(q);
}

// The child of backend_destroyingWhileABatchRunsIsSafe: a Backend on the
// environment it was given, a batch started, then the Backend deleted.
int destroyBackendWhileBatchRuns(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    const OmarchyPaths paths = OmarchyPaths::fromEnvironment();
    Omarchy omarchy(paths);
    auto backend = std::make_unique<Backend>(&omarchy, fakeEngine);
    auto waitFor = [](const std::function<bool()> &condition) {
        for (int i = 0; i < 600 && !condition(); ++i)
            QTest::qWait(50);
        return condition();
    };
    if (!waitFor([&] { return backend->engineReady(); }))
        return 10;
    backend->setSubject("oil rig");
    backend->generate();
    if (!backend->running())
        return 11;
    const QString log = qEnvironmentVariable("FAKE_SD_CLI_LOG");
    if (!waitFor([&] { return !imageCalls(log).isEmpty(); }))
        return 12;
    QTest::qWait(300);
    backend.reset(); // the batch is still running
    printf("destroyed\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc > 1 && QByteArray(argv[1]) == "--child-destroy-backend")
        return destroyBackendWhileBatchRuns(argc, argv);
    QGuiApplication app(argc, argv);
    LimgenTests tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "limgen_tests.moc"
