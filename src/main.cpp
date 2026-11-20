// limgen: generates desktop backgrounds for the current Omarchy theme with an
// image model on the local GPU.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "omarchy.h"
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
    const bool window = argc == 1 || (argc == 2 && first == "open");
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

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator(paths.language);
    app.installTranslator(&translator);

    if (!window)
        return Cli::run(arguments, paths);

    QQuickStyle::setStyle("Material");
    Theme theme(paths.state, nullptr, paths.hyprctl);
    Omarchy omarchy(paths);
    Backend backend(&omarchy);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
