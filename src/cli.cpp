#include "cli.h"

#include "batch.h"
#include "catalogue.h"
#include "config.h"
#include "hardware.h"
#include "keeper.h"
#include "library.h"
#include "modelstore.h"
#include "omarchy.h"
#include "palette.h"
#include "prompt.h"
#include "sdcppengine.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLockFile>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <unistd.h>

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

// Installed for the length of a long command, so Ctrl+C becomes a clean stop
// (a cancel, then exit 130) instead of leaving half-written files behind.
struct InterruptHandlers {
    InterruptHandlers() {
        interrupted = false;
        std::signal(SIGINT, onInterrupt);
        std::signal(SIGTERM, onInterrupt);
    }
    ~InterruptHandlers() {
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);
    }
};

QString sizeText(qint64 bytes) { return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat); }

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

    QString catalogueError;
    const Catalogue catalogue = Catalogue::fromEnvironment(&catalogueError);
    if (!catalogue.isValid())
        return fail(catalogueError);
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
            problems << tr("The default model, %1, is not installed. Run limgen models pull %2.")
                            .arg(model.name, model.id);
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
    command.parser.addOption({"from", tr("Make variations of this image: a library id or an image file"),
                              "image"});
    command.parser.addOption({"strength", tr("How far variations move from the image, 0.1 to 0.9 (default 0.5)"),
                              "n", "0.5"});
    if (!command.parse(arguments, 0, 1))
        return command.status;

    const QString subject = command.argument(0).trimmed();
    const QString fullPrompt = command.parser.value("prompt").trimmed();
    const QString from = command.parser.value("from");
    // A variation needs nothing typed: it keeps the image's look, and a
    // library image brings its own prompt.
    if (subject.isEmpty() && fullPrompt.isEmpty() && from.isEmpty())
        return fail(tr("Give a subject, a full prompt with --prompt, or an image with --from."), Usage);
    if (command.parser.isSet("strength") && from.isEmpty())
        return fail(tr("--strength goes with --from."), Usage);
    bool strengthOk = false;
    const double strength = command.parser.value("strength").toDouble(&strengthOk);
    if (!strengthOk || !(strength >= Batch::minStrength && strength <= Batch::maxStrength))
        return fail(tr("--strength takes a number from 0.1 to 0.9."), Usage);
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

    QString catalogueError;
    const Catalogue catalogue = Catalogue::fromEnvironment(&catalogueError);
    if (!catalogue.isValid())
        return fail(catalogueError);
    if (command.parser.isSet("model") && !catalogue.model(command.parser.value("model"))) {
        QStringList ids;
        for (const auto &m : catalogue.models())
            ids << m.id;
        return fail(tr("--model takes one of: %1.").arg(ids.join(", ")), Usage);
    }

    BatchSettings settings;
    settings.modelId = command.parser.value("model");
    settings.subject = subject;
    settings.style = style;
    settings.fullPrompt = fullPrompt;
    settings.count = count;
    settings.seed = seed;
    settings.source = from;
    settings.strength = strength;
    Batch batch(catalogue, paths);
    QString message;
    const Batch::Outcome prepared = batch.prepare(settings, SdCppEngine().probe(), &message);
    if (prepared != Batch::Outcome::Ok)
        return fail(message, prepared == Batch::Outcome::UnknownModel || prepared == Batch::Outcome::UnknownSource
                                 ? Usage
                                 : NotRunnable);

    QList<LibraryEntry> entries;
    Batch::Outcome outcome = Batch::Outcome::Ok;
    QEventLoop loop;
    QObject::connect(&batch, &Batch::sampled, &loop, [&](int image) {
        if (!command.json())
            print(stderr, tr("Draft %1 of %2 sampled").arg(image + 1).arg(count));
    });
    QObject::connect(&batch, &Batch::finished, &loop, [&](const QList<LibraryEntry> &filed) {
        entries = filed;
        loop.quit();
    });
    QObject::connect(&batch, &Batch::failed, &loop, [&](Batch::Outcome failure, const QString &text) {
        outcome = failure;
        message = text;
        loop.quit();
    });
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &batch, [&] {
        if (interrupted)
            batch.cancel();
    });
    // The handlers stay until generate returns, so a Ctrl+C while drafts are
    // copied into the library cannot leave a half-copied file or the cache
    // folder behind.
    const InterruptHandlers handlers;
    watch.start(100);
    batch.start();
    loop.exec();
    watch.stop();

    // Ctrl+C in a terminal also reaches the engine, which can die of it
    // before limgen cancels; however the run ended, the user asked to stop.
    // Drafts already filed are reported as they are.
    if (outcome == Batch::Outcome::Cancelled || (outcome != Batch::Outcome::Ok && interrupted))
        return fail(tr("Interrupted; no drafts were kept."), Interrupted);
    if (outcome != Batch::Outcome::Ok)
        return fail(message, outcome == Batch::Outcome::NotRunnable ? NotRunnable : Error);

    if (command.json()) {
        QJsonArray list;
        for (int i = 0; i < entries.size(); ++i)
            list << QJsonObject{{"id", entries.at(i).id},
                                {"path", entries.at(i).image},
                                {"seed", double(seed + quint32(i))}};
        print({{"model", batch.model()->id},
               {"level", batch.model()->levels.first().id},
               {"theme", batch.themeSlug().isEmpty() ? QJsonValue() : QJsonValue(batch.themeSlug())},
               {"style", batch.styleId()},
               {"prompt", batch.request().prompt},
               {"seed", double(seed)},
               {"source", batch.source().isEmpty() ? QJsonValue() : QJsonValue(batch.source())},
               {"drafts", list}});
        return Ok;
    }
    for (const LibraryEntry &entry : entries)
        print(stdout, entry.id + "  " + entry.image);
    return Ok;
}

// Finishes one draft and adds it to a theme's background folder, for keep
// and set. Prints nothing on success; the caller reports the result.
int runKeep(const KeepSettings &settings, const OmarchyPaths &paths, bool json, KeepResult *result) {
    QString catalogueError;
    const Catalogue catalogue = Catalogue::fromEnvironment(&catalogueError);
    if (!catalogue.isValid())
        return fail(catalogueError);
    Keeper keeper(catalogue, paths);
    QString message;
    const Keeper::Outcome prepared = keeper.prepare(settings, SdCppEngine().probe(), &message);
    if (prepared != Keeper::Outcome::Ok)
        return fail(message, prepared == Keeper::Outcome::UnknownDraft || prepared == Keeper::Outcome::UnknownTheme
                                 ? Usage
                                 : prepared == Keeper::Outcome::NotRunnable ? NotRunnable : Error);
    if (!json && !keeper.result().alreadyKept)
        print(stderr, tr("Finishing %1 at %2x%3")
                          .arg(settings.draftId)
                          .arg(keeper.finalSize().width())
                          .arg(keeper.finalSize().height()));

    Keeper::Outcome outcome = Keeper::Outcome::Ok;
    QEventLoop loop;
    QObject::connect(&keeper, &Keeper::finished, &loop, [&](const KeepResult &kept) {
        *result = kept;
        loop.quit();
    });
    QObject::connect(&keeper, &Keeper::failed, &loop, [&](Keeper::Outcome failure, const QString &text) {
        outcome = failure;
        message = text;
        loop.quit();
    });
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &keeper, [&] {
        if (interrupted)
            keeper.cancel();
    });
    const InterruptHandlers handlers;
    watch.start(100);
    keeper.start();
    loop.exec();
    watch.stop();

    if (outcome == Keeper::Outcome::Cancelled || (outcome != Keeper::Outcome::Ok && interrupted))
        return fail(tr("Interrupted; nothing was kept."), Interrupted);
    if (outcome != Keeper::Outcome::Ok)
        return fail(message, outcome == Keeper::Outcome::NotRunnable ? NotRunnable : Error);
    return Ok;
}

QJsonObject keptJson(const KeepResult &kept) {
    return {{"draft", kept.draft.id},
            {"final", kept.final.isValid() ? QJsonValue(kept.final.id) : QJsonValue()},
            {"path", kept.installed},
            {"theme", kept.theme.isEmpty() ? QJsonValue() : QJsonValue(kept.theme)},
            {"alreadyKept", kept.alreadyKept}};
}

int keep(const QStringList &arguments, const OmarchyPaths &paths) {
    Command command("keep", tr("Finish a draft (upscale, colour pass) and add it to a theme's backgrounds. Prints "
                               "the path of the image added."));
    command.parser.addPositionalArgument("id", tr("A draft id, as limgen generate prints it"), "<id>");
    command.parser.addOption({"theme", tr("The theme to add it to (default: the theme it was made for)"), "slug"});
    command.parser.addOption({"color-pass", tr("Colour pass strength from 0 (off) to 1 (default from the "
                                               "settings, 0.3 at first)"),
                              "strength"});
    command.parser.addOption({"format", tr("png or jpeg (default from the settings, png at first)"), "format"});
    command.parser.addOption({"set", tr("Also show it as the background now")});
    if (!command.parse(arguments, 1))
        return command.status;

    const Config config;
    KeepSettings settings;
    settings.draftId = command.argument(0);
    settings.colorPass = config.colorPass();
    settings.format = config.format();
    settings.jpegQuality = config.jpegQuality();
    if (command.parser.isSet("color-pass")) {
        bool numberOk = false;
        settings.colorPass = command.parser.value("color-pass").toDouble(&numberOk);
        // Written this way round so "nan" fails too.
        if (!numberOk || !(settings.colorPass >= 0 && settings.colorPass <= 1))
            return fail(tr("--color-pass takes a number from 0 to 1."), Usage);
    }
    if (command.parser.isSet("format")) {
        settings.format = command.parser.value("format");
        if (settings.format != "png" && settings.format != "jpeg")
            return fail(tr("--format takes png or jpeg."), Usage);
    }
    // A script cannot be asked which theme it meant, so a draft goes to the
    // theme it was made for, whose palette it matches; --theme overrides.
    const Omarchy omarchy(paths);
    const LibraryEntry draft = Library().find(settings.draftId);
    settings.theme = command.parser.isSet("theme") ? command.parser.value("theme")
                                                   : draft.metadata.value("theme").toObject().value("slug").toString();
    if (settings.theme.isEmpty())
        settings.theme = omarchy.slug();

    KeepResult kept;
    if (const int status = runKeep(settings, paths, command.json(), &kept); status != Ok)
        return status;
    bool set = false;
    if (command.parser.isSet("set")) {
        QString error;
        if (!omarchy.setBackground(kept.installed, &error))
            return fail(error);
        set = true;
    }
    if (command.json()) {
        QJsonObject object = keptJson(kept);
        object.insert("set", set);
        print(object);
        return Ok;
    }
    if (kept.alreadyKept)
        print(stderr, tr("Already kept."));
    print(stdout, kept.installed);
    return Ok;
}

int set(const QStringList &arguments, const OmarchyPaths &paths) {
    Command command("set", tr("Show an image as the background now. A draft is kept first, into the current "
                              "theme."));
    command.parser.addPositionalArgument("image", tr("A draft or final id, or an image file"), "<id-or-file>");
    if (!command.parse(arguments, 1))
        return command.status;

    const Omarchy omarchy(paths);
    const QString given = command.argument(0);
    QString file;
    QJsonObject object;
    if (Library::isValidId(given)) {
        const LibraryEntry entry = Library().find(given);
        if (!entry.isValid())
            return fail(tr("No draft or final %1 in the library.").arg(given), Usage);
        // A final is set through its draft, so it lands in the current
        // theme's folder like any kept draft (or is found there already).
        const bool isFinal = entry.metadata.value("kind").toString() == "final";
        const QString draftId =
            isFinal ? entry.metadata.value("finish").toObject().value("from").toString() : entry.id;
        if (isFinal && !Library().find(draftId).isValid())
            return fail(tr("The draft %1 that %2 was finished from is no longer in the library. Set the final's "
                           "file instead: %3")
                            .arg(draftId, given, entry.image));
        const Config config;
        KeepSettings settings;
        settings.draftId = draftId;
        settings.theme = omarchy.slug();
        settings.colorPass = config.colorPass();
        settings.format = config.format();
        settings.jpegQuality = config.jpegQuality();
        KeepResult kept;
        if (const int status = runKeep(settings, paths, command.json(), &kept); status != Ok)
            return status;
        file = kept.installed;
        object = keptJson(kept);
    } else {
        const QFileInfo info(given);
        if (!info.isFile())
            return fail(tr("No draft, final or file %1.").arg(given), Usage);
        file = info.absoluteFilePath();
        object.insert("path", file);
    }
    QString error;
    if (!omarchy.setBackground(file, &error))
        return fail(error);
    if (command.json()) {
        object.insert("set", true);
        print(object);
        return Ok;
    }
    print(stdout, file);
    return Ok;
}

int library(const QStringList &arguments) {
    Command command("library", tr("List the drafts and finished images in the library, newest first."));
    command.parser.addOption({"theme", tr("Only those made for this theme"), "slug"});
    if (!command.parse(arguments))
        return command.status;
    const QString theme = command.parser.value("theme");
    QJsonArray list;
    QStringList lines;
    for (const LibraryEntry &entry : Library().list()) {
        const QString made = entry.metadata.value("theme").toObject().value("slug").toString();
        if (command.parser.isSet("theme") && made != theme)
            continue;
        const QString kind = entry.metadata.value("kind").toString();
        QJsonObject object{{"id", entry.id},
                           {"kind", kind},
                           {"created", entry.metadata.value("created")},
                           {"theme", made.isEmpty() ? QJsonValue() : QJsonValue(made)},
                           {"subject", entry.metadata.value("subject")},
                           {"path", entry.image}};
        if (kind == "final")
            object.insert("from", entry.metadata.value("finish").toObject().value("from"));
        list << object;
        lines << QStringList{entry.id, kind, made.isEmpty() ? "-" : made, entry.image}.join("  ");
    }
    if (command.json()) {
        fputs(QJsonDocument(list).toJson().constData(), stdout);
        return Ok;
    }
    if (!lines.isEmpty())
        print(stdout, lines.join('\n'));
    return Ok;
}

int modelsList(bool json) {
    QString catalogueError;
    const Catalogue catalogue = Catalogue::fromEnvironment(&catalogueError);
    if (!catalogue.isValid())
        return fail(catalogueError);
    const QString modelsDirectory = Catalogue::defaultModelsDirectory();
    const ModelStore store(modelsDirectory);
    const Gpu gpu = Hardware::matching(Hardware::detect(), SdCppEngine().probe().gpu().description);

    QJsonArray list;
    QStringList lines{tr("Models in %1:").arg(modelsDirectory)};
    for (const CatalogueModel &model : catalogue.models()) {
        QJsonArray levels;
        for (const ModelLevel &level : model.levels) {
            const ModelStore::Plan plan = store.plan(catalogue, level, false);
            const bool installed = plan.files.isEmpty();
            // Free memory now, as generate checks it; unknown on Intel.
            const QJsonValue runsHere =
                gpu.freeMiB < 0 ? QJsonValue() : QJsonValue(gpu.freeMiB >= level.minFreeMiB);
            levels << QJsonObject{{"id", level.id},
                                  {"installed", installed},
                                  {"downloadBytes", double(plan.downloadBytes())},
                                  {"minFreeMiB", level.minFreeMiB},
                                  {"runsHere", runsHere}};
            const QString state = installed ? tr("installed") : tr("not installed (%1)").arg(sizeText(plan.downloadBytes()));
            const QString fit = runsHere.isNull() ? tr("memory unknown")
                                : runsHere.toBool() ? tr("runs here")
                                                    : tr("needs %1 MiB of free video memory").arg(level.minFreeMiB);
            lines << QString("  %1 %2").arg(model.id + (model.isDefault ? tr(" (default)") : QString()), -26)
                         .arg(QString("%1, %2, %3").arg(model.name, model.publisher, model.licenceId));
            lines << QString("  %1 %2").arg(QString(), -26).arg(QString("%1 %2; %3").arg(level.id, state, fit));
        }
        list << QJsonObject{{"id", model.id},
                            {"name", model.name},
                            {"publisher", model.publisher},
                            {"default", model.isDefault},
                            {"licence", QJsonObject{{"id", model.licenceId},
                                                    {"summary", model.licenceSummary},
                                                    {"url", model.licenceUrl}}},
                            {"levels", levels}};
    }
    if (json) {
        print({{"modelsDirectory", modelsDirectory}, {"models", list}});
        return Ok;
    }
    for (const QString &line : lines)
        print(stdout, line);
    return Ok;
}

int modelsPull(const QString &id, const QString &levelId, bool yes, bool json) {
    QString catalogueError;
    const Catalogue catalogue = Catalogue::fromEnvironment(&catalogueError);
    if (!catalogue.isValid())
        return fail(catalogueError);
    const CatalogueModel *model = catalogue.model(id);
    if (!model) {
        QStringList ids;
        for (const auto &m : catalogue.models())
            ids << m.id;
        return fail(tr("Unknown model %1. Models: %2.").arg(id, ids.join(", ")), Usage);
    }
    const ModelLevel *level = &model->levels.first();
    if (!levelId.isEmpty()) {
        level = nullptr;
        QStringList levels;
        for (const auto &l : model->levels) {
            levels << l.id;
            if (l.id == levelId)
                level = &l;
        }
        if (!level)
            return fail(tr("%1 has no level %2. Levels: %3.").arg(model->id, levelId, levels.join(", ")), Usage);
    }

    const QString modelsDirectory = Catalogue::defaultModelsDirectory();
    ModelStore store(modelsDirectory);
    const ModelStore::Plan plan = store.plan(catalogue, *level);
    auto report = [&](const QStringList &installed) {
        if (json)
            print({{"model", model->id}, {"level", level->id}, {"installed", QJsonArray::fromStringList(installed)}});
        else
            for (const QString &path : installed)
                print(stdout, path);
    };
    if (plan.files.isEmpty()) {
        if (!json)
            print(stderr, tr("%1 is already installed.").arg(model->name));
        report({});
        return Ok;
    }

    // What the user agrees to: the licence, every file and its size, and the
    // disk space it takes (docs/design.md section 5). It goes to stderr, so
    // stdout stays clean for scripts.
    print(stderr, tr("%1 by %2").arg(model->name, model->publisher));
    print(stderr, tr("Licence: %1. %2").arg(model->licenceId, model->licenceSummary));
    print(stderr, model->licenceUrl);
    print(stderr, tr("Files for %1 (%2), into %3:").arg(model->id, level->id, modelsDirectory));
    for (const CatalogueFile &file : plan.files)
        print(stderr, QString("  %1 %2  %3").arg(file.folder + "/" + file.name, -44).arg(sizeText(file.bytes), 9)
                          .arg(file.licence));
    QString total = tr("Download %1").arg(sizeText(plan.downloadBytes()));
    if (plan.partBytes > 0)
        total += tr(" (%1 already downloaded)").arg(sizeText(plan.partBytes));
    if (plan.availableBytes >= 0)
        total += tr("; %1 free.").arg(sizeText(plan.availableBytes));
    print(stderr, total);
    if (!plan.fitsOnDisk())
        return fail(tr("Not enough disk space: %1 needs %2 plus %3 to spare.")
                        .arg(modelsDirectory, sizeText(plan.downloadBytes()), sizeText(ModelStore::headroomBytes)),
                    NotRunnable);

    if (!yes) {
        if (!isatty(STDIN_FILENO))
            return fail(tr("Nothing downloaded. Add --yes to download without a prompt."), Usage);
        fprintf(stderr, "%s", qPrintable(tr("Download? [y/N] ")));
        fflush(stderr);
        char answer[16] = {};
        const QString reply = fgets(answer, sizeof answer, stdin) ? QString::fromLocal8Bit(answer).trimmed().toLower()
                                                                  : QString();
        if (reply != "y" && reply != "yes")
            return fail(tr("Nothing downloaded."));
    }

    // Two downloads into the same .part file would corrupt it.
    QDir().mkpath(modelsDirectory);
    QLockFile lock(modelsDirectory + "/.download.lock");
    if (!lock.tryLock(0))
        return fail(tr("Another limgen is downloading into %1; try again when it is done.").arg(modelsDirectory));
    // Another limgen may have installed files while this one waited at the
    // prompt; only what is still missing is fetched.
    const ModelStore::Plan current = store.plan(catalogue, *level);
    if (current.files.isEmpty()) {
        if (!json)
            print(stderr, tr("%1 is already installed.").arg(model->name));
        report({});
        return Ok;
    }

    QStringList installed;
    QString error;
    bool cancelled = false;
    const bool live = isatty(STDERR_FILENO);
    QElapsedTimer shown;
    QEventLoop loop;
    QObject::connect(&store, &ModelStore::progress, &loop, [&](qint64 done, qint64 total, const QString &file) {
        if (json || !live || (shown.isValid() && shown.elapsed() < 500))
            return;
        shown.start();
        fprintf(stderr, "\r%s\033[K", qPrintable(tr("%1: %2 of %3").arg(file, sizeText(done), sizeText(total))));
        fflush(stderr);
    });
    QObject::connect(&store, &ModelStore::fileInstalled, &loop, [&](const QString &key) {
        const QString path = Catalogue::path(*catalogue.file(key), modelsDirectory);
        installed << path;
        if (!json)
            fprintf(stderr, "%s%s\n", live ? "\r\033[K" : "", qPrintable(tr("Checked and installed %1").arg(path)));
    });
    QObject::connect(&store, &ModelStore::finished, &loop, &QEventLoop::quit);
    QObject::connect(&store, &ModelStore::failed, &loop, [&](const QString &message) {
        error = message;
        loop.quit();
    });
    QObject::connect(&store, &ModelStore::cancelled, &loop, [&] {
        cancelled = true;
        loop.quit();
    });
    const InterruptHandlers handlers;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &store, [&] {
        if (interrupted)
            store.cancel();
    });
    watch.start(100);
    store.start(current.files);
    loop.exec();
    watch.stop();
    if (live && !json)
        fprintf(stderr, "\r\033[K");

    // A failure or a finished download is reported as it is, even when a
    // Ctrl+C arrived too late to stop it.
    if (!error.isEmpty())
        return fail(error);
    if (cancelled)
        return fail(tr("Paused. Run the same command to resume the download."), Interrupted);
    report(installed);
    return Ok;
}

int models(const QStringList &arguments) {
    Command command("models", tr("List the models limgen can run, or download one with limgen models pull <id>."));
    command.parser.addPositionalArgument("action", tr("list (the default) or pull"), "[list|pull]");
    command.parser.addPositionalArgument("id", tr("For pull: the model to download"), "[id]");
    command.parser.addOption({"level", tr("For pull: the quantisation level (default: the model's first)"), "id"});
    command.parser.addOption({"yes", tr("For pull: download without asking; the licence is still printed")});
    if (!command.parse(arguments, 0, 2))
        return command.status;
    const QString action = command.argument(0).isEmpty() ? QString("list") : command.argument(0);
    if (action == "list") {
        if (!command.argument(1).isEmpty() || command.parser.isSet("level") || command.parser.isSet("yes"))
            return fail(tr("limgen models list takes no model and no --level or --yes.") + "\n" +
                            command.parser.helpText().trimmed(),
                        Usage);
        return modelsList(command.json());
    }
    if (action == "pull") {
        if (command.argument(1).isEmpty())
            return fail(tr("Name the model to download: limgen models pull <id>."), Usage);
        return modelsPull(command.argument(1), command.parser.value("level"), command.parser.isSet("yes"),
                          command.json());
    }
    return fail(tr("Unknown action %1: use list or pull.").arg(action) + "\n" + command.parser.helpText().trimmed(),
                Usage);
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
              "  keep <id>         Finish a draft and add it to the theme's backgrounds\n"
              "  library           The drafts and finished images limgen made\n"
              "  set <id-or-file>  Show an image as the background now\n"
              "  models [list]     The models limgen can run, installed or not\n"
              "  models pull <id>  Download a model, after showing its licence and size\n"
              "  help [command]    This summary, or one command's options\n"
              "  --version         The version");
}

bool isCommand(const QString &word) {
    return QStringList{"open", "palette", "doctor", "generate", "keep", "set", "library",
                       "models", "help", "--help", "-h", "--version"}
        .contains(word);
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
    if (command == "keep")
        return keep(arguments, paths);
    if (command == "set")
        return set(arguments, paths);
    if (command == "library")
        return library(arguments);
    if (command == "models")
        return models(arguments);
    if (command == "help" || command == "--help" || command == "-h")
        return help(command == "help" ? arguments : QStringList{arguments.value(0), "help"}, paths);
    if (command == "--version") {
        print(stdout, "limgen " + QCoreApplication::applicationVersion());
        return Ok;
    }
    return fail(tr("Unknown command: %1. Run limgen help for the list.").arg(command), Usage);
}

} // namespace Cli
