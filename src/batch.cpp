#include "batch.h"

#include "resample.h"
#include "timings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QImageReader>
#include <QRandomGenerator>
#include <QTimer>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Batch", text); }

QString memoryText(const Gpu &gpu) {
    if (gpu.totalMiB < 0)
        return tr("%1, memory unknown").arg(gpu.name);
    return tr("%1, %2 of %3 MiB free").arg(gpu.name).arg(gpu.freeMiB).arg(gpu.totalMiB);
}

} // namespace

Batch::Batch(const Catalogue &catalogue, const OmarchyPaths &paths, const QString &engineProgram, QObject *parent)
    : QObject(parent), m_catalogue(catalogue), m_paths(paths),
      m_engine(std::make_unique<SdCppEngine>(engineProgram)) {
    SdCppEngine *engine = m_engine.get();
    connect(engine, &Engine::progress, this, &Batch::engineProgress);
    connect(engine, &Engine::sampled, this, [this](int image) {
        m_lastSampledMs = m_clock.elapsed();
        emit sampled(draftIndex(image));
    });
    connect(engine, &Engine::preview, this,
            [this](int image, const QString &path) { emit preview(draftIndex(image), path); });
    connect(engine, &Engine::finished, this, &Batch::engineFinished);
    connect(engine, &Engine::failed, this, &Batch::engineFailed);
}

Batch::~Batch() {
    // Destroying the engine stops a running process, and the engine reports
    // that as a failure. This batch is half destroyed by then, so it stops
    // listening first. The engine goes before the batch folder is removed,
    // so nothing writes into it afterwards; the folder is of no use to
    // anyone once the batch is gone.
    m_engine->disconnect(this);
    m_engine.reset();
    if (m_running)
        cleanUp();
}

Batch::Outcome Batch::prepare(const BatchSettings &settings, const EngineProbe &probe, QString *message) {
    auto refuse = [message](Outcome outcome, const QString &text) {
        if (message)
            *message = text;
        return outcome;
    };
    m_model = settings.modelId.isEmpty() ? m_catalogue.defaultModel() : m_catalogue.model(settings.modelId);
    if (!m_model) {
        QStringList ids;
        for (const auto &model : m_catalogue.models())
            ids << model.id;
        return refuse(Outcome::UnknownModel, tr("Unknown model %1. Models: %2.").arg(settings.modelId, ids.join(", ")));
    }
    m_level = &m_model->levels.first();
    const QString modelsDirectory = Catalogue::defaultModelsDirectory();
    // Without previews the tiny preview decoder is not needed.
    const QStringList skip = settings.previews ? QStringList() : QStringList{"tae"};
    const QStringList missing = m_catalogue.missingFiles(*m_level, modelsDirectory, false, skip);
    if (!missing.isEmpty()) {
        QStringList names;
        for (const QString &key : missing)
            names << m_catalogue.file(key)->folder + "/" + m_catalogue.file(key)->name;
        return refuse(Outcome::NotRunnable, tr("%1 is not installed. Missing in %2: %3. Run limgen models pull %4.")
                                                .arg(m_model->name, modelsDirectory, names.join(", "), m_model->id));
    }
    // A variation's source: a library image, whose prompt it can reuse, or
    // any image file.
    m_sourceImage.clear();
    QJsonObject source;
    QString sourcePrompt;
    if (!settings.source.isEmpty()) {
        if (Library::isValidId(settings.source)) {
            const LibraryEntry entry = Library().find(settings.source);
            if (!entry.isValid())
                return refuse(Outcome::UnknownSource, tr("No image %1 in the library.").arg(settings.source));
            m_sourceImage = entry.image;
            sourcePrompt = entry.metadata.value("prompt").toString();
            source.insert("id", entry.id);
        } else {
            m_sourceImage = QFileInfo(settings.source).absoluteFilePath();
            source.insert("path", m_sourceImage);
        }
        if (!QImageReader(m_sourceImage).canRead())
            return refuse(Outcome::UnknownSource, tr("Cannot read the image %1.").arg(settings.source));
    }

    if (probe.error.kind != EngineError::None)
        return refuse(Outcome::NotRunnable, probe.error.message);
    m_gpu = Hardware::matching(Hardware::detect(), probe.gpu().description);
    if (m_gpu.freeMiB >= 0 && m_gpu.freeMiB < m_level->minFreeMiB)
        return refuse(Outcome::NotRunnable,
                      tr("%1 needs %2 MiB of free video memory; %3. Close programs that use the GPU and try again.")
                          .arg(m_model->name)
                          .arg(m_level->minFreeMiB)
                          .arg(memoryText(m_gpu)));
    m_backend = probe.gpu().backend();

    const Omarchy omarchy(m_paths);
    QString themeSlug;
    Palette palette;
    palette.error = tr("No Omarchy theme");
    QJsonObject generic;
    m_target = QSize();
    if (settings.generic) {
        // The palette the user chose, if any; the current theme plays no part.
        QJsonValue chosen;
        if (!settings.paletteColors.isEmpty()) {
            palette = Palette::fromColors(settings.paletteColors);
            if (!palette.isValid())
                return refuse(Outcome::UnknownPalette, palette.error);
            chosen = QJsonObject{{"theme", QJsonValue()}};
        } else if (!settings.paletteTheme.isEmpty()) {
            const QString folder = omarchy.themeDirectory(settings.paletteTheme);
            QFile colors(folder + "/colors.toml");
            if (folder.isEmpty() || !colors.open(QIODevice::ReadOnly))
                return refuse(Outcome::UnknownPalette, tr("No theme %1 with a colors.toml in %2.")
                                                           .arg(settings.paletteTheme, m_paths.themes.join(", ")));
            palette = Palette::fromToml(QString::fromUtf8(colors.readAll()));
            if (!palette.isValid())
                return refuse(Outcome::UnknownPalette, palette.error);
            chosen = QJsonObject{{"theme", settings.paletteTheme}};
        }
        if (chosen.isObject()) {
            QJsonObject object = chosen.toObject(), roles;
            for (const auto &colour : palette.colors)
                roles.insert(colour.role, colour.hex);
            object.insert("colors", roles);
            chosen = object;
        }
        m_target = settings.target.isEmpty() ? omarchy.largestMonitorSize() : settings.target;
        generic = QJsonObject{{"target", QJsonArray{m_target.width(), m_target.height()}},
                              {"palette", chosen}};
    } else {
        themeSlug = omarchy.slug();
        if (!themeSlug.isEmpty()) {
            QString error;
            const QString text = omarchy.readColors(&error);
            if (error.isEmpty())
                palette = Palette::fromToml(text);
        }
    }
    Prompt::Text text = settings.generic ? Prompt::buildGeneric(settings.subject, palette)
                                         : Prompt::build(settings.subject, palette, settings.style);
    if (!settings.fullPrompt.trimmed().isEmpty())
        text.positive = settings.fullPrompt.trimmed();
    // A variation of a library image with nothing typed says again what
    // the image was made from.
    else if (settings.subject.trimmed().isEmpty() && !sourcePrompt.isEmpty())
        text.positive = sourcePrompt;
    m_seed = settings.seed ? *settings.seed : QRandomGenerator::global()->bounded(1u << 31);
    // Variety gives each draft of a generic batch its own hint, and so its
    // own prompt. An edited full prompt is taken as it is, for every draft,
    // so it has no hints, and the sidecar says the variety was seeds.
    const Prompt::Variety variety = settings.generic && settings.fullPrompt.trimmed().isEmpty()
                                        ? settings.variety
                                        : Prompt::Variety::Seeds;
    if (settings.generic)
        generic.insert("variety", Prompt::varietyId(variety));
    m_hints = Prompt::varietyHints(variety, settings.count, m_seed);
    m_prompts.clear();
    for (const QString &hint : std::as_const(m_hints))
        m_prompts << (hint.isEmpty() ? text.positive : Prompt::buildGeneric(settings.subject, palette, hint).positive);
    m_perDraft = m_prompts.size() > 1 && m_prompts.count(m_prompts.first()) != m_prompts.size();

    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/limgen";
    m_request = EngineRequest{};
    for (auto it = m_level->files.begin(); it != m_level->files.end(); ++it)
        if (it.key() != "tae" || settings.previews)
            m_request.files.insert(it.key(), Catalogue::path(*m_catalogue.file(it.value()), modelsDirectory));
    // One run for the batch makes every draft from one prompt: the first
    // draft's, which carries its hint when a single draft has one.
    m_request.prompt = m_perDraft || m_prompts.isEmpty() ? text.positive : m_prompts.first();
    m_request.negative = text.negative;
    const QSize size = draftSize(m_target, QSize(m_model->width, m_model->height));
    m_request.width = size.width();
    m_request.height = size.height();
    m_request.steps = m_model->steps;
    m_request.cfg = m_model->cfg;
    m_request.sampler = m_model->sampler;
    m_request.seed = m_seed;
    m_request.count = settings.count;
    m_request.previews = settings.previews;
    m_request.outputDirectory =
        QString("%1/batch-%2").arg(cache).arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
    if (!m_sourceImage.isEmpty()) {
        // A model with few steps needs at least one to change anything, so
        // a lower strength is raised to that; the sidecar records the one
        // used. The file is made at start().
        m_request.initImage = m_request.outputDirectory + "/source.png";
        m_request.strength = strengthFor(m_request.steps, settings.strength);
        source.insert("strength", m_request.strength);
    }

    m_record = DraftRecord{};
    // A generic draft records its palette under "generic", not as a theme.
    m_record.themeSlug = palette.isValid() ? themeSlug : QString();
    m_record.themeMode = palette.isValid() && !settings.generic ? palette.mode : QString();
    if (!settings.generic)
        for (const auto &colour : palette.colors)
            m_record.palette.insert(colour.role, colour.hex);
    m_record.subject = settings.subject;
    m_record.prompt = m_request.prompt;
    m_record.negative = m_request.cfg > 1.0 ? m_request.negative : QString();
    m_record.style = settings.generic ? QString() : Prompt::styleId(settings.style);
    m_record.generic = generic;
    m_record.modelId = m_model->id;
    m_record.level = m_level->id;
    for (auto it = m_level->files.begin(); it != m_level->files.end(); ++it)
        if (it.key() != "tae")
            m_record.filesSha256 << m_catalogue.file(it.value())->sha256;
    m_record.engineVersion = probe.version;
    m_record.engineBackend = m_backend;
    m_record.steps = m_request.steps;
    m_record.cfg = m_request.cfg;
    m_record.sampler = m_request.sampler;
    m_record.width = m_request.width;
    m_record.height = m_request.height;
    m_record.source = source;
    return Outcome::Ok;
}

double Batch::strengthFor(int steps, double strength) {
    strength = qBound(minStrength, strength, maxStrength);
    EngineRequest request;
    request.steps = steps;
    request.initImage = "source";
    request.strength = strength;
    if (steps > 0 && request.samplingSteps() < 2)
        strength = std::min(maxStrength, std::max(strength, 1.0001 / steps));
    return strength;
}

QSize Batch::draftSize(const QSize &target, const QSize &native) {
    if (target.isEmpty() || native.isEmpty())
        return native;
    const double pixels = double(native.width()) * native.height();
    const double ratio = double(target.width()) / target.height();
    auto snap = [](double side) { return qBound(256, int(std::lround(side / 64.0)) * 64, 2048); };
    return QSize(snap(std::sqrt(pixels * ratio)), snap(std::sqrt(pixels / ratio)));
}

QString Batch::timingKey() const {
    return m_model ? Timings::key(m_model->id, m_level->id, m_backend) : QString();
}

double Batch::estimate() const {
    if (!m_model)
        return -1;
    const Timing timing = Timings().get(timingKey());
    // Each run pays the start, the loading and the prompt again.
    if (m_perDraft) {
        const double one = timing.estimate(1, m_request.samplingSteps());
        return one > 0 ? one * m_request.count : -1;
    }
    return timing.estimate(m_request.count, m_request.samplingSteps());
}

EngineRequest Batch::runRequest(int index) const {
    EngineRequest request = m_request;
    request.count = 1;
    request.seed = m_seed + quint32(index);
    request.prompt = m_prompts.value(index, m_request.prompt);
    request.outputDirectory = QString("%1/run-%2").arg(m_request.outputDirectory).arg(index);
    return request;
}

void Batch::startRun() {
    if (!m_running)
        return;
    if (m_stopping) {
        cleanUp();
        m_running = false;
        emit failed(Outcome::Cancelled, tr("Cancelled."));
        return;
    }
    // Each run is timed on its own: the first one is a batch of one.
    m_stepSeconds.clear();
    m_lastSampledMs = -1;
    m_clock.start();
    m_engine->start(runRequest(m_run));
}

void Batch::start() {
    if (m_running || !m_model)
        return;
    m_running = true;
    m_stopping = false;
    m_run = 0;
    m_runDrafts.clear();
    m_stepSeconds.clear();
    m_lastSampledMs = -1;
    m_clock.start();
    if (!m_request.initImage.isEmpty()) {
        // The engine takes the source at the drafts' size; covering it here
        // keeps its centre rather than squeezing it.
        const QImage source = Resample::cover(QImage(m_sourceImage), QSize(m_request.width, m_request.height));
        if (source.isNull() || !QDir().mkpath(m_request.outputDirectory) || !source.save(m_request.initImage)) {
            // Reported from the event loop, as an engine failure would be.
            QTimer::singleShot(0, this, [this] {
                if (!m_running)
                    return;
                cleanUp();
                m_running = false;
                emit failed(Outcome::Failed, tr("Cannot prepare the image %1 for the variation.").arg(m_sourceImage));
            });
            return;
        }
    }
    if (m_perDraft)
        startRun();
    else
        m_engine->start(m_request);
}

void Batch::cancel() {
    if (!m_running)
        return;
    // Between two runs no engine is running; the next run sees the flag.
    m_stopping = true;
    if (m_engine->isRunning())
        m_engine->cancel();
}

void Batch::engineProgress(int image, int step, int steps, double secondsPerStep) {
    // The first step of each draft also pays for copying weights to the GPU;
    // it counts as overhead, not as the speed of a step.
    if (step > 1 && secondsPerStep > 0)
        m_stepSeconds << secondsPerStep;
    emit progress(draftIndex(image), step, steps, secondsPerStep);
}

void Batch::engineFinished(const QStringList &drafts) {
    if (!m_perDraft) {
        recordTiming(m_request.count);
        fileDrafts(drafts);
        return;
    }
    if (m_run == 0)
        recordTiming(1);
    m_runDrafts << drafts.value(0);
    if (++m_run < m_request.count) {
        // From the event loop: the engine is still inside its own signal.
        QTimer::singleShot(0, this, &Batch::startRun);
        return;
    }
    fileDrafts(m_runDrafts);
}

void Batch::fileDrafts(const QStringList &drafts) {
    Library library;
    QList<LibraryEntry> entries;
    for (int i = 0; i < drafts.size(); ++i) {
        DraftRecord record = m_record;
        record.seed = m_seed + quint32(i);
        record.prompt = m_prompts.value(i, m_record.prompt);
        if (!record.generic.isEmpty() && !m_hints.value(i).isEmpty())
            record.generic.insert("hint", m_hints.at(i));
        QString error;
        const LibraryEntry entry = library.addDraft(drafts.at(i), record, QDateTime::currentDateTimeUtc(), &error);
        if (!entry.isValid()) {
            cleanUp();
            m_running = false;
            emit failed(Outcome::Failed, error);
            return;
        }
        entries << entry;
    }
    cleanUp();
    m_running = false;
    emit finished(entries);
}

void Batch::engineFailed(const EngineError &error) {
    cleanUp();
    m_running = false;
    if (error.kind == EngineError::Cancelled) {
        emit failed(Outcome::Cancelled, error.message);
        return;
    }
    QString message = error.message;
    if (error.kind == EngineError::OutOfMemory && !m_gpu.name.isEmpty())
        message += " " + tr("(%1, also after retrying with tiled decoding.)").arg(memoryText(m_gpu));
    if (!error.detail.isEmpty())
        message += "\n" + error.detail;
    const bool machine = error.kind == EngineError::OutOfMemory || error.kind == EngineError::EngineMissing ||
                         error.kind == EngineError::NoGpu || error.kind == EngineError::WrongVersion;
    emit failed(machine ? Outcome::NotRunnable : Outcome::Failed, message);
}

void Batch::recordTiming(int count) {
    // After an out-of-memory retry the clock covers both attempts, which says
    // nothing about how fast a batch runs here. A variation runs fewer steps
    // and encodes its source first, so it is not a measure of a batch either.
    if (m_stepSeconds.isEmpty() || m_lastSampledMs < 0 || m_engine->retried() || !m_request.initImage.isEmpty())
        return;
    QList<double> sorted = m_stepSeconds;
    std::sort(sorted.begin(), sorted.end());
    const double perStep = sorted.at(sorted.size() / 2); // the median, so one slow step does not skew it
    const double total = m_clock.elapsed() / 1000.0;
    const double sampledAt = m_lastSampledMs / 1000.0;
    Timing measured;
    measured.secondsPerStep = perStep;
    measured.overheadSeconds = qMax(0.0, sampledAt - count * m_request.steps * perStep);
    measured.decodeSecondsPerImage = qMax(0.0, (total - sampledAt) / count);
    Timings().record(timingKey(), measured);
}

void Batch::cleanUp() { QDir(m_request.outputDirectory).removeRecursively(); }
