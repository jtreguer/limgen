#include "keeper.h"

#include "colorpass.h"
#include "palette.h"
#include "resample.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace {

// Without a monitor to measure (no Hyprland, or a command run over SSH),
// the final is made at the size the finish was measured at in L0.
const QSize fallbackSize(2560, 1440);

// The theme's backgrounds are read small: their colour statistics need no
// more, and a large JPEG decodes much faster at a fraction of its size.
QImage readSmall(const QString &path) {
    QImageReader reader(path);
    const QSize size = reader.size();
    if (size.isValid() && size.width() > 512)
        reader.setScaledSize(size.scaled(512, 512, Qt::KeepAspectRatio));
    return reader.read();
}

struct FinishJob {
    QString upscaled, output, format, parameters;
    QSize size;
    double colorPass = 0;
    int jpegQuality = 92;
    QStringList backgrounds;
    QList<QColor> palette;
};

} // namespace

Keeper::Keeper(const Catalogue &catalogue, const OmarchyPaths &paths, const QString &engineProgram, QObject *parent)
    : QObject(parent), m_catalogue(catalogue), m_paths(paths),
      m_engine(std::make_unique<SdCppEngine>(engineProgram)) {
    qRegisterMetaType<KeepResult>();
    SdCppEngine *engine = m_engine.get();
    // The upscale is most of the work; saving takes the last tenth.
    connect(engine, &Engine::progress, this,
            [this](int, int step, int steps, double) { emit progress(0.9 * step / qMax(1, steps)); });
    connect(engine, &Engine::finished, this, &Keeper::engineFinished);
    connect(engine, &Engine::failed, this, &Keeper::engineFailed);
    connect(&m_saving, &QFutureWatcher<Finished>::finished, this, &Keeper::saved);
}

Keeper::~Keeper() {
    // As in Batch: stopping the engine makes it report a failure, which this
    // half-destroyed object must not hear. The saving thread is waited for,
    // since it writes into the work folder.
    m_engine->disconnect(this);
    m_engine.reset();
    m_saving.disconnect(this);
    m_saving.waitForFinished();
    if (m_running)
        cleanUp();
}

QString Keeper::fileName(const QString &subject, const QString &draftId, const QString &format) {
    // Accents come off ("forêt" becomes "foret"); other scripts leave nothing,
    // and then the name says "background".
    const QString decomposed = subject.normalized(QString::NormalizationForm_KD);
    QString slug;
    for (const QChar c : decomposed) {
        if (c.unicode() < 128 && c.isLetterOrNumber())
            slug += c.toLower();
        else if (c.category() == QChar::Mark_NonSpacing)
            continue;
        else if (!slug.isEmpty() && !slug.endsWith('-'))
            slug += '-';
    }
    slug = slug.left(40);
    while (slug.endsWith('-'))
        slug.chop(1);
    if (slug.isEmpty())
        slug = "background";
    return QString("limgen-%1-%2.%3").arg(slug, draftId.right(6), format == "jpeg" ? "jpg" : "png");
}

QString Keeper::parameters(const QJsonObject &metadata) {
    QStringList lines{metadata.value("prompt").toString()};
    const QString negative = metadata.value("negative").toString();
    if (!negative.isEmpty())
        lines << "Negative prompt: " + negative;
    const QJsonArray size = metadata.value("size").toArray();
    lines << QString("Steps: %1, Sampler: %2, CFG scale: %3, Seed: %4, Size: %5x%6, Model: %7")
                 .arg(metadata.value("steps").toInt())
                 .arg(metadata.value("sampler").toString())
                 .arg(metadata.value("cfg").toDouble())
                 .arg(qint64(metadata.value("seed").toDouble()))
                 .arg(size.at(0).toInt())
                 .arg(size.at(1).toInt())
                 .arg(metadata.value("model").toObject().value("id").toString());
    return lines.join('\n');
}

Keeper::Outcome Keeper::prepare(const KeepSettings &settings, const EngineProbe &probe, QString *message) {
    auto refuse = [message](Outcome outcome, const QString &text) {
        if (message)
            *message = text;
        return outcome;
    };
    m_settings = settings;
    m_result = KeepResult{};
    m_backgrounds.clear();
    m_palette.clear();

    m_result.draft = Library().find(settings.draftId);
    if (!m_result.draft.isValid())
        return refuse(Outcome::UnknownDraft,
                      tr("No draft %1 in the library. limgen generate prints the ids of the drafts it makes.")
                          .arg(settings.draftId));
    const QJsonObject &draft = m_result.draft.metadata;
    if (draft.value("kind").toString() != "draft")
        return refuse(Outcome::UnknownDraft, tr("%1 is a finished image, not a draft.").arg(settings.draftId));

    const Omarchy omarchy(m_paths);
    if (!settings.theme.isEmpty() && omarchy.themeDirectory(settings.theme).isEmpty())
        return refuse(Outcome::UnknownTheme, tr("No theme %1 in %2.").arg(settings.theme, m_paths.themes.join(", ")));
    const QString folder = omarchy.keepDirectory(settings.theme);
    m_result.theme = settings.theme;

    const CatalogueFile *upscaler = m_catalogue.upscaler();
    const QString modelsDirectory = Catalogue::defaultModelsDirectory();
    if (!upscaler || Catalogue::state(*upscaler, modelsDirectory) != Catalogue::FileState::Ok)
        return refuse(Outcome::NotRunnable,
                      tr("The upscaler that finishes kept drafts is not installed (%1 in %2). limgen models pull "
                         "downloads it with any model.")
                          .arg(upscaler ? upscaler->folder + "/" + upscaler->name : QString(), modelsDirectory));
    if (probe.error.kind != EngineError::None)
        return refuse(Outcome::NotRunnable, probe.error.message);

    const QSize draftSize = QImageReader(m_result.draft.image).size();
    if (!draftSize.isValid())
        return refuse(Outcome::Failed, tr("Cannot read %1.").arg(m_result.draft.image));
    m_size = settings.size;
    if (m_size.isEmpty()) {
        const Monitor monitor = Omarchy::largest(omarchy.monitors());
        m_size = monitor.width > 0 ? QSize(monitor.width, monitor.height) : fallbackSize;
    }

    // A draft kept before, in either format, is not made again.
    const QString subject = draft.value("subject").toString();
    m_result.installed = folder + "/" + fileName(subject, settings.draftId, settings.format);
    for (const char *format : {"png", "jpeg"}) {
        const QString existing = folder + "/" + fileName(subject, settings.draftId, format);
        if (QFileInfo::exists(existing)) {
            m_result.installed = existing;
            m_result.alreadyKept = true;
        }
    }

    // The colour pass aims at the theme's own backgrounds when it has any,
    // and at its palette otherwise: from its colors.toml, or, for the theme
    // the draft was made for, from the draft's record.
    if (settings.colorPass > 0 && !settings.theme.isEmpty()) {
        m_backgrounds = omarchy.themeBackgrounds(settings.theme);
        QFile colors(omarchy.themeDirectory(settings.theme) + "/colors.toml");
        if (colors.open(QIODevice::ReadOnly)) {
            const Palette palette = Palette::fromToml(QString::fromUtf8(colors.readAll()));
            for (const PaletteColor &colour : palette.colors)
                m_palette << QColor(colour.hex);
        }
        const QJsonObject made = draft.value("theme").toObject();
        if (m_palette.isEmpty() && made.value("slug").toString() == settings.theme)
            for (const QJsonValue &hex : made.value("palette").toObject())
                m_palette << QColor(hex.toString());
    }

    m_request = EngineRequest{};
    m_request.mode = EngineRequest::Mode::Upscale;
    m_request.files.insert("upscale-model", Catalogue::path(*upscaler, modelsDirectory));
    m_request.input = m_result.draft.image;
    m_request.width = draftSize.width() * 4;
    m_request.height = draftSize.height() * 4;
    m_request.outputDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                                QString("/limgen/keep-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
    return Outcome::Ok;
}

void Keeper::start() {
    if (m_running || !m_result.draft.isValid())
        return;
    m_running = true;
    m_cancelled = false;
    if (m_result.alreadyKept) {
        // Reported from the event loop, as a finished upscale would be, so a
        // caller that waits in one hears it.
        QTimer::singleShot(0, this, [this] {
            if (!m_running)
                return;
            m_running = false;
            emit progress(1);
            emit finished(m_result);
        });
        return;
    }
    m_engine->start(m_request);
}

void Keeper::cancel() {
    if (!m_running)
        return;
    m_cancelled = true;
    if (m_engine->isRunning())
        m_engine->cancel();
    else if (!m_saving.isRunning())
        fail(Outcome::Cancelled, tr("Cancelled."));
}

void Keeper::engineFinished(const QStringList &files) {
    if (m_cancelled) {
        fail(Outcome::Cancelled, tr("Cancelled."));
        return;
    }
    QJsonObject metadata = m_result.draft.metadata;
    metadata.insert("size", QJsonArray{m_size.width(), m_size.height()});

    FinishJob job;
    job.upscaled = files.value(0);
    job.format = m_settings.format == "jpeg" ? "jpeg" : "png";
    job.output = m_request.outputDirectory + (job.format == "jpeg" ? "/final.jpg" : "/final.png");
    job.parameters = parameters(metadata);
    job.size = m_size;
    job.colorPass = m_settings.colorPass;
    job.jpegQuality = m_settings.jpegQuality;
    job.backgrounds = m_backgrounds;
    job.palette = m_palette;
    // Resizing and the colour pass take a second or two on a large image,
    // so they run off the thread that drives the window.
    m_saving.setFuture(QtConcurrent::run([job] {
        Finished done;
        QImage image(job.upscaled);
        if (image.isNull()) {
            done.error = tr("Cannot read the upscaled image %1.").arg(job.upscaled);
            return done;
        }
        image = Resample::cover(image, job.size);
        if (job.colorPass > 0) {
            LabStats target;
            if (!job.backgrounds.isEmpty()) {
                QList<QImage> images;
                for (const QString &path : job.backgrounds)
                    images << readSmall(path);
                target = ColorPass::statistics(images);
                done.colorTarget = "backgrounds";
            }
            if (!target.valid && !job.palette.isEmpty()) {
                target = ColorPass::statistics(job.palette);
                done.colorTarget = "palette";
            }
            if (target.valid)
                image = ColorPass::apply(image, target, job.colorPass);
            else
                done.colorTarget.clear();
        }
        image.setText("parameters", job.parameters);
        QImageWriter writer(job.output, job.format.toLatin1());
        if (job.format == "jpeg")
            writer.setQuality(job.jpegQuality);
        if (!writer.write(image)) {
            done.error = tr("Cannot write %1: %2").arg(job.output, writer.errorString());
            return done;
        }
        done.file = job.output;
        return done;
    }));
}

void Keeper::engineFailed(const EngineError &error) {
    if (error.kind == EngineError::Cancelled || m_cancelled) {
        fail(Outcome::Cancelled, tr("Cancelled."));
        return;
    }
    QString message = error.message;
    if (error.kind == EngineError::OutOfMemory)
        message += " " + tr("Finishing needs about 2 GB of free video memory, also with smaller tiles. Close "
                            "programs that use the GPU and keep it again.");
    if (!error.detail.isEmpty())
        message += "\n" + error.detail;
    const bool machine = error.kind == EngineError::OutOfMemory || error.kind == EngineError::EngineMissing ||
                         error.kind == EngineError::NoGpu || error.kind == EngineError::WrongVersion;
    fail(machine ? Outcome::NotRunnable : Outcome::Failed, message);
}

void Keeper::saved() {
    const Finished done = m_saving.result();
    if (m_cancelled) {
        fail(Outcome::Cancelled, tr("Cancelled."));
        return;
    }
    if (!done.error.isEmpty()) {
        fail(Outcome::Failed, done.error);
        return;
    }

    QJsonObject metadata = m_result.draft.metadata;
    for (const char *key : {"id", "kind", "created"})
        metadata.remove(key);
    metadata.insert("size", QJsonArray{m_size.width(), m_size.height()});
    QJsonObject finish{{"from", m_result.draft.id},
                       {"upscaler", m_catalogue.upscaler()->key},
                       {"colorPass", done.colorTarget.isEmpty() ? 0.0 : m_settings.colorPass},
                       {"colorTarget", done.colorTarget.isEmpty() ? QJsonValue() : QJsonValue(done.colorTarget)},
                       {"theme", m_result.theme.isEmpty() ? QJsonValue() : QJsonValue(m_result.theme)},
                       {"size", QJsonArray{m_size.width(), m_size.height()}},
                       {"format", m_settings.format == "jpeg" ? "jpeg" : "png"}};
    if (m_settings.format == "jpeg")
        finish.insert("quality", m_settings.jpegQuality);
    metadata.insert("finish", finish);

    QString error;
    m_result.final = Library().addFinal(done.file, metadata, QDateTime::currentDateTimeUtc(), &error);
    if (!m_result.final.isValid()) {
        fail(Outcome::Failed, error);
        return;
    }

    // Written beside its name and renamed into place. The .part name is not
    // an image name, so Omarchy's background cycling never picks it up, and
    // rename() refuses an existing file, so nothing is overwritten.
    const QString folder = QFileInfo(m_result.installed).absolutePath();
    if (!QDir().mkpath(folder)) {
        QFile::remove(m_result.final.image);
        QFile::remove(m_result.final.sidecar);
        m_result.final = LibraryEntry{};
        fail(Outcome::Failed, tr("Cannot create %1.").arg(folder));
        return;
    }
    const QString part = m_result.installed + ".part";
    QFile::remove(part);
    if (!QFile::copy(m_result.final.image, part) || !QFile::rename(part, m_result.installed)) {
        QFile::remove(part);
        // The final was filed a moment ago for this keep only; without the
        // copy it is a keep that did not happen.
        QFile::remove(m_result.final.image);
        QFile::remove(m_result.final.sidecar);
        m_result.final = LibraryEntry{};
        fail(Outcome::Failed, tr("Cannot write %1.").arg(m_result.installed));
        return;
    }
    cleanUp();
    m_running = false;
    emit progress(1);
    emit finished(m_result);
}

void Keeper::fail(Outcome outcome, const QString &message) {
    cleanUp();
    m_running = false;
    emit failed(outcome, message);
}

void Keeper::cleanUp() {
    if (!m_request.outputDirectory.isEmpty())
        QDir(m_request.outputDirectory).removeRecursively();
}
