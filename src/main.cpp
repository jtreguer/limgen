// limgen: generates images on the local GPU with an image model: backgrounds
// for the current Omarchy theme, or anything else at any size.

#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "omarchy.h"
#include "studio.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // The window is `limgen open`, or limgen with no command. Everything else
    // is a command that draws nothing, so it must not need a display; the
    // platform has to be chosen before QGuiApplication exists. `open` with
    // anything after it (--help, a mistake) is answered as a command too.
    const QByteArray first = argc > 1 ? QByteArray(argv[1]) : QByteArray();
    // QGuiApplication removes the options it knows (-style, -platform,
    // -reverse...) from argv, even spelled with two dashes, which would
    // silently swallow a command's own --style. Commands get the arguments
    // as they were typed.
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);
    // `limgen open generic` and `limgen open theme` skip the home screen,
    // for a key binding that goes straight to one.
    const QByteArray screen = argc == 3 && first == "open" ? QByteArray(argv[2]) : QByteArray();
    const bool window = argc == 1 || (argc == 2 && first == "open") || screen == "generic" || screen == "theme";
    // Omarchy sessions also set QT_QPA_PLATFORMTHEME=gtk3, whose plugin opens
    // a display of its own, so a command run over SSH or from a script with no
    // display would fail; commands draw nothing and need no platform theme.
    if (!window) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qunsetenv("QT_QPA_PLATFORMTHEME");
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("limgen");
    app.setApplicationVersion("0.1.0");
    // Matches the window to limgen.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("limgen");
    app.setWindowIcon(QIcon::fromTheme("limgen"));

    const OmarchyPaths paths = OmarchyPaths::fromEnvironment();

    // Commands speak English, whatever the desktop's language: scripts and
    // agents read their output, as with Omarchy's other apps. Their numbers
    // too, rather than the system locale's ("6.7 GB", not "6,7 GB").
    if (!window) {
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        return Cli::run(arguments, paths);
    }

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator(paths.language);
    app.installTranslator(&translator);

    QQuickStyle::setStyle("Material");
    Theme theme(paths.state, nullptr, paths.hyprctl);
    Omarchy omarchy(paths);
    Backend backend(&omarchy);
    Studio studio(&backend);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("studio", &studio);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (!screen.isEmpty())
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openScreen",
                                  Q_ARG(QVariant, QVariant(QString::fromLatin1(screen))));

    return app.exec();
}
