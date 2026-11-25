#include "backend.h"

#include "omarchy.h"
#include "prompt.h"
#include "timings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace {

QString sizeText(qint64 bytes) { return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat); }

} // namespace

Backend::Backend(Omarchy *omarchy, const QString &engineProgram, QObject *parent)
    : QObject(parent), m_omarchy(omarchy), m_modelsDirectory(Catalogue::defaultModelsDirectory()),
      m_store(m_modelsDirectory) {
    if (engineProgram.isEmpty()) {
        m_engines = SdCppEngine::installed();
        m_engineProgram = SdCppEngine::programFor(m_config.engine());
    } else {
        m_engines = {EngineInstall{QString(), engineProgram}};
        m_engineProgram = engineProgram;
    }
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
    connect(this, &Backend::sourceChanged, this, &Backend::updateEstimate);
    connect(this, &Backend::settingsChanged, this, &Backend::updateEstimate);

    reload();
    refreshModel();
    if (!m_catalogue.isValid())
        setStatus(m_catalogueError, true);
    // Discarded drafts a window left behind when it did not close normally.
    // A day's grace leaves another window's undo alone.
    Library().purgeDiscarded(QDateTime::currentDateTime().addDays(-1));
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
    if (m_keeper) {
        m_keeper->disconnect(this);
        m_keeper.reset();
    }
    // Undo ends with the window, and with it the discarded drafts. Nothing
    // is announced: the window may already be gone.
    Library library;
    for (const Action &action : std::as_const(m_undo))
        if (action.kind == Action::Discard)
            library.purge(action.draftId);
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
    // Only what the last reload said is taken back: a kept image lands in
    // a watched folder and reloads the theme, and its message must stay.
    QString themeStatus;
    bool themeError = false;
    if (m_themeName.isEmpty()) {
        m_palette = Palette{};
        m_palette.error = tr("No Omarchy theme found");
        themeStatus = tr("No Omarchy theme found in %1. limgen still runs, with the default accent and no palette.")
                          .arg(m_omarchy->paths().state);
    } else {
        QString error;
        const QString text = m_omarchy->readColors(&error);
        m_palette = error.isEmpty() ? Palette::fromToml(text) : Palette{};
        if (!error.isEmpty())
            m_palette.error = error;
        if (!m_palette.isValid()) {
            themeStatus = m_palette.error;
            themeError = true;
        }
    }
    if (!themeStatus.isEmpty())
        setStatus(themeStatus, themeError);
    else if (!m_themeStatus.isEmpty() && m_status == m_themeStatus)
        setStatus(QString());
    m_themeStatus = themeStatus;
    // The left column: the theme's own backgrounds, then the user's.
    m_backgrounds.clear();
    const QString userFolder = m_omarchy->userBackgroundDirectory();
    for (const QString &path : m_omarchy->themeBackgrounds() + m_omarchy->userBackgrounds()) {
        const QString name = QFileInfo(path).fileName();
        m_backgrounds << QVariantMap{{"path", path},
                                     {"url", QUrl::fromLocalFile(path).toString()},
                                     {"name", name},
                                     {"user", !userFolder.isEmpty() && path.startsWith(userFolder + "/")},
                                     {"limgen", name.startsWith("limgen-")}};
    }
    forgetMissingSource();
    if (m_libraryLoaded)
        loadLibrary();
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

QVariantList Backend::engines() const {
    QVariantList list;
    for (const EngineInstall &engine : m_engines) {
        // Backend names are names, the same in every language.
        static const QMap<QString, QString> names{{"vulkan", "Vulkan"}, {"cuda", "CUDA"}, {"rocm", "ROCm"}};
        QString name = names.value(engine.backend, engine.backend);
        if (name.isEmpty())
            name = names.value(m_probe.gpu().backend(), QFileInfo(engine.program).fileName());
        list << QVariantMap{{"backend", engine.backend}, {"name", name}};
    }
    return list;
}

int Backend::engineIndex() const {
    for (int i = 0; i < m_engines.size(); ++i)
        if (m_engines.at(i).program == m_engineProgram)
            return i;
    return -1;
}

void Backend::selectEngine(int index) {
    if (busy() || m_probing.isRunning() || index < 0 || index >= m_engines.size() || index == engineIndex())
        return;
    m_engineProgram = m_engines.at(index).program;
    m_config.setEngine(m_engines.at(index).backend);
    saveConfig();
    recheckEngine();
}

void Backend::saveConfig() {
    QString error;
    if (!m_config.save(&error))
        setStatus(error, true);
}

void Backend::updateGpu(const Gpu &gpu) {
    if (gpu.name.isEmpty())
        return;
    m_gpu = gpu;
    emit engineChanged();
}

void Backend::setStudioBusy(bool busy) {
    if (busy == m_studioBusy)
        return;
    m_studioBusy = busy;
    emit stateChanged();
    // A keep asked for meanwhile, and a batch waiting for it, go now.
    if (!busy)
        nextKeep();
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
    saveConfig();
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
    saveConfig();
    emit promptChanged();
    emit stateChanged();
}

QString Backend::shownPrompt() const {
    // A variation of a library image with nothing typed uses that image's
    // prompt (Batch does the same), so that is the one shown.
    const QString reused = m_source.value("prompt").toString();
    return m_subject.trimmed().isEmpty() && !reused.isEmpty() ? reused : builtPrompt();
}

void Backend::setFullPrompt(const QString &prompt) {
    // Text that matches the prompt shown is not an edit, so the prompt keeps
    // following the subject, the theme and the source.
    const bool edited = prompt != shownPrompt();
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
    // A keep does not stop a new batch; the batch waits for it.
    return !prompt.trimmed().isEmpty() && m_catalogue.isValid() && engineReady() && modelInstalled() && !running() &&
           !m_batchWaiting && !downloading() && !m_studioBusy;
}

bool Backend::canGenerate() const {
    if (m_promptEdited)
        return canGenerateWith(m_editedPrompt);
    // A variation needs nothing typed: it keeps its source's look.
    return (!m_subject.trimmed().isEmpty() || !m_source.isEmpty()) && canGenerateWith(builtPrompt());
}

void Backend::generate() {
    if (m_studioBusy) {
        setStatus(tr("The Generic screen is using the GPU. Try again when its previews or upscale are done."));
        return;
    }
    if (!canGenerate())
        return;
    // Unedited, the prompt is built when the batch starts, from the theme as
    // it is then.
    BatchSettings settings;
    settings.fullPrompt = m_promptEdited ? m_editedPrompt : QString();
    settings.subject = m_subject.trimmed();
    settings.style = m_style;
    settings.source = m_source.value("key").toString();
    settings.strength = strength();
    startBatch(settings);
}

void Backend::rerun() {
    // The drafts are filed with the subject, style and source the prompt
    // came from, not those chosen since.
    if (!canRerun())
        return;
    BatchSettings settings = m_lastSettings;
    settings.fullPrompt = m_lastPrompt;
    settings.seed.reset();
    startBatch(settings);
}

void Backend::startBatch(BatchSettings settings) {
    if (keeping()) {
        m_batchWaiting = true;
        m_waitingSettings = settings;
        setStatus(tr("The batch starts when the kept image is finished."));
        emit stateChanged();
        updateEstimate();
        return;
    }
    settings.modelId = m_modelId;
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
    m_lastSettings = settings;
    m_batchTheme = batch->themeSlug();
    // Undo covers one batch: its tiles are about to go.
    forgetActions();

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
        if (m_libraryLoaded)
            loadLibrary();
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
                                {"seed", double(batch->request().seed + quint32(i))},
                                {"theme", m_batchTheme},
                                {"keeping", false},
                                {"keepFraction", -1.0},
                                {"kept", false},
                                {"keptPath", QString()},
                                {"set", false},
                                {"discarded", false}};
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
    if (running()) {
        m_batch->cancel();
    } else if (m_batchWaiting) {
        m_batchWaiting = false;
        setStatus(tr("Cancelled; the batch did not start."));
        emit stateChanged();
        updateEstimate();
    }
}

void Backend::stopAll() {
    cancel();
    m_batchWaiting = false;
    // Keeps still waiting their turn are dropped, and no unfinished keep is
    // left to undo.
    for (const KeepJob &job : std::as_const(m_keepQueue))
        setDraftFor(job.index, job.draftId, {{"keeping", false}, {"keepFraction", -1.0}});
    m_keepQueue.clear();
    m_undo.removeIf([](const Action &a) { return a.kind == Action::Keep && !a.done; });
    if (m_keeper) {
        setDraftFor(m_keepJob.index, m_keepJob.draftId, {{"keeping", false}, {"keepFraction", -1.0}});
        m_keeper->cancel();
    }
    pauseDownload();
    emit stateChanged();
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
    if (m_batchWaiting) {
        text = tr("Waits for the kept image");
    } else if (running()) {
        const double elapsed = m_batchClock.elapsed() / 1000.0;
        const int steps = m_batch->request().samplingSteps();
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
        // A variation runs only part of the steps; the same formula as the
        // engine request's works it out.
        EngineRequest request;
        request.steps = model->steps;
        request.strength = Batch::strengthFor(model->steps, strength());
        if (!m_source.isEmpty())
            request.initImage = m_source.value("path").toString();
        const double seconds = timing.estimate(draftCount, request.samplingSteps());
        text = seconds > 0 ? tr("About %1 for %n drafts", "", draftCount).arg(durationText(seconds))
                           : tr("The first batch measures how fast this machine is.");
    }
    if (text != m_estimate) {
        m_estimate = text;
        emit estimateChanged();
    }
}

// ------------------------------------------------------------ keeping

void Backend::setColorPass(double strength) {
    if (qFuzzyCompare(1 + strength, 1 + colorPass()))
        return;
    m_config.setColorPass(strength);
    saveConfig();
    emit settingsChanged();
}

void Backend::setFormatIndex(int index) {
    if (index == formatIndex() || index < 0 || index > 1)
        return;
    m_config.setFormat(index == 1 ? "jpeg" : "png");
    saveConfig();
    emit settingsChanged();
}

QVariantMap Backend::draftAt(int index, const QString &id) const {
    if (index < 0 || index >= m_drafts.size())
        return {};
    const QVariantMap draft = m_drafts.at(index).toMap();
    return id.isEmpty() || draft.value("id").toString() == id ? draft : QVariantMap();
}

// The tile may belong to a later batch by the time a keep ends, so the id
// must still match.
void Backend::setDraftFor(int index, const QString &id, const QVariantMap &changes) {
    if (!draftAt(index, id).isEmpty())
        setDraft(index, changes);
}

void Backend::keep(int index) { requestKeep(index, false); }

void Backend::keepAndSet(int index) { requestKeep(index, true); }

void Backend::requestKeep(int index, bool set) {
    const QVariantMap draft = draftAt(index, QString());
    if (draft.value("state").toString() != "done" || draft.value("discarded").toBool() ||
        draft.value("keeping").toBool())
        return;
    if (draft.value("kept").toBool()) {
        // Kept already: S still shows it, K has nothing left to do.
        if (set && !draft.value("set").toBool()) {
            QString error;
            if (!m_omarchy->setBackground(draft.value("keptPath").toString(), &error)) {
                setStatus(error, true);
                return;
            }
            setDraft(index, {{"set", true}});
            setStatus(tr("Set as the background."));
        }
        return;
    }
    const QString madeFor = draft.value("theme").toString();
    if (!madeFor.isEmpty() && !m_themeName.isEmpty() && madeFor != m_themeName) {
        emit themeChoiceNeeded(index, madeFor, m_themeName, set);
        return;
    }
    keepInto(index, m_themeName.isEmpty() ? madeFor : m_themeName, set);
}

void Backend::keepInto(int index, const QString &theme, bool set) {
    const QVariantMap draft = draftAt(index, QString());
    if (draft.value("state").toString() != "done" || draft.value("discarded").toBool() ||
        draft.value("keeping").toBool() || draft.value("kept").toBool())
        return;
    Action action;
    action.kind = Action::Keep;
    action.index = index;
    action.draftId = draft.value("id").toString();
    action.theme = theme;
    action.set = set;
    m_redo.clear();
    enqueueKeep(action);
}

void Backend::enqueueKeep(const Action &queued) {
    Action action = queued;
    action.serial = m_nextSerial++;
    action.done = false;
    m_undo << action;
    m_keepQueue << KeepJob{action.serial, action.index, action.draftId, action.theme, action.set};
    setDraftFor(action.index, action.draftId, {{"keeping", true}, {"keepFraction", -1.0}});
    if (m_studioBusy)
        setStatus(tr("The keep starts when the Generic screen's work is done."));
    emit stateChanged();
    nextKeep();
}

void Backend::nextKeep() {
    if (m_keeper || running() || m_studioBusy)
        return;
    if (m_keepQueue.isEmpty()) {
        if (m_batchWaiting) {
            m_batchWaiting = false;
            startBatch(m_waitingSettings);
        }
        return;
    }
    const KeepJob job = m_keepQueue.takeFirst();
    KeepSettings settings;
    settings.draftId = job.draftId;
    settings.theme = job.theme;
    settings.colorPass = m_config.colorPass();
    settings.format = m_config.format();
    settings.jpegQuality = m_config.jpegQuality();

    m_keeper = std::make_unique<Keeper>(m_catalogue, m_omarchy->paths(), m_engineProgram);
    Keeper *keeper = m_keeper.get();
    QString message;
    if (keeper->prepare(settings, m_probe, &message) != Keeper::Outcome::Ok) {
        m_keeper.reset();
        keepFailed(job, message);
        QTimer::singleShot(0, this, &Backend::nextKeep);
        return;
    }
    m_keepJob = job;
    // Owned by m_keeper until it reports, then deleted from the event loop,
    // since it is still inside its own signal then.
    auto release = [this, keeper] {
        if (m_keeper.get() == keeper)
            m_keeper.release()->deleteLater();
        emit stateChanged();
        QTimer::singleShot(0, this, &Backend::nextKeep);
    };
    connect(keeper, &Keeper::progress, this,
            [this, job](double fraction) { setDraftFor(job.index, job.draftId, {{"keepFraction", fraction}}); });
    connect(keeper, &Keeper::finished, this, [this, job, release](const KeepResult &result) {
        keepFinished(job, result);
        release();
    });
    connect(keeper, &Keeper::failed, this, [this, job, release](Keeper::Outcome outcome, const QString &text) {
        // A cancel comes from undo or stopAll, which already reset the tile.
        // The engine can take a moment to end, and by then the tile may hold
        // a new keep of the same draft, which this must not touch.
        if (outcome != Keeper::Outcome::Cancelled)
            keepFailed(job, text);
        release();
    });
    setDraftFor(job.index, job.draftId, {{"keepFraction", 0.0}});
    keeper->start();
    emit stateChanged();
}

Backend::Action *Backend::actionFor(quint64 serial) {
    for (Action &action : m_undo)
        if (action.kind == Action::Keep && action.serial == serial)
            return &action;
    return nullptr;
}

void Backend::setBackgroundFor(Action &action) {
    action.previousBackground = m_omarchy->currentBackground();
    QString error;
    if (!m_omarchy->setBackground(action.installed, &error)) {
        action.set = false;
        setStatus(error, true);
    }
}

void Backend::keepFinished(const KeepJob &job, const KeepResult &result) {
    Action fallback;
    Action *action = actionFor(job.serial);
    if (!action)
        action = &fallback; // undo was cleared meanwhile; the keep still counts
    action->done = true;
    action->installed = result.installed;
    action->installedByUs = !result.alreadyKept;
    action->finalImage = result.final.image;
    action->set = job.set;

    const QString name = QFileInfo(result.installed).fileName();
    const QString where = result.theme.isEmpty() ? QFileInfo(result.installed).absolutePath() : result.theme;
    if (result.alreadyKept)
        setStatus(tr("Already kept as %1 in %2.").arg(name, where));
    else
        setStatus(tr("Kept as %1 in %2.").arg(name, where));
    if (action->set) {
        setBackgroundFor(*action);
        if (action->set)
            setStatus(tr("Kept as %1 in %2 and set as the background.").arg(name, where));
    }
    const bool set = action->set;
    // Kept before: there is nothing of this keep to undo, unless it also
    // changed the background.
    if (!action->installedByUs && !set && action != &fallback)
        m_undo.removeIf([&](const Action &a) { return a.kind == Action::Keep && a.serial == job.serial; });
    setDraftFor(job.index, job.draftId,
                {{"keeping", false}, {"kept", true}, {"keptPath", result.installed}, {"set", set}});
    if (m_libraryLoaded)
        loadLibrary();
    emit stateChanged();
}

void Backend::keepFailed(const KeepJob &job, const QString &message) {
    m_undo.removeIf([&](const Action &a) { return a.kind == Action::Keep && a.serial == job.serial; });
    setDraftFor(job.index, job.draftId, {{"keeping", false}, {"keepFraction", -1.0}});
    setStatus(message, true);
    emit stateChanged();
}

void Backend::discard(int index) {
    const QVariantMap draft = draftAt(index, QString());
    if (draft.value("state").toString() != "done" || draft.value("discarded").toBool() ||
        draft.value("keeping").toBool() || draft.value("kept").toBool())
        return;
    QString error;
    if (!Library().discard(draft.value("id").toString(), &error)) {
        setStatus(error, true);
        return;
    }
    Action action;
    action.kind = Action::Discard;
    action.index = index;
    action.draftId = draft.value("id").toString();
    m_undo << action;
    m_redo.clear();
    setDraft(index, {{"discarded", true}});
    setStatus(tr("Discarded. Ctrl+Z brings it back."));
    if (m_libraryLoaded)
        loadLibrary();
    emit stateChanged();
}

void Backend::undo() {
    if (m_undo.isEmpty())
        return;
    // Undoing a keep that is the background now needs an earlier one to
    // go back to; removing the file would leave the desktop pointing at
    // nothing.
    const Action &last = m_undo.last();
    if (last.kind == Action::Keep && last.done && last.set && last.installedByUs &&
        m_omarchy->currentBackground() == last.installed &&
        (last.previousBackground.isEmpty() || !QFileInfo::exists(last.previousBackground))) {
        setStatus(tr("%1 is the background, and there is no earlier one to go back to. Set another background "
                     "first, then undo.")
                      .arg(QFileInfo(last.installed).fileName()),
                  true);
        return;
    }
    Action action = m_undo.takeLast();
    if (action.kind == Action::Discard) {
        QString error;
        if (!Library().restore(action.draftId, &error)) {
            setStatus(error, true);
            emit stateChanged();
            return;
        }
        setDraftFor(action.index, action.draftId, {{"discarded", false}});
        setStatus(tr("The draft is back."));
    } else if (!action.done) {
        // Still waiting or being finished: it simply stops.
        m_keepQueue.removeIf([&](const KeepJob &job) { return job.serial == action.serial; });
        if (m_keeper && m_keepJob.serial == action.serial)
            m_keeper->cancel();
        setDraftFor(action.index, action.draftId, {{"keeping", false}, {"keepFraction", -1.0}});
        setStatus(tr("The keep was cancelled."));
    } else {
        // The background goes back first, while the kept file still exists
        // for Omarchy to move away from.
        if (action.set && !action.previousBackground.isEmpty() && QFileInfo::exists(action.previousBackground) &&
            m_omarchy->currentBackground() == action.installed)
            m_omarchy->setBackground(action.previousBackground);
        if (action.installedByUs)
            QFile::remove(action.installed);
        forgetMissingSource();
        setDraftFor(action.index, action.draftId, {{"kept", false}, {"keptPath", QString()}, {"set", false}});
        setStatus(tr("Removed %1 from the backgrounds.").arg(QFileInfo(action.installed).fileName()));
    }
    m_redo << action;
    if (m_libraryLoaded)
        loadLibrary();
    emit stateChanged();
}

void Backend::redo() {
    if (m_redo.isEmpty())
        return;
    Action action = m_redo.takeLast();
    if (action.kind == Action::Discard) {
        QString error;
        if (!Library().discard(action.draftId, &error)) {
            setStatus(error, true);
            emit stateChanged();
            return;
        }
        setDraftFor(action.index, action.draftId, {{"discarded", true}});
        setStatus(tr("Discarded. Ctrl+Z brings it back."));
        m_undo << action;
        if (m_libraryLoaded)
            loadLibrary();
    } else if (!action.done) {
        enqueueKeep(action);
        return;
    } else {
        // The final is still in the library, so the copy comes back as it was.
        if (action.installedByUs && !QFileInfo::exists(action.installed)) {
            const QString part = action.installed + ".part";
            QFile::remove(part);
            if (!QFile::copy(action.finalImage, part) || !QFile::rename(part, action.installed)) {
                QFile::remove(part);
                setStatus(tr("Cannot write %1.").arg(action.installed), true);
                emit stateChanged();
                return;
            }
        }
        if (action.set)
            setBackgroundFor(action);
        setDraftFor(action.index, action.draftId,
                    {{"kept", true}, {"keptPath", action.installed}, {"set", action.set}});
        setStatus(tr("Kept again as %1.").arg(QFileInfo(action.installed).fileName()));
        m_undo << action;
    }
    if (m_libraryLoaded)
        loadLibrary();
    emit stateChanged();
}

void Backend::forgetActions() {
    Library library;
    for (const Action &action : std::as_const(m_undo))
        if (action.kind == Action::Discard)
            library.purge(action.draftId);
    const bool had = !m_undo.isEmpty() || !m_redo.isEmpty();
    m_undo.clear();
    m_redo.clear();
    if (had)
        emit stateChanged();
}

// ------------------------------------------------------------ variations

void Backend::setStrength(double strength) {
    if (qFuzzyCompare(1 + strength, 1 + this->strength()))
        return;
    m_config.setStrength(strength);
    saveConfig();
    emit settingsChanged();
}

void Backend::useBackground(int index) {
    if (index < 0 || index >= m_backgrounds.size())
        return;
    const QVariantMap background = m_backgrounds.at(index).toMap();
    if (m_source.value("path") == background.value("path")) {
        clearSource();
        return;
    }
    m_source = {{"kind", "background"},
                {"key", background.value("path")},
                {"path", background.value("path")},
                {"url", background.value("url")},
                {"name", background.value("name")}};
    emit sourceChanged();
    emit promptChanged();
    emit stateChanged();
}

void Backend::cycleBackground(int step) {
    if (m_backgrounds.isEmpty())
        return;
    int current = -1;
    for (int i = 0; i < m_backgrounds.size(); ++i)
        if (m_backgrounds.at(i).toMap().value("path") == m_source.value("path"))
            current = i;
    const int count = int(m_backgrounds.size());
    const int next = current < 0 ? (step > 0 ? 0 : count - 1) : ((current + step) % count + count) % count;
    if (next != current)
        useBackground(next);
}

void Backend::useLibrary(const QString &id) {
    const LibraryEntry entry = Library().find(id);
    if (!entry.isValid()) {
        setStatus(tr("No image %1 in the library.").arg(id), true);
        return;
    }
    const QString subject = entry.metadata.value("subject").toString();
    m_source = {{"kind", "library"},
                {"key", entry.id},
                {"path", entry.image},
                {"url", QUrl::fromLocalFile(entry.image).toString()},
                {"name", subject.isEmpty() ? entry.id : subject},
                {"prompt", entry.metadata.value("prompt").toString()}};
    emit sourceChanged();
    emit promptChanged();
    emit stateChanged();
}

void Backend::clearSource() {
    if (m_source.isEmpty())
        return;
    m_source.clear();
    emit sourceChanged();
    emit promptChanged();
    emit stateChanged();
}

// A background chosen as the source goes with its file: when the theme
// changes, or the file is removed (a keep undone).
void Backend::forgetMissingSource() {
    if (m_source.value("kind").toString() != "background")
        return;
    const QString path = m_source.value("path").toString();
    bool listed = false;
    for (const QVariant &background : std::as_const(m_backgrounds))
        listed = listed || background.toMap().value("path").toString() == path;
    if (!listed || !QFileInfo::exists(path))
        clearSource();
}

void Backend::vary(int index) {
    // V starts a batch; when none can start, it changes nothing either.
    const QVariantMap draft = draftAt(index, QString());
    if (draft.value("state").toString() != "done" || draft.value("discarded").toBool() ||
        !canGenerateWith(builtPrompt()))
        return;
    useLibrary(draft.value("id").toString());
    generate();
}

void Backend::varyLibrary(const QString &id) {
    if (!canGenerateWith(builtPrompt()))
        return;
    useLibrary(id);
    if (m_source.value("key") == id)
        generate();
}

void Backend::loadLibrary() {
    const QList<LibraryEntry> entries = Library().list();
    // A draft counts as kept when a final was made from it.
    QSet<QString> kept;
    for (const LibraryEntry &entry : entries)
        if (entry.metadata.value("kind").toString() == "final")
            kept.insert(entry.metadata.value("finish").toObject().value("from").toString());
    m_library.clear();
    for (const LibraryEntry &entry : entries) {
        const QString made = entry.metadata.value("theme").toObject().value("slug").toString();
        if (!m_themeName.isEmpty() && made != m_themeName)
            continue;
        m_library << QVariantMap{{"id", entry.id},
                                 {"kind", entry.metadata.value("kind").toString()},
                                 {"image", QUrl::fromLocalFile(entry.image).toString()},
                                 {"subject", entry.metadata.value("subject").toString()},
                                 {"seed", entry.metadata.value("seed").toDouble()},
                                 {"created", entry.metadata.value("created").toString()},
                                 {"kept", kept.contains(entry.id)},
                                 {"keptPath", QString()},
                                 {"state", "done"}};
    }
    m_libraryLoaded = true;
    emit libraryChanged();
}
