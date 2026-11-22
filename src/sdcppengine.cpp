#include "sdcppengine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>

namespace {

// Runs the engine for a quick question and returns what it printed, or a
// null array when it could not start or took longer than the timeout.
QByteArray ask(const QString &program, const QStringList &arguments, int timeoutMs) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    QByteArray output = process.readAll();
    if (output.isNull())
        output = "";
    return output;
}

// Output is kept for error messages only; the end of it is what matters.
constexpr qsizetype outputLimit = 256 * 1024;

} // namespace

QString EngineDevice::backend() const {
    static const QRegularExpression number("[0-9]+$");
    return QString(name).remove(number).toLower();
}

EngineDevice EngineProbe::gpu() const {
    for (const auto &device : devices)
        if (device.isGpu())
            return device;
    return {};
}

SdCppEngine::SdCppEngine(const QString &program, QObject *parent) : Engine(parent), m_program(program) {
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyRead, this, &SdCppEngine::readOutput);
    connect(&m_process, &QProcess::finished, this, &SdCppEngine::ended);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail({EngineError::EngineMissing, tr("Cannot start the engine at %1.").arg(m_program),
                  m_process.errorString()});
    });
    m_killTimer.setSingleShot(true);
    m_killTimer.setInterval(2000);
    connect(&m_killTimer, &QTimer::timeout, &m_process, &QProcess::kill);
}

SdCppEngine::~SdCppEngine() {
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }
}

QString SdCppEngine::defaultProgram() {
    const QString overridden = qEnvironmentVariable("LIMGEN_SD_CLI");
    return overridden.isEmpty() ? QString("/usr/lib/limgen/sd-cli") : overridden;
}

QString SdCppEngine::parseVersion(const QByteArray &output) {
    static const QRegularExpression version(R"(stable-diffusion\.cpp version (\S+?),? commit)");
    return version.match(QString::fromUtf8(output)).captured(1);
}

QList<EngineDevice> SdCppEngine::parseDevices(const QByteArray &output) {
    QList<EngineDevice> devices;
    for (const QString &line : QString::fromUtf8(output).split('\n', Qt::SkipEmptyParts)) {
        const int tab = line.indexOf('\t');
        if (tab <= 0)
            continue;
        devices << EngineDevice{line.left(tab).trimmed(), line.mid(tab + 1).trimmed()};
    }
    return devices;
}

EngineProbe SdCppEngine::probe() const {
    EngineProbe probe;
    const QFileInfo info(m_program);
    if (!info.isFile() || !info.isExecutable()) {
        probe.error = {EngineError::EngineMissing,
                       tr("No engine at %1. Install the limgen-engine-vulkan package, or set LIMGEN_SD_CLI to an "
                          "sd-cli built with bin/build-engine.")
                           .arg(m_program),
                       {}};
        return probe;
    }
    const QByteArray versionOutput = ask(m_program, {"--version"}, 5000);
    probe.version = parseVersion(versionOutput);
    if (probe.version != QLatin1String(pinnedVersion)) {
        probe.error = {EngineError::WrongVersion,
                       tr("The engine at %1 is %2, but this limgen needs %3.")
                           .arg(m_program, probe.version.isEmpty() ? tr("unknown") : probe.version, pinnedVersion),
                       QString::fromUtf8(versionOutput).trimmed()};
        return probe;
    }
    // Listing devices initialises every backend, which can take a while on
    // the first run after boot.
    probe.devices = parseDevices(ask(m_program, {"--list-devices"}, 20000));
    if (probe.gpu().name.isEmpty())
        probe.error = {EngineError::NoGpu,
                       tr("The engine sees no GPU. Check the GPU driver and, for the Vulkan engine, the Vulkan "
                          "loader (vulkan-icd-loader) and your driver's Vulkan support."),
                       {}};
    return probe;
}

QStringList SdCppEngine::arguments(const EngineRequest &request) {
    QStringList arguments;
    if (request.mode == EngineRequest::Mode::Upscale) {
        // ESRGAN at tile size 256 was 1 s faster than the default 128 in L0;
        // 512 ran out of memory on an 8 GB card.
        arguments << "-M" << "upscale" << "--upscale-model" << request.files.value("upscale-model") << "-i"
                  << request.input << "-o" << draftPaths(request).first() << "--upscale-tile-size"
                  << QString::number(request.upscaleTileSize);
        return arguments;
    }
    // Roles are named after the sd-cli options that take them.
    for (const char *role : {"model", "diffusion-model", "llm", "vae"})
        if (request.files.contains(role))
            arguments << QString("--%1").arg(role) << request.files.value(role);
    arguments << "--prompt" << request.prompt;
    // Turbo models run without guidance (cfg 1) and ignore a negative prompt.
    if (request.cfg > 1.0 && !request.negative.isEmpty())
        arguments << "--negative-prompt" << request.negative;
    arguments << "--width" << QString::number(request.width) << "--height" << QString::number(request.height)
              << "--steps" << QString::number(request.steps) << "--cfg-scale" << QString::number(request.cfg);
    if (!request.sampler.isEmpty())
        arguments << "--sampling-method" << request.sampler;
    arguments << "--seed" << QString::number(request.seed) << "--batch-count" << QString::number(request.count)
              << "--output" << request.outputDirectory + "/draft-%d.png";
    if (!request.initImage.isEmpty())
        arguments << "--init-img" << request.initImage << "--strength" << QString::number(request.strength);
    // The tiny decoder draws previews only; without --taesd-preview-only the
    // engine would also decode the drafts with it, at lower quality.
    if (request.previews && request.files.contains("tae"))
        arguments << "--preview" << "tae" << "--tae" << request.files.value("tae") << "--taesd-preview-only"
                  << "--preview-path" << request.outputDirectory + "/preview/%03d.png" << "--preview-interval"
                  << QString::number(request.previewInterval);
    if (request.vaeTiling)
        arguments << "--vae-tiling";
    return arguments;
}

QStringList SdCppEngine::draftPaths(const EngineRequest &request) {
    if (request.mode == EngineRequest::Mode::Upscale)
        return {request.outputDirectory + "/upscaled.png"};
    QStringList paths;
    for (int i = 0; i < request.count; ++i)
        paths << QString("%1/draft-%2.png").arg(request.outputDirectory).arg(i);
    return paths;
}

EngineError SdCppEngine::classify(int exitCode, QProcess::ExitStatus status, const QString &output) {
    QStringList errors;
    for (const QString &line : output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts))
        if (line.contains("[E] "))
            errors << line.mid(line.indexOf("[E] ") + 4).trimmed();
    const QString detail = errors.isEmpty() ? output.right(2000).trimmed() : errors.join('\n');
    // A clean exit with no error line is a success, whatever the warnings
    // said: auto-fit can warn about memory and then manage.
    if (status == QProcess::NormalExit && exitCode == 0 && errors.isEmpty())
        return {};
    const QString lower = output.toLower();

    static const char *const memory[] = {"out of memory", "cudamalloc failed", "erroroutofdevicememory",
                                         "cannot make enough memory available", "failed to allocate"};
    for (const char *pattern : memory)
        if (lower.contains(QLatin1String(pattern)))
            return {EngineError::OutOfMemory, tr("The GPU ran out of video memory."), detail};
    if (lower.contains("cannot inspect model source") || lower.contains("failed to open"))
        return {EngineError::MissingFile, tr("The engine could not open a model file."), detail};
    if (lower.contains("get sd version from file failed"))
        return {EngineError::UnsupportedModel, tr("The engine does not recognise one of the model files."), detail};
    if (status != QProcess::NormalExit)
        return {EngineError::Failed, tr("The engine stopped unexpectedly."), detail};
    // The pinned release can log an error and still exit 0 (seen with an
    // ESRGAN upscale out of memory), so an error line is a failure too.
    return {EngineError::Failed, tr("The engine failed."), detail};
}

void SdCppEngine::start(const EngineRequest &request) {
    if (isRunning())
        return;
    m_request = request;
    m_retried = false;
    m_cancelled = false;
    launch();
}

void SdCppEngine::launch() {
    QDir().mkpath(m_request.outputDirectory);
    // Previews of an earlier attempt (the run before an out-of-memory retry)
    // would otherwise be reported as this run's.
    QDir(m_request.outputDirectory + "/preview").removeRecursively();
    if (m_request.previews)
        QDir().mkpath(m_request.outputDirectory + "/preview");
    m_parser = m_request.mode == EngineRequest::Mode::Upscale ? std::make_unique<ProgressParser>(1, 0)
                                                                : std::make_unique<ProgressParser>(m_request.count, m_request.samplingSteps());
    m_output.clear();
    m_previewsReported = 0;
    m_process.start(m_program, arguments(m_request));
}

void SdCppEngine::cancel() {
    // Once only: a caller may ask again and again (a command polls for
    // Ctrl+C), and restarting the timer would put off the kill for ever
    // when the engine ignores SIGTERM.
    if (!isRunning() || m_cancelled)
        return;
    m_cancelled = true;
    m_process.terminate();
    m_killTimer.start();
}

void SdCppEngine::readOutput() {
    const QByteArray chunk = m_process.readAll();
    m_output += QString::fromUtf8(chunk);
    if (m_output.size() > outputLimit)
        m_output = m_output.right(outputLimit);
    if (!m_parser)
        return;
    for (const ProgressEvent &event : m_parser->feed(chunk)) {
        if (event.kind == ProgressEvent::Step) {
            emit progress(event.image, event.step, event.steps, event.secondsPerStep);
            reportPreviews(false);
        } else if (event.kind == ProgressEvent::Sampled) {
            emit sampled(event.image);
        }
    }
}

// Previews are written in place, not renamed into place, so the newest one
// may still be half written; a preview is reported once the next exists or
// the batch is over.
void SdCppEngine::reportPreviews(bool all) {
    if (!m_request.previews)
        return;
    const QStringList files =
        QDir(m_request.outputDirectory + "/preview").entryList({"*.png"}, QDir::Files, QDir::Name);
    const int complete = all ? files.size() : files.size() - 1;
    const int steps = m_parser ? m_parser->steps() : m_request.samplingSteps();
    const int perImage = qMax(1, steps / qMax(1, m_request.previewInterval));
    for (int i = m_previewsReported; i < complete; ++i)
        emit preview(qMin(i / perImage, m_request.count - 1),
                     m_request.outputDirectory + "/preview/" + files.at(i));
    m_previewsReported = qMax(m_previewsReported, complete);
}

void SdCppEngine::ended(int exitCode, QProcess::ExitStatus status) {
    m_killTimer.stop();
    if (m_parser)
        for (const ProgressEvent &event : m_parser->finish())
            if (event.kind == ProgressEvent::Sampled)
                emit sampled(event.image);

    const QStringList drafts = draftPaths(m_request);
    if (m_cancelled) {
        // Partial files of a cancelled batch are of no use to anyone.
        for (const QString &draft : drafts)
            QFile::remove(draft);
        QDir(m_request.outputDirectory + "/preview").removeRecursively();
        fail({EngineError::Cancelled, tr("Cancelled."), {}});
        return;
    }

    const EngineError error = classify(exitCode, status, m_output);
    // Decoding all drafts at once is the peak of a batch, so the one retry
    // decodes in tiles, which needs much less video memory. An upscale
    // retries with smaller tiles, for the same reason.
    if (error.kind == EngineError::OutOfMemory && !m_retried) {
        bool again = false;
        if (m_request.mode == EngineRequest::Mode::Upscale && m_request.upscaleTileSize > 128) {
            m_request.upscaleTileSize = 128;
            again = true;
        } else if (m_request.mode == EngineRequest::Mode::Generate && !m_request.vaeTiling) {
            m_request.vaeTiling = true;
            again = true;
        }
        if (again) {
            m_retried = true;
            for (const QString &draft : drafts)
                QFile::remove(draft);
            launch();
            return;
        }
    }
    if (error.kind != EngineError::None) {
        fail(error);
        return;
    }
    for (int i = 0; i < drafts.size(); ++i) {
        if (QImageReader(drafts.at(i)).size() != QSize(m_request.width, m_request.height)) {
            const QString message = m_request.mode == EngineRequest::Mode::Upscale
                                        ? tr("The engine reported success, but the upscaled image is missing or the wrong size.")
                                        : tr("The engine reported success, but draft %1 is missing or the wrong size.").arg(i + 1);
            fail({EngineError::Failed, message, m_output.right(2000).trimmed()});
            return;
        }
    }
    reportPreviews(true);
    emit finished(drafts);
}

void SdCppEngine::fail(const EngineError &error) {
    m_killTimer.stop();
    emit failed(error);
}
