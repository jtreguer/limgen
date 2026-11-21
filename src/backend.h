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
#include "keeper.h"
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
    // Keeping: kept drafts are finished one at a time, in the background; a
    // batch asked for meanwhile waits for them, since both need the GPU.
    Q_PROPERTY(bool keeping READ keeping NOTIFY stateChanged)
    Q_PROPERTY(bool batchWaiting READ batchWaiting NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    // How kept drafts are finished, saved in config.json.
    Q_PROPERTY(double colorPass READ colorPass WRITE setColorPass NOTIFY settingsChanged)
    Q_PROPERTY(int formatIndex READ formatIndex WRITE setFormatIndex NOTIFY settingsChanged)

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
    // Work that quitting would stop: a batch (or one waiting), a keep, a
    // download.
    bool busy() const { return running() || m_batchWaiting || keeping() || downloading(); }
    bool canGenerate() const;
    bool canRerun() const { return !m_lastPrompt.isEmpty() && canGenerateWith(m_lastPrompt); }
    // One map per draft: state ("waiting", "sampling", "sampled", "done"),
    // step, steps, preview and image (file URLs), id, seed, the theme it was
    // made for; and once done, whether it is being kept (keeping, with
    // keepFraction, -1 while it waits its turn), kept (with keptPath, and
    // set when it became the background) or discarded.
    QVariantList drafts() const { return m_drafts; }
    QString estimate() const { return m_estimate; }

    // A duration for people: "45 s", "2 min 5 s", through the locale.
    static QString durationText(double seconds);

    bool keeping() const { return m_keeper || !m_keepQueue.isEmpty(); }
    bool batchWaiting() const { return m_batchWaiting; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    double colorPass() const { return m_config.colorPass(); }
    void setColorPass(double strength);
    int formatIndex() const { return m_config.format() == "jpeg" ? 1 : 0; }
    void setFormatIndex(int index);

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
    // Before quitting: stops a batch and the keeps, and pauses a download.
    void stopAll();

    // K and S: finish the draft and add it to the theme's backgrounds; S
    // also shows it now. When the theme changed since the draft was made,
    // themeChoiceNeeded asks the window which theme to use, and the answer
    // comes back through keepInto.
    void keep(int index);
    void keepAndSet(int index);
    void keepInto(int index, const QString &theme, bool set);
    // D: moves the draft out of the library until undone, or for good when
    // the next batch starts or the window closes.
    void discard(int index);
    // Ctrl+Z and Ctrl+Shift+Z, for the keeps and discards of this batch.
    void undo();
    void redo();

signals:
    void paletteChanged();
    void statusChanged();
    void engineChanged();
    void modelsChanged();
    void downloadChanged();
    void promptChanged();
    void stateChanged();
    void settingsChanged();
    void themeChoiceNeeded(int index, const QString &madeFor, const QString &current, bool set);
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

    // One keep in the queue, or running.
    struct KeepJob {
        quint64 serial = 0;
        int index = -1;
        QString draftId, theme;
        bool set = false;
    };
    // A keep or a discard, for undo and redo.
    struct Action {
        enum Kind { Keep, Discard } kind = Keep;
        quint64 serial = 0; // the keep job it came from
        int index = -1;
        QString draftId, theme;
        bool set = false;
        bool done = false;          // the keep has finished
        bool installedByUs = false; // false when the draft was kept before
        QString installed, finalImage, previousBackground;
    };
    void requestKeep(int index, bool set);
    void enqueueKeep(const Action &action);
    void nextKeep();
    void keepFinished(const KeepJob &job, const KeepResult &result);
    void keepFailed(const KeepJob &job, const QString &message);
    Action *actionFor(quint64 serial);
    QVariantMap draftAt(int index, const QString &id) const;
    void setDraftFor(int index, const QString &id, const QVariantMap &changes);
    void setBackgroundFor(Action &action);
    void forgetActions();
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
    QString m_themeStatus; // what the last reload put in the status line

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
    QString m_batchTheme;
    // A batch asked for while keeps run starts when they are done.
    bool m_batchWaiting = false;
    QString m_waitingPrompt, m_waitingSubject;
    Prompt::Style m_waitingStyle = Prompt::defaultStyle;

    std::unique_ptr<Keeper> m_keeper;
    KeepJob m_keepJob;
    QList<KeepJob> m_keepQueue;
    quint64 m_nextSerial = 1;
    QList<Action> m_undo, m_redo;
    QVariantList m_drafts;
    QString m_estimate;
    QElapsedTimer m_batchClock;
    double m_batchEstimate = -1;
    double m_secondsPerStep = -1;
    int m_stepsDone = 0;
    QTimer m_estimateTimer;
};
