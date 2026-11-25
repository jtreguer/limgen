#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QSize>
#include <QStringList>
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

    // A generic batch (the Generic screen, limgen image): the subject is the
    // whole prompt, used as typed with the chosen palette's words, without a
    // style and without the current theme. The drafts take the target's
    // shape at the model's own pixel count, and are finished to the target.
    bool generic = false;
    QSize target;              // empty for the largest monitor's size
    QString paletteTheme;      // a theme whose palette the prompt uses
    QStringList paletteColors; // or colours chosen by hand ("#rrggbb"); neither for no palette
    // How much the previews differ beyond their seeds (Prompt::Variety).
    // With hints, each draft has its own prompt, and the engine runs once
    // per draft, since sd-cli takes one prompt per run.
    Prompt::Variety variety = Prompt::Variety::Seeds;
};

// One batch of drafts, from the checks to the library: is the model
// installed, is the engine usable, is there enough video memory; then the
// theme snapshot, the engine run and the drafts filed with their metadata.
// `limgen generate` and the window both use it, so they make drafts the
// same way. It also measures the batch for the estimates (Timings).
class Batch : public QObject {
    Q_OBJECT

public:
    enum class Outcome { Ok, UnknownModel, UnknownSource, UnknownPalette, NotRunnable, Failed, Cancelled };
    Q_ENUM(Outcome)

    // The strengths a variation takes. Below 0.1 it changes next to
    // nothing; at 1 it ignores the source, which is a plain batch.
    static constexpr double minStrength = 0.1, maxStrength = 0.9;
    // The strength a variation runs at: within the range, and raised to the
    // lowest that runs a step when the model has too few for it; never past
    // the maximum.
    static double strengthFor(int steps, double strength);
    // The draft size for a target: the target's ratio at about the model's
    // own pixel count (`native`), each side a multiple of 64 from 256 to
    // 2048. 1344x768 for a 16:9 target and a 1344x768 model, 768x1344 for
    // 9:16, 1024x1024 for a square. An empty target gives `native`.
    static QSize draftSize(const QSize &target, const QSize &native);
    // The largest ratio either way a generic target may have: wider than
    // 4:1, a draft side would leave 256 to 2048.
    static constexpr double maxRatio = 4.0;

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
    // A generic batch's target size; empty for a theme batch.
    QSize target() const { return m_target; }
    // Each draft's variety hint and full prompt, in batch order. The hints
    // are empty strings without variety, and every prompt is then the
    // request's.
    QStringList hints() const { return m_hints; }
    QStringList prompts() const { return m_prompts; }
    // Whether the engine runs once per draft (the prompts differ).
    bool runsPerDraft() const { return m_perDraft; }
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
    void startRun();
    void fileDrafts(const QStringList &drafts);
    // The engine request for one draft of a batch that runs per draft.
    EngineRequest runRequest(int index) const;
    // The draft an engine event is about: the run's, when runs are per draft.
    int draftIndex(int image) const { return m_perDraft ? m_run : image; }
    void recordTiming(int count);
    QString timingKey() const;
    void cleanUp();

    Catalogue m_catalogue;
    OmarchyPaths m_paths;
    QString m_sourceImage; // the file a variation starts from
    QSize m_target;
    QStringList m_hints, m_prompts;
    bool m_perDraft = false;
    int m_run = 0;                // the draft whose run is going, when runs are per draft
    QStringList m_runDrafts;      // the drafts the runs made so far
    bool m_stopping = false;      // cancelled between two runs
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
