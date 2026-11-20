#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

// One batch for an engine to make: everything it needs, already decided.
struct EngineRequest {
    QMap<QString, QString> files; // sd-cli role ("diffusion-model", "llm", "vae", "tae", "model") to path
    QString prompt;
    QString negative; // passed only to models that use guidance (cfg above 1)
    int width = 0, height = 0;
    int steps = 0;
    double cfg = 1.0;
    QString sampler;
    quint32 seed = 0; // the batch's first image; each next one gets seed + 1
    int count = 1;
    QString outputDirectory; // drafts land here as draft-0.png, draft-1.png, ...
    bool previews = false;   // needs files["tae"]; previews land in outputDirectory/preview/
    int previewInterval = 2;
    bool vaeTiling = false;  // decode in tiles, for less video memory
};

struct EngineError {
    enum Kind {
        None,
        EngineMissing,    // no engine program where it should be
        WrongVersion,     // not the engine release limgen was built for
        NoGpu,            // the engine sees no GPU
        OutOfMemory,      // ran out of video memory, also after a retry
        MissingFile,      // a model file could not be opened
        UnsupportedModel, // a file is not a model the engine knows
        Failed,           // anything else the engine reported
        Cancelled,
    };
    Kind kind = None;
    QString message; // for the user, one sentence
    QString detail;  // the engine's own words, for the curious and for bug reports
};
Q_DECLARE_METATYPE(EngineError)

// The program that runs a model. The first engine is stable-diffusion.cpp's
// sd-cli; this interface leaves room for a second.
class Engine : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void start(const EngineRequest &request) = 0;
    virtual void cancel() = 0;
    virtual bool isRunning() const = 0;

signals:
    void progress(int image, int step, int steps, double secondsPerStep);
    void sampled(int image);
    // A preview of `image` that is complete on disk.
    void preview(int image, const QString &path);
    // The drafts, in batch order, all checked to exist at the requested size.
    void finished(const QStringList &drafts);
    void failed(const EngineError &error);
};
