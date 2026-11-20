#include "cli.h"

#include "omarchy.h"
#include "palette.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <cstdio>

namespace Cli {

namespace {

enum Exit { Ok = 0, Error = 1, Usage = 2, NotRunnable = 3 };

QString tr(const char *text) { return QCoreApplication::translate("Cli", text); }

void print(FILE *stream, const QString &text) { fprintf(stream, "%s\n", qPrintable(text)); }
void print(const QJsonObject &object) { fputs(QJsonDocument(object).toJson().constData(), stdout); }

// Two decimals are far finer than any colour difference anyone can see.
double rounded(double value) { return std::round(value * 100) / 100; }

int fail(const QString &message, int code = Error) {
    print(stderr, message);
    return code;
}

// A parser for one command. QCommandLineParser::process() would exit with
// status 1 on a bad option, but usage errors exit 2, so parse() is used and
// the result handled here.
struct Command {
    QCommandLineParser parser;
    int status = -1; // set when the command should stop: help printed or a usage error

    Command(const QString &name, const QString &description, bool json = true) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
        if (json)
            parser.addOption({"json", tr("Print the result as JSON")});
    }

    bool parse(const QStringList &arguments, int positionals = 0) {
        if (!parser.parse(arguments)) {
            status = fail(parser.errorText() + "\n" + parser.helpText().trimmed(), Usage);
            return false;
        }
        if (parser.isSet("help")) {
            print(stdout, parser.helpText().trimmed());
            status = Ok;
            return false;
        }
        // The first positional argument is the command itself.
        if (parser.positionalArguments().size() != positionals + 1) {
            status = fail(tr("Unexpected argument: %1").arg(parser.positionalArguments().value(positionals + 1)) +
                              "\n" + parser.helpText().trimmed(),
                          Usage);
            return false;
        }
        return true;
    }
    bool json() const { return parser.isSet("json"); }
};

// Reached only when `open` has something after it; plain `limgen open` opens
// the window in main() without coming here.
int open(const QStringList &arguments) {
    Command command("open", tr("Open the window. Takes no arguments."), false);
    if (!command.parse(arguments))
        return command.status;
    return fail(tr("limgen open takes no options.") + "\n" + command.parser.helpText().trimmed(), Usage);
}

int palette(const QStringList &arguments, const OmarchyPaths &paths) {
    Command command("palette", tr("Print the current theme's palette and the words it becomes in prompts."));
    if (!command.parse(arguments))
        return command.status;

    const Omarchy omarchy(paths);
    if (!omarchy.present())
        return fail(tr("No Omarchy theme found in %1.").arg(paths.state));
    QString error;
    const QString text = omarchy.readColors(&error);
    if (!error.isEmpty())
        return fail(error);
    const Palette palette = Palette::fromToml(text);
    if (!palette.isValid())
        return fail(omarchy.colorsPath() + ": " + palette.error);

    if (command.json()) {
        QJsonArray colors;
        for (const auto &colour : palette.colors)
            colors << QJsonObject{{"role", colour.role},
                                  {"hex", colour.hex},
                                  {"words", colour.words},
                                  {"lab", QJsonArray{rounded(colour.lab[0]), rounded(colour.lab[1]),
                                                     rounded(colour.lab[2])}}};
        print({{"theme", omarchy.slug()},
               {"mode", palette.mode},
               {"colors", colors},
               {"words", QJsonArray::fromStringList(palette.words)},
               {"mood", palette.mood}});
        return Ok;
    }
    print(stdout, QString("%1 (%2)").arg(omarchy.slug(), palette.mode));
    for (const auto &colour : palette.colors)
        print(stdout, QString("  %1 %2  %3").arg(colour.role, -11).arg(colour.hex, colour.words));
    print(stdout, tr("Words: %1").arg(palette.words.join(", ")));
    print(stdout, tr("Mood: %1").arg(palette.mood));
    return Ok;
}

int help(const QStringList &arguments, const OmarchyPaths &paths) {
    const QString topic = arguments.value(2);
    if (isCommand(topic) && topic != "help")
        return run({arguments.value(0), topic, "--help"}, paths);
    if (!topic.isEmpty())
        return fail(tr("Unknown command: %1. Run limgen help for the list.").arg(topic), Usage);
    print(stdout, tr("Usage: limgen <command> [options]") + "\n\n" + summary());
    return Ok;
}

} // namespace

QString summary() {
    return tr("Window:\n"
              "  open              Open the window (also limgen with no command)\n\n"
              "Commands that need no display (limgen help <command> for options):\n"
              "  palette           The current theme's palette and the words it becomes\n"
              "  help [command]    This summary, or one command's options\n"
              "  --version         The version");
}

bool isCommand(const QString &word) {
    return QStringList{"open", "palette", "help", "--help", "-h", "--version"}.contains(word);
}

int run(const QStringList &arguments, const OmarchyPaths &paths) {
    const QString command = arguments.value(1);
    if (command == "open")
        return open(arguments);
    if (command == "palette")
        return palette(arguments, paths);
    if (command == "help" || command == "--help" || command == "-h")
        return help(command == "help" ? arguments : QStringList{arguments.value(0), "help"}, paths);
    if (command == "--version") {
        print(stdout, "limgen " + QCoreApplication::applicationVersion());
        return Ok;
    }
    return fail(tr("Unknown command: %1. Run limgen help for the list.").arg(command), Usage);
}

} // namespace Cli
