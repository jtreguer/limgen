#include "backend.h"

#include "omarchy.h"
#include "prompt.h"
#include "timings.h"

#include <QDir>
#include <QLocale>
#include <QUrl>
#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace {

QString sizeText(qint64 bytes) { return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat); }

} // namespace

Backend::Backend(Omarchy *omarchy, const QString &engineProgram, QObject *parent)
    : QObject(parent), m_omarchy(omarchy), m_engineProgram(engineProgram),
      m_modelsDirectory(Catalogue::defaultModelsDirectory()), m_store(m_modelsDirectory) {
    m_catalogue = Catalogue::fromEnvironment(&m_catalogueError);
    m_modelId = m_config.modelId();
    if (!m_catalogue.model(m_modelId))
        m_modelId = m_catalogue.defaultModel() ? m_catalogue.defaultModel()->id : QString();
    Prompt::styleFromId(m_config.styleId(), &m_style);

    connect(m_omarchy, &Omarchy::changed, this, &Backend::reload);
    connect(&m_probing, &QFutureWatcher<Probe>::finished, this, [this] {
        const Probe probe = m_probing.result();
        m_probe = probe.engine;
        m_gpu = Hardware::matching(probe.gpus, m_probe.gpu().description);
        m_probed = true;
        emit engineChanged();
        emit stateChanged();
        updateEstimate();
    });

    connect(&m_store, &ModelStore::progress, this, [this](qint64 done, qint64 total, const QString &) {
        m_downloadDone = done;
        m_downloadTotal = total;
        emit downloadChanged();
    });
    connect(&m_store, &ModelStore::finished, this, [this] {
        m_downloadLock.reset();
        refreshModel();
        setStatus(tr("%1 is installed.").arg(modelName()));
        emit downloadChanged();
    });
    connect(&m_store, &ModelStore::failed, this, [this](const QString &message) {
        m_downloadLock.reset();
        refreshModel();
        setStatus(message, true);
        emit downloadChanged();
    });
    connect(&m_store, &ModelStore::cancelled, this, [this] {
        m_downloadLock.reset();
        refreshModel();
        setStatus(tr("Download paused. Download again to resume where it stopped."));
        emit downloadChanged();
    });

    m_estimateTimer.setInterval(1000);
    connect(&m_estimateTimer, &QTimer::timeout, this, &Backend::updateEstimate);

    reload();
    refreshModel();
    if (!m_catalogue.isValid())
        setStatus(m_catalogueError, true);
    recheckEngine();
}

Backend::~Backend() {
    // Destroying a running batch stops its engine, and the batch reports
    // that end as a failure. The report must not reach this object while
    // its members are being destroyed, so the batch goes first, unheard.
    if (m_batch) {
        m_batch->disconnect(this);
        m_batch.reset();
    }
    // The engine check runs in a worker thread and must end before the
    // objects it reports to.
    m_probing.waitForFinished();
}

QVariantList Backend::swatches() const {
    QVariantList list;
    if (!m_palette.isValid())
        return list;
    for (const PaletteColor &colour : m_palette.colors)
        list << QVariantMap{{"role", colour.role}, {"hex", colour.hex}, {"words", colour.words}};
    return list;
}

void Backend::reload() {
    m_themeName = m_omarchy->slug();
    if (m_themeName.isEmpty()) {
        m_palette = Palette{};
        m_palette.error = tr("No Omarchy theme found");
        setStatus(tr("No Omarchy theme found in %1. limgen still runs, with the default accent and no palette.")
                      .arg(m_omarchy->paths().state));
    } else {
        QString error;
        const QString text = m_omarchy->readColors(&error);
        m_palette = error.isEmpty() ? Palette::fromToml(text) : Palette{};
        if (!error.isEmpty())
            m_palette.error = error;
        if (!m_palette.isValid())
            setStatus(m_palette.error, true);
        else if (!m_statusIsError)
            setStatus(QString());
    }
    emit paletteChanged();
    // The built prompt follows the theme live; a running batch keeps the
    // theme it started with (Batch reads it once).
    emit promptChanged();
}

void Backend::setStatus(const QString &status, bool error) {
    if (status == m_status && error == m_statusIsError)
        return;
    m_status = status;
    m_statusIsError = error;
    emit statusChanged();
}

// ------------------------------------------------------------ engine

void Backend::recheckEngine() {
    if (m_probing.isRunning())
        return;
    m_probed = false;
    const QString program = m_engineProgram;
    // Listing the engine's devices starts every GPU backend, which can take
    // seconds on the first run after boot, so it runs off the UI thread.
    m_probing.setFuture(QtConcurrent::run([program] {
        Probe probe;
        probe.engine = SdCppEngine(program).probe();
        probe.gpus = Hardware::detect();
        return probe;
    }));
    emit engineChanged();
    emit stateChanged();
}

QString Backend::gpuText() const {
    if (m_gpu.name.isEmpty())
        return m_probed ? tr("No GPU found") : QString();
    if (m_gpu.totalMiB < 0)
        return m_gpu.name;
    const QLocale locale;
    return tr("%1, %2 of %3 GB free")
        .arg(m_gpu.name, locale.toString(m_gpu.freeMiB / 1024.0, 'f', 1), locale.toString(m_gpu.totalMiB / 1024.0, 'f', 1));
}

// ------------------------------------------------------------ models

void Backend::refreshModel() {
    const CatalogueModel *model = currentModel();
    m_plan = model ? m_store.plan(m_catalogue, model->levels.first(), true) : ModelStore::Plan{};
    emit modelsChanged();
    emit stateChanged();
    updateEstimate();
}

QVariantList Backend::models() const {
    QVariantList list;
    for (const CatalogueModel &model : m_catalogue.models()) {
        const ModelLevel &level = model.levels.first();
        const ModelStore::Plan plan = m_store.plan(m_catalogue, level, true);
        // -1 unknown, 0 not enough free video memory now, 1 runs here.
        const int runsHere = m_gpu.freeMiB < 0 ? -1 : m_gpu.freeMiB >= level.minFreeMiB ? 1 : 0;
        list << QVariantMap{{"id", model.id},
                            {"name", model.name},
                            {"publisher", model.publisher},
                            {"licence", model.licenceId},
                            {"isDefault", model.isDefault},
                            {"installed", plan.files.isEmpty()},
                            {"size", plan.files.isEmpty() ? QString() : sizeText(plan.downloadBytes())},
                            {"runsHere", runsHere},
                            {"selected", model.id == m_modelId}};
    }
    return list;
}

QString Backend::modelName() const { return currentModel() ? currentModel()->name : QString(); }
QString Backend::publisher() const { return currentModel() ? currentModel()->publisher : QString(); }
QString Backend::licenceUrl() const { return currentModel() ? currentModel()->licenceUrl : QString(); }

QString Backend::licenceText() const {
    const CatalogueModel *model = currentModel();
    return model ? tr("%1. %2").arg(model->licenceId, model->licenceSummary) : QString();
}

QString Backend::downloadSize() const {
    if (m_plan.partBytes > 0)
        return tr("%1 to download (%2 already downloaded)")
            .arg(sizeText(m_plan.downloadBytes()), sizeText(m_plan.partBytes));
    return tr("%1 to download").arg(sizeText(m_plan.downloadBytes()));
}

QString Backend::diskText() const {
    if (m_plan.availableBytes < 0)
        return {};
    return tr("%1 free in %2").arg(sizeText(m_plan.availableBytes), m_modelsDirectory);
}

QString Backend::downloadText() const {
    return tr("%1 of %2").arg(sizeText(m_downloadDone), sizeText(m_downloadTotal));
}

void Backend::selectModel(const QString &id) {
    if (busy() || !m_catalogue.model(id) || id == m_modelId)
        return;
    m_modelId = id;
    m_config.setModelId(id);
    QString error;
    if (!m_config.save(&error))
        setStatus(error, true);
    refreshModel();
}

void Backend::startDownload() {
    if (busy())
        return;
    refreshModel();
    if (m_plan.files.isEmpty())
        return;
    if (!m_plan.fitsOnDisk()) {
        setStatus(tr("Not enough disk space in %1: the download needs %2, and %3 more to spare.")
                      .arg(m_modelsDirectory, sizeText(m_plan.downloadBytes()), sizeText(ModelStore::headroomBytes)),
                  true);
        return;
    }
    // Two downloads into the same .part file would corrupt it, whether the
    // other one is another window or `limgen models pull`.
    QDir().mkpath(m_modelsDirectory);
    m_downloadLock = std::make_unique<QLockFile>(m_modelsDirectory + "/.download.lock");
    if (!m_downloadLock->tryLock(0)) {
        m_downloadLock.reset();
        setStatus(tr("Another limgen is downloading into %1; try again when it is done.").arg(m_modelsDirectory),
                  true);
        return;
    }
    m_downloadDone = m_plan.partBytes;
    m_downloadTotal = m_plan.totalBytes;
    setStatus(QString());
    m_store.start(m_plan.files);
    emit downloadChanged();
    emit stateChanged();
}

void Backend::pauseDownload() { m_store.cancel(); }

// ------------------------------------------------------------ prompt

QString Backend::builtPrompt() const { return Prompt::build(m_subject, m_palette, m_style).positive; }

void Backend::setSubject(const QString &subject) {
    if (subject == m_subject)
        return;
    m_subject = subject;
    emit promptChanged();
    emit stateChanged();
}

int Backend::styleIndex() const { return Prompt::styleIds().indexOf(Prompt::styleId(m_style)); }

void Backend::setStyleIndex(int index) {
    const QStringList ids = Prompt::styleIds();
    if (index < 0 || index >= ids.size() || index == styleIndex())
        return;
    Prompt::styleFromId(ids.at(index), &m_style);
    m_config.setStyleId(ids.at(index));
    QString error;
    if (!m_config.save(&error))
        setStatus(error, true);
    emit promptChanged();
    emit stateChanged();
}

void Backend::setFullPrompt(const QString &prompt) {
    // Text that matches the built prompt is not an edit, so the prompt keeps
    // following the subject and the theme.
    const bool edited = prompt != builtPrompt();
    if (edited == m_promptEdited && (!edited || prompt == m_editedPrompt))
        return;
    m_promptEdited = edited;
    m_editedPrompt = edited ? prompt : QString();
    emit promptChanged();
    emit stateChanged();
}

void Backend::resetPrompt() {
    if (!m_promptEdited)
        return;
    m_promptEdited = false;
    m_editedPrompt.clear();
    emit promptChanged();
    emit stateChanged();
}

// ------------------------------------------------------------ batch

bool Backend::canGenerateWith(const QString &prompt) const {
    return !prompt.trimmed().isEmpty() && m_catalogue.isValid() && engineReady() && modelInstalled() && !busy();
}

bool Backend::canGenerate() const {
    if (m_promptEdited)
        return canGenerateWith(m_editedPrompt);
    return !m_subject.trimmed().isEmpty() && canGenerateWith(builtPrompt());
}

void Backend::generate() {
    if (!canGenerate())
        return;
    // Unedited, the prompt is built when the batch starts, from the theme as
    // it is then.
    startBatch(m_promptEdited ? m_editedPrompt : QString(), m_subject.trimmed(), m_style);
}

void Backend::rerun() {
    // The drafts are filed with the subject and style the prompt came from,
    // not those typed since.
    if (canRerun())
        startBatch(m_lastPrompt, m_lastSubject, m_lastStyle);
}

void Backend::startBatch(const QString &fullPrompt, const QString &subject, Prompt::Style style) {
    BatchSettings settings;
    settings.modelId = m_modelId;
    settings.subject = subject;
    settings.style = style;
    settings.fullPrompt = fullPrompt;
    settings.count = draftCount;
    settings.previews = true;

    m_batch = std::make_unique<Batch>(m_catalogue, m_omarchy->paths(), m_engineProgram);
    Batch *batch = m_batch.get();
    QString message;
    if (batch->prepare(settings, m_probe, &message) != Batch::Outcome::Ok) {
        m_batch.reset();
        setStatus(message, true);
        emit stateChanged();
        return;
    }
    if (!batch->gpu().name.isEmpty()) {
        m_gpu = batch->gpu();
        emit engineChanged();
    }
    m_lastPrompt = batch->request().prompt;
    m_lastSubject = subject;
    m_lastStyle = style;

    // The batch is owned by m_batch before it starts, so a failure reported
    // at once still finds it there.
    auto release = [this, batch] {
        if (m_batch.get() == batch)
            m_batch.release()->deleteLater();
        m_estimateTimer.stop();
        emit stateChanged();
    };
    connect(batch, &Batch::progress, this, [this](int image, int step, int steps, double secondsPerStep) {
        if (secondsPerStep > 0 && step > 1)
            m_secondsPerStep = secondsPerStep;
        m_stepsDone = image * steps + step;
        setDraft(image, {{"state", "sampling"}, {"step", step}, {"steps", steps}});
    });
    connect(batch, &Batch::sampled, this, [this](int image) { setDraft(image, {{"state", "sampled"}}); });
    connect(batch, &Batch::preview, this, [this](int image, const QString &path) {
        setDraft(image, {{"preview", QUrl::fromLocalFile(path).toString()}});
    });
    connect(batch, &Batch::finished, this, [this, release](const QList<LibraryEntry> &entries) {
        for (int i = 0; i < entries.size(); ++i)
            setDraft(i, {{"state", "done"},
                         {"image", QUrl::fromLocalFile(entries.at(i).image).toString()},
                         {"id", entries.at(i).id},
                         {"preview", QString()}});
        m_estimate = tr("Made %n drafts in %1", "", entries.size())
                         .arg(durationText(m_batchClock.elapsed() / 1000.0));
        emit estimateChanged();
        setStatus(QString());
        release();
    });
    connect(batch, &Batch::failed, this, [this, release](Batch::Outcome outcome, const QString &text) {
        m_drafts.clear();
        emit draftsChanged();
        if (outcome == Batch::Outcome::Cancelled)
            setStatus(tr("Cancelled; no drafts were kept."));
        else
            setStatus(text, true);
        release();
        updateEstimate();
    });

    m_drafts.clear();
    for (int i = 0; i < draftCount; ++i)
        m_drafts << QVariantMap{{"state", "waiting"},
                                {"step", 0},
                                {"steps", batch->request().steps},
                                {"preview", QString()},
                                {"image", QString()},
                                {"id", QString()},
                                {"seed", double(batch->request().seed + quint32(i))}};
    m_batchEstimate = batch->estimate();
    m_secondsPerStep = -1;
    m_stepsDone = 0;
    m_batchClock.start();
    m_estimateTimer.start();
    setStatus(QString());
    emit draftsChanged();
    batch->start();
    emit stateChanged();
    updateEstimate();
}

void Backend::cancel() {
    if (running())
        m_batch->cancel();
}

void Backend::stopAll() {
    cancel();
    pauseDownload();
}

void Backend::setDraft(int index, const QVariantMap &changes) {
    if (index < 0 || index >= m_drafts.size())
        return;
    QVariantMap draft = m_drafts.at(index).toMap();
    for (auto it = changes.begin(); it != changes.end(); ++it)
        draft.insert(it.key(), it.value());
    m_drafts[index] = draft;
    emit draftsChanged();
}

// ------------------------------------------------------------ estimates

QString Backend::durationText(double seconds) {
    const int total = qMax(1, int(std::lround(seconds)));
    const QLocale locale;
    if (total < 60)
        return tr("%1 s").arg(locale.toString(total));
    return tr("%1 min %2 s").arg(locale.toString(total / 60), locale.toString(total % 60));
}

void Backend::updateEstimate() {
    QString text;
    if (running()) {
        const double elapsed = m_batchClock.elapsed() / 1000.0;
        const int steps = m_batch->request().steps;
        const int remainingSteps = qMax(0, draftCount * steps - m_stepsDone);
        double remaining = -1;
        if (m_batchEstimate > 0)
            remaining = qMax(m_batchEstimate - elapsed, remainingSteps * qMax(0.0, m_secondsPerStep));
        else if (m_secondsPerStep > 0)
            remaining = remainingSteps * m_secondsPerStep;
        text = remaining >= 0 ? tr("About %1 left").arg(durationText(remaining)) : tr("Starting the engine");
    } else if (engineReady() && modelInstalled() && currentModel()) {
        const CatalogueModel *model = currentModel();
        const Timing timing =
            Timings().get(Timings::key(model->id, model->levels.first().id, m_probe.gpu().backend()));
        const double seconds = timing.estimate(draftCount, model->steps);
        text = seconds > 0 ? tr("About %1 for %n drafts", "", draftCount).arg(durationText(seconds))
                           : tr("The first batch measures how fast this machine is.");
    }
    if (text != m_estimate) {
        m_estimate = text;
        emit estimateChanged();
    }
}
