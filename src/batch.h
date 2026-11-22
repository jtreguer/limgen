#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <memory>
#include <optional>

#include "catalogue.h"
#include "hardware.h"
#include "library.h"
#include "omarchy.h"
#include "palette.h"
#include "prompt.h"
#include "sdcppengine.h"

struct BatchSettings {
    QString modelId; // empty: the catalogue's default model
    QString subject;
    Prompt::Style style = Prompt::defaultStyle;
    QString fullPrompt; // used as given, instead of the built prompt, when not empty
    int count = 4;
    std::optional<quint32> seed; // random when unset; each next draft gets one more
    bool previews = false;       // the window shows them; commands do not
    // A variation starts from this image instead of noise: a library id or
    // an image file. Empty for a plain batch.
    QString source;
    double strength = 0.5; // how far the drafts may move from the source
};

// One batch of drafts, from the checks to the library: is the model
// installed, is the engine usable, is there enough video memory; then the
// theme snapshot, the engine run and the drafts filed with their metadata.
// `limgen generate` and the window both use it, so they make drafts the
// same way. It also measures the batch for the estimates (Timings).
class Batch : public QObject {
    Q_OBJECT

public:
    enum class Outcome { Ok, UnknownModel, UnknownSource, NotRunnable, Failed, Cancelled };
    Q_ENUM(Outcome)

    // The strengths a variation takes. Below 0.1 it changes next to
    // nothing; at 1 it ignores the source, which is a plain batch.
    static constexpr double minStrength = 0.1, maxStrength = 0.9;
    // The strength a variation runs at: within the range, and raised to the
    // lowest that runs a step when the model has too few for it; never past
    // the maximum.
    static double strengthFor(int steps, double strength);

    Batch(const Catalogue &catalogue, const OmarchyPaths &paths,
          const QString &engineProgram = SdCppEngine::defaultProgram(), QObject *parent = nullptr);
    ~Batch() override;

    // Checks, in this order: the model is known, its files are installed,
    // the source (for a variation) can be read, the engine probe found a
    // usable engine, the GPU has enough free memory.
    // Then reads the theme once (a theme switch during the batch does not
    // change what it was made for) and builds the engine request. Returns
    // Ok, or the outcome with *message saying what is wrong and what to do.
    Outcome prepare(const BatchSettings &settings, const EngineProbe &probe, QString *message);
    void start();
    void cancel();
    bool isRunning() const { return m_running; }

    const CatalogueModel *model() const { return m_model; }
    const EngineRequest &request() const { return m_request; }
    QString themeSlug() const { return m_record.themeSlug; }
    QString styleId() const { return m_record.style; }
    // For a variation: the source as the sidecar records it, with the
    // strength used; empty for a plain batch.
    QJsonObject source() const { return m_record.source; }
    Gpu gpu() const { return m_gpu; }
    // How long the batch will take on this machine, in seconds, from earlier
    // batches of the same model and backend; -1 when not known yet.
    double estimate() const;

signals:
    void progress(int image, int step, int steps, double secondsPerStep);
    void sampled(int image);
    void preview(int image, const QString &path);
    void finished(const QList<LibraryEntry> &entries);
    void failed(Batch::Outcome outcome, const QString &message);

private:
    void engineProgress(int image, int step, int steps, double secondsPerStep);
    void engineFinished(const QStringList &drafts);
    void engineFailed(const EngineError &error);
    void recordTiming();
    QString timingKey() const;
    void cleanUp();

    Catalogue m_catalogue;
    OmarchyPaths m_paths;
    QString m_sourceImage; // the file a variation starts from
    std::unique_ptr<SdCppEngine> m_engine;
    const CatalogueModel *m_model = nullptr;
    const ModelLevel *m_level = nullptr;
    EngineRequest m_request;
    DraftRecord m_record;
    quint32 m_seed = 0;
    QString m_backend;
    Gpu m_gpu;
    bool m_running = false;

    QElapsedTimer m_clock;
    qint64 m_lastSampledMs = -1;
    QList<double> m_stepSeconds;
};
