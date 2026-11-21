#include "batch.h"

#include "timings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <algorithm>

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
        emit sampled(image);
    });
    connect(engine, &Engine::preview, this, &Batch::preview);
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
    const QString themeSlug = omarchy.slug();
    Palette palette;
    palette.error = tr("No Omarchy theme");
    if (!themeSlug.isEmpty()) {
        QString error;
        const QString text = omarchy.readColors(&error);
        if (error.isEmpty())
            palette = Palette::fromToml(text);
    }
    Prompt::Text text = Prompt::build(settings.subject, palette, settings.style);
    if (!settings.fullPrompt.trimmed().isEmpty())
        text.positive = settings.fullPrompt.trimmed();
    m_seed = settings.seed ? *settings.seed : QRandomGenerator::global()->bounded(1u << 31);

    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/limgen";
    m_request = EngineRequest{};
    for (auto it = m_level->files.begin(); it != m_level->files.end(); ++it)
        if (it.key() != "tae" || settings.previews)
            m_request.files.insert(it.key(), Catalogue::path(*m_catalogue.file(it.value()), modelsDirectory));
    m_request.prompt = text.positive;
    m_request.negative = text.negative;
    m_request.width = m_model->width;
    m_request.height = m_model->height;
    m_request.steps = m_model->steps;
    m_request.cfg = m_model->cfg;
    m_request.sampler = m_model->sampler;
    m_request.seed = m_seed;
    m_request.count = settings.count;
    m_request.previews = settings.previews;
    m_request.outputDirectory =
        QString("%1/batch-%2").arg(cache).arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));

    m_record = DraftRecord{};
    m_record.themeSlug = palette.isValid() ? themeSlug : QString();
    m_record.themeMode = palette.isValid() ? palette.mode : QString();
    for (const auto &colour : palette.colors)
        m_record.palette.insert(colour.role, colour.hex);
    m_record.subject = settings.subject;
    m_record.prompt = m_request.prompt;
    m_record.negative = m_request.cfg > 1.0 ? m_request.negative : QString();
    m_record.style = Prompt::styleId(settings.style);
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
    return Outcome::Ok;
}

QString Batch::timingKey() const {
    return m_model ? Timings::key(m_model->id, m_level->id, m_backend) : QString();
}

double Batch::estimate() const {
    if (!m_model)
        return -1;
    return Timings().get(timingKey()).estimate(m_request.count, m_request.steps);
}

void Batch::start() {
    if (m_running || !m_model)
        return;
    m_running = true;
    m_stepSeconds.clear();
    m_lastSampledMs = -1;
    m_clock.start();
    m_engine->start(m_request);
}

void Batch::cancel() {
    if (m_running)
        m_engine->cancel();
}

void Batch::engineProgress(int image, int step, int steps, double secondsPerStep) {
    // The first step of each draft also pays for copying weights to the GPU;
    // it counts as overhead, not as the speed of a step.
    if (step > 1 && secondsPerStep > 0)
        m_stepSeconds << secondsPerStep;
    emit progress(image, step, steps, secondsPerStep);
}

void Batch::engineFinished(const QStringList &drafts) {
    recordTiming();
    Library library;
    QList<LibraryEntry> entries;
    for (int i = 0; i < drafts.size(); ++i) {
        DraftRecord record = m_record;
        record.seed = m_seed + quint32(i);
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

void Batch::recordTiming() {
    // After an out-of-memory retry the clock covers both attempts, which says
    // nothing about how fast a batch runs here.
    if (m_stepSeconds.isEmpty() || m_lastSampledMs < 0 || m_engine->retried())
        return;
    QList<double> sorted = m_stepSeconds;
    std::sort(sorted.begin(), sorted.end());
    const double perStep = sorted.at(sorted.size() / 2); // the median, so one slow step does not skew it
    const double total = m_clock.elapsed() / 1000.0;
    const double sampledAt = m_lastSampledMs / 1000.0;
    Timing measured;
    measured.secondsPerStep = perStep;
    measured.overheadSeconds = qMax(0.0, sampledAt - m_request.count * m_request.steps * perStep);
    measured.decodeSecondsPerImage = qMax(0.0, (total - sampledAt) / m_request.count);
    Timings().record(timingKey(), measured);
}

void Batch::cleanUp() { QDir(m_request.outputDirectory).removeRecursively(); }
