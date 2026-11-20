#include "cli.h"

#include "catalogue.h"
#include "hardware.h"
#include "library.h"
#include "omarchy.h"
#include "palette.h"
#include "prompt.h"
#include "sdcppengine.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>

namespace Cli {

namespace {

enum Exit { Ok = 0, Error = 1, Usage = 2, NotRunnable = 3, Interrupted = 130 };

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

    // Accepts between `least` and `most` arguments after the command;
    // `most` defaults to `least`.
    bool parse(const QStringList &arguments, int least = 0, int most = -1) {
        if (most < least)
            most = least;
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
        const int given = parser.positionalArguments().size() - 1;
        if (given > most) {
            status = fail(tr("Unexpected argument: %1").arg(parser.positionalArguments().value(most + 1)) + "\n" +
                              parser.helpText().trimmed(),
                          Usage);
            return false;
        }
        if (given < least) {
            status = fail(tr("Missing argument.") + "\n" + parser.helpText().trimmed(), Usage);
            return false;
        }
        return true;
    }
    bool json() const { return parser.isSet("json"); }
    QString argument(int index) const { return parser.positionalArguments().value(index + 1); }
};

// Set by SIGINT or SIGTERM; a timer in the event loop turns it into a
// cancel, since almost nothing is safe to do inside a signal handler.
std::atomic<bool> interrupted{false};
extern "C" void onInterrupt(int) { interrupted = true; }

QString describe(const CatalogueFile &file) { return file.folder + "/" + file.name; }

QString memoryText(const Gpu &gpu) {
    if (gpu.totalMiB < 0)
        return tr("%1, memory unknown").arg(gpu.name);
    return tr("%1, %2 of %3 MiB free").arg(gpu.name).arg(gpu.freeMiB).arg(gpu.totalMiB);
}

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

int doctor(const QStringList &arguments) {
    Command command("doctor", tr("Check the engine, the GPU and the installed models, and say what is missing."));
    command.parser.addOption({"verify", tr("Also check each installed file's SHA-256 (reads several gigabytes)")});
    if (!command.parse(arguments))
        return command.status;
    const bool verify = command.parser.isSet("verify");

    QStringList problems;
    const SdCppEngine engine;
    const EngineProbe probe = engine.probe();
    if (probe.error.kind != EngineError::None)
        problems << probe.error.message;
    const QList<Gpu> gpus = Hardware::detect();
    const Gpu gpu = Hardware::matching(gpus, probe.gpu().description);

    const Catalogue catalogue = Catalogue::builtIn();
    const QString modelsDirectory = Catalogue::defaultModelsDirectory();
    QJsonArray models;
    QStringList modelLines;
    bool defaultRunnable = false;
    for (const CatalogueModel &model : catalogue.models()) {
        const ModelLevel &level = model.levels.first();
        QStringList missing;
        for (const QString &key : catalogue.missingFiles(level, modelsDirectory, verify))
            missing << describe(*catalogue.file(key));
        const bool memory = gpu.freeMiB < 0 || gpu.freeMiB >= level.minFreeMiB;
        const bool runnable = missing.isEmpty() && memory && probe.error.kind == EngineError::None;
        if (model.isDefault)
            defaultRunnable = runnable;
        if (model.isDefault && !missing.isEmpty())
            problems << tr("The default model, %1, is not installed.").arg(model.name);
        if (model.isDefault && missing.isEmpty() && !memory)
            problems << tr("%1 needs %2 MiB of free video memory; %3.")
                            .arg(model.name).arg(level.minFreeMiB).arg(memoryText(gpu));
        models << QJsonObject{{"id", model.id},
                              {"name", model.name},
                              {"default", model.isDefault},
                              {"level", level.id},
                              {"installed", missing.isEmpty()},
                              {"missing", QJsonArray::fromStringList(missing)},
                              {"minFreeMiB", level.minFreeMiB},
                              {"runnable", runnable}};
        const QString state = !missing.isEmpty() ? tr("not installed; missing %1").arg(missing.join(", "))
                              : !memory          ? tr("installed; needs %1 MiB free").arg(level.minFreeMiB)
                              : runnable         ? tr("ready")
                                                 : tr("installed");
        modelLines << QString("  %1 %2").arg(model.id + (model.isDefault ? tr(" (default)") : QString()), -26)
                          .arg(state);
    }
    const CatalogueFile *upscaler = catalogue.upscaler();
    const bool upscalerInstalled =
        upscaler && Catalogue::state(*upscaler, modelsDirectory, verify) == Catalogue::FileState::Ok;

    if (command.json()) {
        QJsonArray devices, gpuList;
        for (const auto &device : probe.devices)
            devices << QJsonObject{{"name", device.name}, {"description", device.description}};
        for (const auto &g : gpus)
            gpuList << QJsonObject{{"vendor", g.vendor}, {"name", g.name},
                                   {"totalMiB", double(g.totalMiB)}, {"freeMiB", double(g.freeMiB)}};
        print({{"ok", defaultRunnable},
               {"engine", QJsonObject{{"path", engine.program()},
                                      {"version", probe.version},
                                      {"pinned", SdCppEngine::pinnedVersion},
                                      {"backend", probe.gpu().backend()},
                                      {"devices", devices}}},
               {"gpus", gpuList},
               {"modelsDirectory", modelsDirectory},
               {"models", models},
               {"upscaler", QJsonObject{{"installed", upscalerInstalled}}},
               {"problems", QJsonArray::fromStringList(problems)}});
        return defaultRunnable ? Ok : NotRunnable;
    }
    print(stdout, tr("Engine:   %1").arg(engine.program()));
    if (!probe.version.isEmpty())
        print(stdout, tr("          %1, %2").arg(probe.version,
                                                 probe.gpu().name.isEmpty() ? tr("no GPU device")
                                                                            : probe.gpu().name + " (" +
                                                                                  probe.gpu().description + ")"));
    print(stdout, tr("GPU:      %1").arg(gpu.name.isEmpty() ? tr("none found") : memoryText(gpu)));
    print(stdout, tr("Models:   %1").arg(modelsDirectory));
    for (const QString &line : modelLines)
        print(stdout, line);
    print(stdout, tr("Upscaler: %1").arg(upscalerInstalled ? tr("installed") : tr("not installed")));
    print(stdout, problems.isEmpty() ? tr("Problems: none") : tr("Problems:"));
    for (const QString &problem : problems)
        print(stdout, "  " + problem);
    return defaultRunnable ? Ok : NotRunnable;
}

int generate(const QStringList &arguments, const OmarchyPaths &paths) {
    Command command("generate", tr("Make drafts for the current theme and add them to the library. Prints "
                                   "each draft's id and path."));
    command.parser.addPositionalArgument("subject", tr("What the background shows, such as \"oil rig at dusk\""),
                                         "[subject]");
    command.parser.addOption({"count", tr("How many drafts, 1 to 8 (default 4)"), "n", "4"});
    command.parser.addOption({"style", tr("photo, cinematic, painting or minimal (default cinematic)"), "style",
                              Prompt::styleId(Prompt::defaultStyle)});
    command.parser.addOption({"model", tr("A model id from limgen doctor (default: the default model)"), "id"});
    command.parser.addOption({"seed", tr("The first draft's seed; the others count up from it"), "n"});
    command.parser.addOption({"prompt", tr("The full prompt, used as given instead of the one built from the "
                                           "subject and the theme"), "text"});
    if (!command.parse(arguments, 0, 1))
        return command.status;

    const QString subject = command.argument(0).trimmed();
    const QString fullPrompt = command.parser.value("prompt").trimmed();
    if (subject.isEmpty() && fullPrompt.isEmpty())
        return fail(tr("Give a subject, or a full prompt with --prompt."), Usage);
    bool numberOk = false;
    const int count = command.parser.value("count").toInt(&numberOk);
    if (!numberOk || count < 1 || count > 8)
        return fail(tr("--count takes a number from 1 to 8."), Usage);
    Prompt::Style style;
    if (!Prompt::styleFromId(command.parser.value("style"), &style))
        return fail(tr("--style takes one of: %1.").arg(Prompt::styleIds().join(", ")), Usage);
    quint32 seed = QRandomGenerator::global()->bounded(1u << 31);
    if (command.parser.isSet("seed")) {
        seed = command.parser.value("seed").toUInt(&numberOk);
        // Each next draft's seed is one more, and the last must fit too.
        const quint32 highest = 4294967295u - quint32(count - 1);
        if (!numberOk || seed > highest)
            return fail(tr("--seed takes a whole number from 0 to %1 for %2 drafts.").arg(highest).arg(count),
                        Usage);
    }

    const Catalogue catalogue = Catalogue::builtIn();
    if (!catalogue.isValid())
        return fail(tr("The built-in model catalogue is broken; this is a bug in limgen."));
    const CatalogueModel *model = command.parser.isSet("model") ? catalogue.model(command.parser.value("model"))
                                                                : catalogue.defaultModel();
    if (!model) {
        QStringList ids;
        for (const auto &m : catalogue.models())
            ids << m.id;
        return fail(tr("--model takes one of: %1.").arg(ids.join(", ")), Usage);
    }
    const ModelLevel &level = model->levels.first();
    const QString modelsDirectory = Catalogue::defaultModelsDirectory();
    // Commands draw no previews, so the preview decoder is not required.
    const QStringList missing = catalogue.missingFiles(level, modelsDirectory, false, {"tae"});
    if (!missing.isEmpty()) {
        QStringList names;
        for (const QString &key : missing)
            names << describe(*catalogue.file(key));
        return fail(tr("%1 is not installed. Missing in %2: %3").arg(model->name, modelsDirectory, names.join(", ")),
                    NotRunnable);
    }

    SdCppEngine engine;
    const EngineProbe probe = engine.probe();
    if (probe.error.kind != EngineError::None)
        return fail(probe.error.message, NotRunnable);
    const Gpu gpu = Hardware::matching(Hardware::detect(), probe.gpu().description);
    if (gpu.freeMiB >= 0 && gpu.freeMiB < level.minFreeMiB)
        return fail(tr("%1 needs %2 MiB of free video memory; %3. Close programs that use the GPU and try again.")
                        .arg(model->name).arg(level.minFreeMiB).arg(memoryText(gpu)),
                    NotRunnable);

    // The theme is read once: a theme switch during the batch does not
    // change what the batch was made for.
    const Omarchy omarchy(paths);
    const QString themeSlug = omarchy.slug();
    Palette palette;
    palette.error = tr("No Omarchy theme");
    if (!themeSlug.isEmpty()) {
        QString error;
        const QString text = omarchy.readColors(&error);
        if (error.isEmpty())
            palette = Palette::fromToml(text);
    }
    Prompt::Text text = Prompt::build(subject, palette, style);
    if (!fullPrompt.isEmpty())
        text.positive = fullPrompt;

    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/limgen";
    EngineRequest request;
    for (auto it = level.files.begin(); it != level.files.end(); ++it)
        if (it.key() != "tae")
            request.files.insert(it.key(), Catalogue::path(*catalogue.file(it.value()), modelsDirectory));
    request.prompt = text.positive;
    request.negative = text.negative;
    request.width = model->width;
    request.height = model->height;
    request.steps = model->steps;
    request.cfg = model->cfg;
    request.sampler = model->sampler;
    request.seed = seed;
    request.count = count;
    request.outputDirectory = QString("%1/batch-%2").arg(cache).arg(QRandomGenerator::global()->generate(), 8, 16,
                                                                    QChar('0'));

    QStringList drafts;
    EngineError error;
    QEventLoop loop;
    QObject::connect(&engine, &Engine::sampled, &loop, [&](int image) {
        if (!command.json())
            print(stderr, tr("Draft %1 of %2 sampled").arg(image + 1).arg(count));
    });
    QObject::connect(&engine, &Engine::finished, &loop, [&](const QStringList &paths) {
        drafts = paths;
        loop.quit();
    });
    QObject::connect(&engine, &Engine::failed, &loop, [&](const EngineError &e) {
        error = e;
        loop.quit();
    });
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &engine, [&] {
        if (interrupted)
            engine.cancel();
    });
    // The handlers stay until generate returns, so a Ctrl+C while drafts are
    // copied into the library cannot leave a half-copied file or the cache
    // folder behind.
    struct Handlers {
        Handlers() {
            interrupted = false;
            std::signal(SIGINT, onInterrupt);
            std::signal(SIGTERM, onInterrupt);
        }
        ~Handlers() {
            std::signal(SIGINT, SIG_DFL);
            std::signal(SIGTERM, SIG_DFL);
        }
    } handlers;
    watch.start(100);
    engine.start(request);
    loop.exec();
    watch.stop();

    auto cleanUp = [&] { QDir(request.outputDirectory).removeRecursively(); };
    // Ctrl+C in a terminal also reaches the engine, which can die of it
    // before limgen cancels; however the batch ended, the user asked to stop.
    if (interrupted || error.kind == EngineError::Cancelled) {
        cleanUp();
        return fail(tr("Interrupted; no drafts were kept."), Interrupted);
    }
    if (error.kind != EngineError::None) {
        cleanUp();
        QString message = error.message;
        if (error.kind == EngineError::OutOfMemory && !gpu.name.isEmpty())
            message += " " + tr("(%1, also after retrying with tiled decoding.)").arg(memoryText(gpu));
        if (!error.detail.isEmpty())
            message += "\n" + error.detail;
        return fail(message, error.kind == EngineError::OutOfMemory ? NotRunnable : Error);
    }

    DraftRecord record;
    record.themeSlug = palette.isValid() ? themeSlug : QString();
    record.themeMode = palette.isValid() ? palette.mode : QString();
    for (const auto &colour : palette.colors)
        record.palette.insert(colour.role, colour.hex);
    record.subject = subject;
    record.prompt = request.prompt;
    record.negative = request.cfg > 1.0 ? request.negative : QString();
    record.style = Prompt::styleId(style);
    record.modelId = model->id;
    record.level = level.id;
    for (auto it = level.files.begin(); it != level.files.end(); ++it)
        if (it.key() != "tae")
            record.filesSha256 << catalogue.file(it.value())->sha256;
    record.engineVersion = probe.version;
    record.engineBackend = probe.gpu().backend();
    record.steps = request.steps;
    record.cfg = request.cfg;
    record.sampler = request.sampler;
    record.width = request.width;
    record.height = request.height;

    Library library;
    QJsonArray list;
    QStringList lines;
    for (int i = 0; i < drafts.size(); ++i) {
        record.seed = seed + quint32(i);
        QString addError;
        const LibraryEntry entry = library.addDraft(drafts.at(i), record, QDateTime::currentDateTimeUtc(), &addError);
        if (!entry.isValid()) {
            cleanUp();
            return fail(addError);
        }
        list << QJsonObject{{"id", entry.id}, {"path", entry.image}, {"seed", double(record.seed)}};
        lines << entry.id + "  " + entry.image;
    }
    cleanUp();

    if (command.json()) {
        print({{"model", model->id},
               {"level", level.id},
               {"theme", record.themeSlug.isEmpty() ? QJsonValue() : QJsonValue(record.themeSlug)},
               {"style", record.style},
               {"prompt", record.prompt},
               {"seed", double(seed)},
               {"drafts", list}});
        return Ok;
    }
    for (const QString &line : lines)
        print(stdout, line);
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
              "  doctor            The engine, the GPU, the installed models, and what is missing\n"
              "  generate          Make drafts for the current theme\n"
              "  help [command]    This summary, or one command's options\n"
              "  --version         The version");
}

bool isCommand(const QString &word) {
    return QStringList{"open", "palette", "doctor", "generate", "help", "--help", "-h", "--version"}.contains(word);
}

int run(const QStringList &arguments, const OmarchyPaths &paths) {
    const QString command = arguments.value(1);
    if (command == "open")
        return open(arguments);
    if (command == "palette")
        return palette(arguments, paths);
    if (command == "doctor")
        return doctor(arguments);
    if (command == "generate")
        return generate(arguments, paths);
    if (command == "help" || command == "--help" || command == "-h")
        return help(command == "help" ? arguments : QStringList{arguments.value(0), "help"}, paths);
    if (command == "--version") {
        print(stdout, "limgen " + QCoreApplication::applicationVersion());
        return Ok;
    }
    return fail(tr("Unknown command: %1. Run limgen help for the list.").arg(command), Usage);
}

} // namespace Cli
