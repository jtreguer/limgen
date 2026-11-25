#include "studio.h"

#include "backend.h"
#include "omarchy.h"
#include "prompt.h"
#include "timings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

Studio::Studio(Backend *backend, QObject *parent) : QObject(parent), m_backend(backend) {
    const Config &config = m_backend->config();
    m_count = config.genericCount();
    Prompt::varietyFromId(config.genericVariety(), &m_variety);
    m_palette = config.genericPalette();
    m_colors = config.genericColors();

    reload();
    // The saved size is the monitor's when none was saved, a common size
    // when it is one, and a custom size otherwise.
    const QSize saved = config.genericSize();
    if (!saved.isEmpty()) {
        m_sizeIndex = m_sizes.size() - 1;
        m_custom = saved;
        for (int i = 1; i < m_sizes.size() - 1; ++i) {
            const QVariantMap size = m_sizes.at(i).toMap();
            if (size.value("width").toInt() == saved.width() && size.value("height").toInt() == saved.height())
                m_sizeIndex = i;
        }
    }

    connect(m_backend, &Backend::stateChanged, this, &Studio::stateChanged);
    connect(m_backend, &Backend::engineChanged, this, &Studio::stateChanged);
    connect(m_backend, &Backend::engineChanged, this, &Studio::updateEstimate);
    // The previews' size and the estimate follow the model.
    connect(m_backend, &Backend::modelsChanged, this, &Studio::settingsChanged);
    connect(m_backend, &Backend::modelsChanged, this, &Studio::updateEstimate);
    // A theme switch or a new theme folder changes the palettes on offer.
    connect(m_backend->omarchy(), &Omarchy::changed, this, &Studio::reloadPalettes);
    connect(this, &Studio::settingsChanged, this, &Studio::updateEstimate);
    connect(&m_saving, &QFutureWatcher<Saved>::finished, this, [this] {
        const Saved saved = m_saving.result();
        if (saved.error.isEmpty())
            setStatus(tr("Saved %1.").arg(saved.file));
        else
            setStatus(saved.error, true);
        emit stateChanged();
    });
    m_estimateTimer.setInterval(1000);
    connect(&m_estimateTimer, &QTimer::timeout, this, &Studio::updateEstimate);
    updateEstimate();
}

Studio::~Studio() {
    // As in Backend: a batch or an upscale stopped by its destruction
    // reports a failure, which must not reach this half-destroyed object.
    if (m_batch) {
        m_batch->disconnect(this);
        m_batch.reset();
    }
    if (m_keeper) {
        m_keeper->disconnect(this);
        m_keeper.reset();
    }
    m_saving.disconnect(this);
    m_saving.waitForFinished();
    m_backend->setStudioBusy(false);
}

// ------------------------------------------------------------ choices

void Studio::reload() {
    reloadSizes();
    reloadPalettes();
}

void Studio::reloadSizes() {
    m_sizes.clear();
    const Monitor monitor = Omarchy::largest(m_backend->omarchy()->monitors());
    auto add = [this](const QString &kind, int width, int height) {
        m_sizes << QVariantMap{{"kind", kind}, {"width", width}, {"height", height}};
    };
    if (monitor.width > 0 && monitor.height > 0)
        add("monitor", monitor.width, monitor.height);
    else
        add("fallback", 2560, 1440);
    add("preset", 1920, 1080);
    add("preset", 2560, 1440);
    add("preset", 3840, 2160);
    add("wide", 3440, 1440);
    add("portrait", 1080, 1920);
    add("square", 2048, 2048);
    add("custom", m_custom.width(), m_custom.height());
    m_sizeIndex = qBound(0, m_sizeIndex, int(m_sizes.size()) - 1);
    emit sizesChanged();
    emit settingsChanged();
}

void Studio::reloadPalettes() {
    m_palettes.clear();
    m_themePalettes.clear();
    m_palettes << QVariantMap{{"id", QString()}, {"swatches", QStringList()}};
    const Omarchy *omarchy = m_backend->omarchy();
    for (const QString &slug : omarchy->themeSlugs()) {
        QFile colors(omarchy->themeDirectory(slug) + "/colors.toml");
        if (!colors.open(QIODevice::ReadOnly))
            continue;
        const Palette palette = Palette::fromToml(QString::fromUtf8(colors.readAll()));
        if (!palette.isValid())
            continue;
        QStringList swatches;
        for (const PaletteColor &colour : palette.colors)
            swatches << colour.hex;
        m_themePalettes.insert(slug, palette);
        m_palettes << QVariantMap{{"id", slug}, {"swatches", swatches}};
    }
    m_palettes << QVariantMap{{"id", "custom"}, {"swatches", m_colors}};
    // A theme that went away leaves no palette chosen.
    if (paletteIndex() < 0)
        m_palette.clear();
    emit palettesChanged();
    emit settingsChanged();
    emit promptChanged();
}

void Studio::coloursChanged() {
    // Only the custom entry changes; the themes need not be read again.
    m_palettes.last() = QVariantMap{{"id", "custom"}, {"swatches", m_colors}};
    saveSettings();
    emit palettesChanged();
    emit settingsChanged();
    emit promptChanged();
}

void Studio::setSizeIndex(int index) {
    if (index < 0 || index >= m_sizes.size() || index == m_sizeIndex)
        return;
    m_sizeIndex = index;
    saveSettings();
    emit settingsChanged();
    emit stateChanged();
}

void Studio::setCustomWidth(int width) {
    width = qBound(1, width, maxSide);
    if (width == m_custom.width())
        return;
    m_custom.setWidth(width);
    saveSettings();
    emit settingsChanged();
    emit stateChanged();
}

void Studio::setCustomHeight(int height) {
    height = qBound(1, height, maxSide);
    if (height == m_custom.height())
        return;
    m_custom.setHeight(height);
    saveSettings();
    emit settingsChanged();
    emit stateChanged();
}

QSize Studio::target() const {
    if (customSize())
        return m_custom;
    const QVariantMap size = m_sizes.value(m_sizeIndex).toMap();
    return QSize(size.value("width").toInt(), size.value("height").toInt());
}

QSize Studio::draftSize() const {
    const CatalogueModel *model = m_backend->catalogue().model(m_backend->modelId());
    return model ? Batch::draftSize(target(), QSize(model->width, model->height)) : QSize();
}

QString Studio::sizeProblem() const { return sizeProblemFor(target(), draftSize()); }

QString Studio::sizeProblemFor(const QSize &target, const QSize &draft) {
    if (target.width() < minSide || target.height() < minSide)
        return tr("Each side needs at least %1 pixels.").arg(minSide);
    const double ratio = double(target.width()) / target.height();
    if (ratio > Batch::maxRatio || ratio < 1 / Batch::maxRatio)
        return tr("%1×%2 is more than four times as wide as it is tall, or the other way round.")
            .arg(target.width())
            .arg(target.height());
    if (draft.isValid() && (target.width() > 4 * draft.width() || target.height() > 4 * draft.height()))
        return tr("%1×%2 is more than four times the previews' %3×%4, which is as far as the upscaler goes.")
            .arg(target.width())
            .arg(target.height())
            .arg(draft.width())
            .arg(draft.height());
    return {};
}

int Studio::gridIndex() const { return counts().indexOf(m_count); }

void Studio::setGridIndex(int index) {
    const QList<int> all = counts();
    if (index < 0 || index >= all.size() || all.at(index) == m_count)
        return;
    m_count = all.at(index);
    saveSettings();
    emit settingsChanged();
}

void Studio::setVarietyIndex(int index) {
    if (index < 0 || index >= Prompt::varietyIds().size() || index == varietyIndex())
        return;
    m_variety = Prompt::Variety(index);
    saveSettings();
    emit settingsChanged();
}

int Studio::paletteIndex() const {
    for (int i = 0; i < m_palettes.size(); ++i)
        if (m_palettes.at(i).toMap().value("id").toString() == m_palette)
            return i;
    return -1;
}

void Studio::setPaletteIndex(int index) {
    if (index < 0 || index >= m_palettes.size() || index == paletteIndex())
        return;
    m_palette = m_palettes.at(index).toMap().value("id").toString();
    saveSettings();
    emit settingsChanged();
    emit promptChanged();
}

QVariantList Studio::customColors() const {
    QVariantList list;
    for (const QString &hex : m_colors)
        list << QVariantMap{{"hex", hex}, {"words", Palette::describe(hex)}};
    return list;
}

Palette Studio::chosenPalette() const {
    if (m_palette == "custom" && !m_colors.isEmpty())
        return Palette::fromColors(m_colors);
    if (m_themePalettes.contains(m_palette))
        return m_themePalettes.value(m_palette);
    Palette none;
    none.error = "none";
    return none;
}

QString Studio::paletteWords() const {
    const Palette palette = chosenPalette();
    return palette.isValid() ? palette.words.join(", ") : QString();
}

QString Studio::normalised(const QString &hex) {
    static const QRegularExpression rgb(QRegularExpression::anchoredPattern("#[0-9a-f]{6}"));
    static const QRegularExpression argb(QRegularExpression::anchoredPattern("#[0-9a-f]{8}"));
    const QString text = hex.trimmed().toLower();
    if (rgb.match(text).hasMatch())
        return text;
    if (argb.match(text).hasMatch())
        return "#" + text.mid(3);
    return {};
}

void Studio::addColor(const QString &hex) {
    const QString colour = normalised(hex);
    if (colour.isEmpty() || m_colors.size() >= Palette::maxColors)
        return;
    m_colors << colour;
    m_palette = "custom";
    coloursChanged();
}

void Studio::setColor(int index, const QString &hex) {
    const QString colour = normalised(hex);
    if (colour.isEmpty() || index < 0 || index >= m_colors.size() || m_colors.at(index) == colour)
        return;
    m_colors[index] = colour;
    coloursChanged();
}

void Studio::removeColor(int index) {
    if (index < 0 || index >= m_colors.size())
        return;
    m_colors.removeAt(index);
    coloursChanged();
}

void Studio::setPrompt(const QString &prompt) {
    if (prompt == m_prompt)
        return;
    m_prompt = prompt;
    emit promptChanged();
    emit stateChanged();
}

QString Studio::fullPrompt() const { return Prompt::buildGeneric(m_prompt, chosenPalette()).positive; }

void Studio::saveSettings() {
    Config &config = m_backend->config();
    // The monitor's entry is saved as no size, so it follows the monitor.
    config.setGenericSize(m_sizeIndex == 0 ? QSize() : target());
    config.setGenericCount(m_count);
    config.setGenericPalette(m_palette);
    config.setGenericColors(m_colors);
    config.setGenericVariety(Prompt::varietyId(m_variety));
    m_backend->saveConfig();
}

void Studio::setStatus(const QString &status, bool error) {
    if (status == m_status && error == m_statusIsError)
        return;
    m_status = status;
    m_statusIsError = error;
    emit statusChanged();
}

// ------------------------------------------------------------ previews

bool Studio::canGenerate() const {
    return !m_prompt.trimmed().isEmpty() && sizeProblem().isEmpty() && !busy() && !m_backend->gpuBusy() &&
           !m_backend->downloading() && m_backend->engineReady() && m_backend->modelInstalled() &&
           m_backend->catalogue().isValid();
}

void Studio::holdGpu() { m_backend->setStudioBusy(running() || upscaling()); }

void Studio::generate() {
    if (m_backend->gpuBusy()) {
        setStatus(tr("The theme screen is using the GPU. Try again when its batch or keep is done."));
        return;
    }
    if (!canGenerate())
        return;
    BatchSettings settings;
    settings.generic = true;
    settings.modelId = m_backend->modelId();
    settings.subject = m_prompt.trimmed();
    settings.target = target();
    settings.count = m_count;
    settings.previews = true;
    settings.variety = m_variety;
    if (m_palette == "custom")
        settings.paletteColors = m_colors;
    else
        settings.paletteTheme = m_palette;

    m_batch = std::make_unique<Batch>(m_backend->catalogue(), m_backend->omarchy()->paths(),
                                      m_backend->engineProgram());
    Batch *batch = m_batch.get();
    QString message;
    if (batch->prepare(settings, m_backend->probe(), &message) != Batch::Outcome::Ok) {
        m_batch.reset();
        setStatus(message, true);
        emit stateChanged();
        return;
    }
    m_backend->updateGpu(batch->gpu());
    m_batchTarget = batch->target();
    m_batchPrompt = settings.subject;

    auto release = [this, batch] {
        if (m_batch.get() == batch)
            m_batch.release()->deleteLater();
        m_estimateTimer.stop();
        holdGpu();
        emit stateChanged();
        updateEstimate();
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
        setStatus(tr("Made %n previews in %1. Pick one and upscale it.", "", entries.size())
                      .arg(Backend::durationText(m_batchClock.elapsed() / 1000.0)));
        release();
    });
    connect(batch, &Batch::failed, this, [this, release](Batch::Outcome outcome, const QString &text) {
        m_drafts.clear();
        emit draftsChanged();
        if (outcome == Batch::Outcome::Cancelled)
            setStatus(tr("Cancelled; no previews were kept."));
        else
            setStatus(text, true);
        release();
    });

    m_drafts.clear();
    for (int i = 0; i < m_count; ++i)
        m_drafts << QVariantMap{{"state", "waiting"},
                                {"step", 0},
                                {"steps", batch->request().steps},
                                {"preview", QString()},
                                {"image", QString()},
                                {"id", QString()},
                                {"seed", double(batch->request().seed + quint32(i))},
                                {"hint", batch->hints().value(i)},
                                {"width", batch->request().width},
                                {"height", batch->request().height}};
    // The result belongs to the previews it came from, which are about to go.
    if (!m_result.isEmpty()) {
        m_result.clear();
        emit resultChanged();
    }
    m_batchEstimate = batch->estimate();
    m_secondsPerStep = -1;
    m_stepsDone = 0;
    m_batchClock.start();
    m_estimateTimer.start();
    setStatus(QString());
    emit draftsChanged();
    batch->start();
    holdGpu();
    emit stateChanged();
    updateEstimate();
}

void Studio::cancel() {
    if (running())
        m_batch->cancel();
    else if (m_keeper)
        m_keeper->cancel();
}

void Studio::stop() {
    cancel();
    m_saving.waitForFinished();
}

void Studio::setDraft(int index, const QVariantMap &changes) {
    if (index < 0 || index >= m_drafts.size())
        return;
    QVariantMap draft = m_drafts.at(index).toMap();
    for (auto it = changes.begin(); it != changes.end(); ++it)
        draft.insert(it.key(), it.value());
    m_drafts[index] = draft;
    emit draftsChanged();
}

void Studio::updateEstimate() {
    QString text;
    const CatalogueModel *model = m_backend->catalogue().model(m_backend->modelId());
    if (running()) {
        const double elapsed = m_batchClock.elapsed() / 1000.0;
        const int remainingSteps = qMax(0, int(m_drafts.size()) * m_batch->request().steps - m_stepsDone);
        double remaining = -1;
        if (m_batchEstimate > 0)
            remaining = qMax(m_batchEstimate - elapsed, remainingSteps * qMax(0.0, m_secondsPerStep));
        else if (m_secondsPerStep > 0)
            remaining = remainingSteps * m_secondsPerStep;
        text = remaining >= 0 ? tr("About %1 left").arg(Backend::durationText(remaining))
                              : tr("Starting the engine");
    } else if (!upscaling() && m_backend->engineReady() && m_backend->modelInstalled() && model) {
        // Previews have about the model's own pixel count whatever their
        // shape, so the theme screen's measurements hold for them too.
        const Timing timing =
            Timings().get(Timings::key(model->id, model->levels.first().id, m_backend->probe().gpu().backend()));
        // With variety each preview is a run of its own, which pays the
        // start and the loading again.
        const double seconds = m_variety == Prompt::Variety::Seeds ? timing.estimate(m_count, model->steps)
                               : timing.estimate(1, model->steps) > 0 ? m_count * timing.estimate(1, model->steps)
                                                                       : -1;
        text = seconds > 0 ? tr("About %1 for %n previews", "", m_count).arg(Backend::durationText(seconds))
                           : tr("The first batch measures how fast this machine is.");
    }
    if (text != m_estimate) {
        m_estimate = text;
        emit estimateChanged();
    }
}

// ------------------------------------------------------------ upscale and save

void Studio::upscale(int index) {
    if (busy() || index < 0 || index >= m_drafts.size())
        return;
    if (m_backend->gpuBusy()) {
        setStatus(tr("The theme screen is using the GPU. Try again when its batch or keep is done."));
        return;
    }
    const QVariantMap draft = m_drafts.at(index).toMap();
    if (draft.value("state").toString() != "done")
        return;
    KeepSettings settings;
    settings.draftId = draft.value("id").toString();
    settings.install = false;
    settings.size = m_batchTarget;
    settings.colorPass = 0;
    settings.format = "png";

    m_keeper = std::make_unique<Keeper>(m_backend->catalogue(), m_backend->omarchy()->paths(),
                                        m_backend->engineProgram());
    Keeper *keeper = m_keeper.get();
    QString message;
    if (keeper->prepare(settings, m_backend->probe(), &message) != Keeper::Outcome::Ok) {
        m_keeper.reset();
        setStatus(message, true);
        return;
    }
    m_upscalingIndex = index;
    m_upscaleFraction = 0;
    // The tile shows the upscale's progress as the theme screen's tiles
    // show a keep's.
    setDraft(index, {{"keeping", true}, {"keepFraction", 0.0}});
    auto release = [this, keeper] {
        if (m_keeper.get() == keeper)
            m_keeper.release()->deleteLater();
        setDraft(m_upscalingIndex, {{"keeping", false}, {"keepFraction", -1.0}});
        m_upscalingIndex = -1;
        holdGpu();
        emit stateChanged();
        updateEstimate();
    };
    connect(keeper, &Keeper::progress, this, [this](double fraction) {
        m_upscaleFraction = fraction;
        setDraft(m_upscalingIndex, {{"keepFraction", fraction}});
        emit upscaleChanged();
    });
    connect(keeper, &Keeper::finished, this, [this, release, index, draft](const KeepResult &kept) {
        const QSize size = m_batchTarget;
        m_result = QVariantMap{{"id", kept.final.id},
                               {"image", QUrl::fromLocalFile(kept.final.image).toString()},
                               {"path", kept.final.image},
                               {"width", size.width()},
                               {"height", size.height()},
                               {"from", index},
                               {"seed", draft.value("seed")},
                               {"subject", m_batchPrompt}};
        emit resultChanged();
        setStatus(tr("Upscaled to %1×%2.").arg(size.width()).arg(size.height()));
        release();
    });
    connect(keeper, &Keeper::failed, this, [this, release](Keeper::Outcome outcome, const QString &text) {
        if (outcome == Keeper::Outcome::Cancelled)
            setStatus(tr("Upscale cancelled."));
        else
            setStatus(text, true);
        release();
    });
    setStatus(QString());
    keeper->start();
    holdGpu();
    emit stateChanged();
    updateEstimate();
}

QUrl Studio::saveFolder() const {
    const QString pictures = m_backend->omarchy()->paths().pictures;
    if (QFileInfo(pictures).isDir())
        return QUrl::fromLocalFile(pictures);
    const QString parent = QFileInfo(pictures).absolutePath();
    return QUrl::fromLocalFile(QFileInfo(parent).isDir() ? parent : QDir::homePath());
}

QString Studio::suggestedName() const {
    if (m_result.isEmpty())
        return {};
    return Keeper::fileName(m_result.value("subject").toString(), m_result.value("id").toString(), "png");
}

void Studio::saveImage(const QUrl &file, bool jpeg) {
    if (m_result.isEmpty() || saving())
        return;
    QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    if (path.isEmpty())
        return;
    QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != "png" && suffix != "jpg" && suffix != "jpeg") {
        suffix = jpeg ? "jpg" : "png";
        path += "." + suffix;
        if (QFileInfo::exists(path)) {
            setStatus(tr("%1 already exists. Save again with another name.").arg(path), true);
            return;
        }
    }
    const QString source = m_result.value("path").toString();
    const QString parameters = Keeper::parameters(Library().find(m_result.value("id").toString()).metadata);
    const int quality = m_backend->config().jpegQuality();
    setStatus(QString());
    // Encoding a large image takes a second or two, so it runs off the
    // thread that drives the window.
    m_saving.setFuture(QtConcurrent::run([path, source, parameters, quality] {
        return Saved{path, Keeper::saveCopy(source, path, parameters, quality)};
    }));
    emit stateChanged();
}
