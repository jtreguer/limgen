#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QLockFile>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <memory>

#include "batch.h"
#include "catalogue.h"
#include "config.h"
#include "hardware.h"
#include "modelstore.h"
#include "palette.h"
#include "sdcppengine.h"

class Omarchy;

// The window's state for QML. File and process work lives in the classes it
// holds (Omarchy, Palette, Batch, ModelStore); this object turns their state
// into properties and the user's actions into calls.
class Backend : public QObject {
    Q_OBJECT
    // The theme.
    Q_PROPERTY(QString themeName READ themeName NOTIFY paletteChanged)
    Q_PROPERTY(QVariantList swatches READ swatches NOTIFY paletteChanged)
    Q_PROPERTY(QString paletteWords READ paletteWords NOTIFY paletteChanged)
    Q_PROPERTY(QString mood READ mood NOTIFY paletteChanged)
    // One line for what went wrong, or what just happened.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusChanged)
    // The engine and the GPU, checked off the UI thread at start.
    Q_PROPERTY(bool engineChecking READ engineChecking NOTIFY engineChanged)
    Q_PROPERTY(bool engineReady READ engineReady NOTIFY engineChanged)
    Q_PROPERTY(QString engineProblem READ engineProblem NOTIFY engineChanged)
    Q_PROPERTY(QString gpuText READ gpuText NOTIFY engineChanged)
    // The models, the chosen one, and what installing it takes.
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(QString modelId READ modelId NOTIFY modelsChanged)
    Q_PROPERTY(QString modelName READ modelName NOTIFY modelsChanged)
    Q_PROPERTY(QString publisher READ publisher NOTIFY modelsChanged)
    Q_PROPERTY(bool modelInstalled READ modelInstalled NOTIFY modelsChanged)
    Q_PROPERTY(QString licenceText READ licenceText NOTIFY modelsChanged)
    Q_PROPERTY(QString licenceUrl READ licenceUrl NOTIFY modelsChanged)
    Q_PROPERTY(QString downloadSize READ downloadSize NOTIFY modelsChanged)
    Q_PROPERTY(QString diskText READ diskText NOTIFY modelsChanged)
    Q_PROPERTY(bool diskTooFull READ diskTooFull NOTIFY modelsChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadChanged)
    Q_PROPERTY(double downloadFraction READ downloadFraction NOTIFY downloadChanged)
    Q_PROPERTY(QString downloadText READ downloadText NOTIFY downloadChanged)
    // The prompt: the subject, the style, and the full text it becomes.
    Q_PROPERTY(QString subject READ subject WRITE setSubject NOTIFY promptChanged)
    Q_PROPERTY(QStringList styleIds READ styleIds CONSTANT)
    Q_PROPERTY(int styleIndex READ styleIndex WRITE setStyleIndex NOTIFY promptChanged)
    Q_PROPERTY(QString fullPrompt READ fullPrompt WRITE setFullPrompt NOTIFY promptChanged)
    Q_PROPERTY(bool promptEdited READ promptEdited NOTIFY promptChanged)
    // The batch.
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canGenerate READ canGenerate NOTIFY stateChanged)
    Q_PROPERTY(bool canRerun READ canRerun NOTIFY stateChanged)
    Q_PROPERTY(QVariantList drafts READ drafts NOTIFY draftsChanged)
    Q_PROPERTY(QString estimate READ estimate NOTIFY estimateChanged)

public:
    static constexpr int draftCount = 4;

    explicit Backend(Omarchy *omarchy, const QString &engineProgram = SdCppEngine::defaultProgram(),
                     QObject *parent = nullptr);
    ~Backend() override;

    QString themeName() const { return m_themeName; }
    QVariantList swatches() const;
    QString paletteWords() const { return m_palette.isValid() ? m_palette.words.join(", ") : QString(); }
    QString mood() const { return m_palette.isValid() ? m_palette.mood : QString(); }
    QString status() const { return m_status; }
    bool statusIsError() const { return m_statusIsError; }
    const Palette &palette() const { return m_palette; }

    // Checking until the result is stored, which is a moment after the
    // worker thread ends.
    bool engineChecking() const { return !m_probed; }
    bool engineReady() const { return m_probed && m_probe.error.kind == EngineError::None; }
    QString engineProblem() const { return m_probed ? m_probe.error.message : QString(); }
    QString gpuText() const;

    QVariantList models() const;
    QString modelId() const { return m_modelId; }
    QString modelName() const;
    QString publisher() const;
    bool modelInstalled() const { return m_plan.files.isEmpty(); }
    QString licenceText() const;
    QString licenceUrl() const;
    QString downloadSize() const;
    QString diskText() const;
    bool diskTooFull() const { return !m_plan.fitsOnDisk(); }
    bool downloading() const { return m_store.isRunning(); }
    double downloadFraction() const { return m_downloadTotal > 0 ? double(m_downloadDone) / m_downloadTotal : 0; }
    QString downloadText() const;

    QString subject() const { return m_subject; }
    void setSubject(const QString &subject);
    QStringList styleIds() const { return Prompt::styleIds(); }
    int styleIndex() const;
    void setStyleIndex(int index);
    // The edited prompt when the user changed it, else the one built from
    // the subject, the style and the theme, which follows them live.
    QString fullPrompt() const { return m_promptEdited ? m_editedPrompt : builtPrompt(); }
    void setFullPrompt(const QString &prompt);
    bool promptEdited() const { return m_promptEdited; }

    bool running() const { return m_batch && m_batch->isRunning(); }
    bool busy() const { return running() || downloading(); }
    bool canGenerate() const;
    bool canRerun() const { return !m_lastPrompt.isEmpty() && canGenerateWith(m_lastPrompt); }
    // One map per draft: state ("waiting", "sampling", "sampled", "done"),
    // step, steps, preview and image (file URLs), id and seed.
    QVariantList drafts() const { return m_drafts; }
    QString estimate() const { return m_estimate; }

    // A duration for people: "45 s", "2 min 5 s", through the locale.
    static QString durationText(double seconds);

public slots:
    void reload();
    void generate();
    // The same prompt as the last batch, with new seeds.
    void rerun();
    void cancel();
    void selectModel(const QString &id);
    // The user saw the licence, the size and the disk space, and agreed.
    void startDownload();
    // Keeps the .part files; startDownload resumes.
    void pauseDownload();
    void resetPrompt();
    void recheckEngine();
    // Before quitting: stops a batch and pauses a download.
    void stopAll();

signals:
    void paletteChanged();
    void statusChanged();
    void engineChanged();
    void modelsChanged();
    void downloadChanged();
    void promptChanged();
    void stateChanged();
    void draftsChanged();
    void estimateChanged();

private:
    struct Probe {
        EngineProbe engine;
        QList<Gpu> gpus;
    };

    QString builtPrompt() const;
    bool canGenerateWith(const QString &prompt) const;
    void startBatch(const QString &fullPrompt, const QString &subject, Prompt::Style style);
    void setStatus(const QString &status, bool error = false);
    void refreshModel();
    void updateEstimate();
    void setDraft(int index, const QVariantMap &changes);
    const CatalogueModel *currentModel() const { return m_catalogue.model(m_modelId); }

    Omarchy *m_omarchy;
    QString m_engineProgram;
    Catalogue m_catalogue;
    QString m_catalogueError;
    Config m_config;
    QString m_modelsDirectory;

    QString m_themeName;
    Palette m_palette;
    QString m_status;
    bool m_statusIsError = false;

    QFutureWatcher<Probe> m_probing;
    bool m_probed = false;
    EngineProbe m_probe;
    Gpu m_gpu; // the engine's GPU, as last measured

    QString m_modelId;
    ModelStore m_store;
    ModelStore::Plan m_plan;
    std::unique_ptr<QLockFile> m_downloadLock;
    qint64 m_downloadDone = 0, m_downloadTotal = 0;

    QString m_subject;
    Prompt::Style m_style = Prompt::defaultStyle;
    bool m_promptEdited = false;
    QString m_editedPrompt;

    std::unique_ptr<Batch> m_batch;
    QString m_lastPrompt;
    QString m_lastSubject;
    Prompt::Style m_lastStyle = Prompt::defaultStyle;
    QVariantList m_drafts;
    QString m_estimate;
    QElapsedTimer m_batchClock;
    double m_batchEstimate = -1;
    double m_secondsPerStep = -1;
    int m_stepsDone = 0;
    QTimer m_estimateTimer;
};
