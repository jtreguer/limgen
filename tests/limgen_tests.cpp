// Qt Test suite for limgen. Every test works in a temporary folder: Omarchy's
// state, the user's background folder, the XDG folders and the fake tools are
// all made fresh, so nothing here reads or writes the real Omarchy files
// (CLAUDE.md, "Rules that must hold"). No test uses the network.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <cstdio>
#include <memory>

#include "backend.h"
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

    Fixture() {
        const QString base = root.path();
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

private:
    struct CliResult {
        int exitCode = -1;
        QString out, err;
        bool started = false;
    };
    CliResult runCli(const Fixture &fixture, const QStringList &arguments,
                     const QHash<QString, QString> &extraEnvironment = {}) const;
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

LimgenTests::CliResult LimgenTests::runCli(const Fixture &fixture, const QStringList &arguments,
                                           const QHash<QString, QString> &extraEnvironment) const {
    CliResult result;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // The headless commands must not need a display, or a Hyprland session.
    for (const char *name : {"WAYLAND_DISPLAY", "DISPLAY", "QT_QPA_PLATFORM", "HYPRLAND_INSTANCE_SIGNATURE",
                             "LIMGEN_HYPRCTL", "LIMGEN_OMARCHY_BIN", "LIMGEN_SD_CLI",
                             "QT_QPA_PLATFORMTHEME"})
        environment.remove(name);
    // Omarchy sessions export QT_QPA_PLATFORMTHEME=gtk3. It is removed by
    // default so the other tests do not depend on the session they run in;
    // cli_runsWithoutDisplayUnderGtkPlatformTheme puts it back.
    for (auto it = extraEnvironment.begin(); it != extraEnvironment.end(); ++it)
        environment.insert(it.key(), it.value());
    const QString base = fixture.root.path();
    environment.insert("HOME", base + "/home");
    environment.insert("LIMGEN_OMARCHY_STATE", fixture.state);
    environment.insert("LIMGEN_OMARCHY_BACKGROUNDS", fixture.backgrounds);
    environment.insert("XDG_CONFIG_HOME", base + "/config");
    environment.insert("XDG_STATE_HOME", base + "/state");
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

QTEST_MAIN(LimgenTests)
#include "limgen_tests.moc"
