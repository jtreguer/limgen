#pragma once

#include <QList>
#include <QProcess>
#include <QTimer>
#include <memory>

#include "engine.h"
#include "progress.h"

// An engine device as `sd-cli --list-devices` names it: "Vulkan0",
// "CUDA0", "CPU".
struct EngineDevice {
    QString name;
    QString description;
    bool isGpu() const { return !name.startsWith("CPU"); }
    // "vulkan", "cuda", "rocm"...: the device name without its number.
    QString backend() const;
};

// What the engine program says about itself.
struct EngineProbe {
    EngineError error; // None when the engine is usable
    QString version;
    QList<EngineDevice> devices;
    EngineDevice gpu() const; // the first GPU device, or an empty one
};

// An engine build on this machine: one per GPU backend, so the Vulkan and
// CUDA builds can both be installed and the user picks one.
struct EngineInstall {
    QString backend; // "vulkan", "cuda", "rocm"; empty for LIMGEN_SD_CLI, whose backend only a probe tells
    QString program;
};

// stable-diffusion.cpp's sd-cli, run as a child process. limgen builds its
// arguments for one pinned release only (docs/technical.md section 4).
class SdCppEngine : public Engine {
    Q_OBJECT

public:
    static constexpr const char *pinnedVersion = "master-1106-532d2c1";

    explicit SdCppEngine(const QString &program = defaultProgram(), QObject *parent = nullptr);
    ~SdCppEngine() override;

    // The engine builds found, in the order vulkan, cuda, rocm, then any
    // other backend folder by name. LIMGEN_SD_CLI names the only one;
    // otherwise each engine package installs <dir>/<backend>/sd-cli, where
    // <dir> is LIMGEN_ENGINE_DIR or /usr/lib/limgen. A development build,
    // <dir>/<backend>/bin/sd-cli (bin/build-engine), is found as well.
    static QList<EngineInstall> installed();
    static QString engineDirectory();
    // The program for `backend` when it is installed, else the first one
    // installed, else where the Vulkan package would put it (so the error
    // names a real path). LIMGEN_SD_CLI wins over all of them.
    static QString programFor(const QString &backend);
    // programFor() the engine chosen in the settings (config.json).
    static QString defaultProgram();
    QString program() const { return m_program; }

    // Runs --version and --list-devices; a few seconds at most.
    EngineProbe probe() const;

    // The one place sd-cli options are built. Every option used here is
    // checked by the tests against the pinned release's --help.
    static QStringList arguments(const EngineRequest &request);

    static QString parseVersion(const QByteArray &output);
    static QList<EngineDevice> parseDevices(const QByteArray &output);
    // What went wrong, from how the process ended and what it printed.
    static EngineError classify(int exitCode, QProcess::ExitStatus status, const QString &output);
    // Whether the engine said it saved all `count` images ("4/4 images
    // saved"), which it prints after the last file is written.
    static bool savedAll(const QString &output, int count);
    // Draft paths for a request, in batch order.
    static QStringList draftPaths(const EngineRequest &request);

    void start(const EngineRequest &request) override;
    void cancel() override;
    bool isRunning() const override { return m_process.state() != QProcess::NotRunning; }
    // Whether the current or last run was retried after running out of memory.
    bool retried() const { return m_retried; }

private:
    void launch();
    void readOutput();
    void reportPreviews(bool all);
    void ended(int exitCode, QProcess::ExitStatus status);
    void fail(const EngineError &error);

    QString m_program;
    QProcess m_process;
    QTimer m_killTimer;
    EngineRequest m_request;
    std::unique_ptr<ProgressParser> m_parser;
    QString m_output;
    bool m_retried = false;
    bool m_cancelled = false;
    int m_previewsReported = 0;
};
