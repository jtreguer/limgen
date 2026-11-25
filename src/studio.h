#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

#include "batch.h"
#include "keeper.h"

class Backend;

// The Generic screen's state for QML: images of anything, at any of a few
// sizes, with an optional palette, not tied to the current theme. A batch
// of low-resolution previews comes first; the one the user picks is
// upscaled to the chosen size, and saved where they choose. The engine
// check, the models, the downloads and the settings file are the Backend's,
// shared with the theme screen; so is the GPU, which only one of the two
// screens uses at a time.
class Studio : public QObject {
    Q_OBJECT
    // The target sizes: [{kind, width, height}], kind "monitor" (the
    // largest monitor's, or 2560x1440 when none was measured: "fallback"),
    // "preset", "portrait", "square" or "wide", and "custom" last. The
    // window words the labels, so they follow a language switch.
    Q_PROPERTY(QVariantList sizes READ sizes NOTIFY sizesChanged)
    Q_PROPERTY(int sizeIndex READ sizeIndex WRITE setSizeIndex NOTIFY settingsChanged)
    Q_PROPERTY(bool customSize READ customSize NOTIFY settingsChanged)
    Q_PROPERTY(int customWidth READ customWidth WRITE setCustomWidth NOTIFY settingsChanged)
    Q_PROPERTY(int customHeight READ customHeight WRITE setCustomHeight NOTIFY settingsChanged)
    // Why the chosen size cannot be made; empty when it can.
    Q_PROPERTY(QString sizeProblem READ sizeProblem NOTIFY settingsChanged)
    Q_PROPERTY(int targetWidth READ targetWidth NOTIFY settingsChanged)
    Q_PROPERTY(int targetHeight READ targetHeight NOTIFY settingsChanged)
    // The previews' size for the chosen target and model.
    Q_PROPERTY(int draftWidth READ draftWidth NOTIFY settingsChanged)
    Q_PROPERTY(int draftHeight READ draftHeight NOTIFY settingsChanged)
    // The size the previews on screen were made for, which an upscale
    // makes, whatever the Resolution box says since; 0x0 without previews.
    Q_PROPERTY(int batchWidth READ batchWidth NOTIFY draftsChanged)
    Q_PROPERTY(int batchHeight READ batchHeight NOTIFY draftsChanged)
    // How many previews: "2x1", "2x2", "2x3", "3x3" (2, 4, 6, 9).
    Q_PROPERTY(QStringList gridNames READ gridNames CONSTANT)
    Q_PROPERTY(int gridIndex READ gridIndex WRITE setGridIndex NOTIFY settingsChanged)
    Q_PROPERTY(int count READ count NOTIFY settingsChanged)
    // How much the previews differ beyond their seeds, in the order of
    // Prompt::varietyIds(): seeds only, framing, framing and light.
    Q_PROPERTY(int varietyIndex READ varietyIndex WRITE setVarietyIndex NOTIFY settingsChanged)
    // The palettes: [{id, swatches}], with id "" for none, a theme's slug,
    // or "custom" for the colours chosen by hand; swatches are "#rrggbb".
    Q_PROPERTY(QVariantList palettes READ palettes NOTIFY palettesChanged)
    Q_PROPERTY(int paletteIndex READ paletteIndex WRITE setPaletteIndex NOTIFY settingsChanged)
    Q_PROPERTY(bool customPalette READ customPalette NOTIFY settingsChanged)
    // The colours chosen by hand: [{hex, words}].
    Q_PROPERTY(QVariantList customColors READ customColors NOTIFY settingsChanged)
    Q_PROPERTY(int maxColors READ maxColors CONSTANT)
    // The words the chosen palette adds to the prompt; empty for none.
    Q_PROPERTY(QString paletteWords READ paletteWords NOTIFY settingsChanged)
    Q_PROPERTY(QString prompt READ prompt WRITE setPrompt NOTIFY promptChanged)
    // What the model is given: the prompt and the palette's words.
    Q_PROPERTY(QString fullPrompt READ fullPrompt NOTIFY promptChanged)
    // The batch of previews.
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canGenerate READ canGenerate NOTIFY stateChanged)
    Q_PROPERTY(QVariantList drafts READ drafts NOTIFY draftsChanged)
    Q_PROPERTY(QString estimate READ estimate NOTIFY estimateChanged)
    // The upscale of the chosen preview, and its result: {id, image (a file
    // URL), path, width, height, from (the preview's index), seed}; empty
    // until one is made.
    Q_PROPERTY(bool upscaling READ upscaling NOTIFY stateChanged)
    Q_PROPERTY(double upscaleFraction READ upscaleFraction NOTIFY upscaleChanged)
    Q_PROPERTY(int upscalingIndex READ upscalingIndex NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap result READ result NOTIFY resultChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY stateChanged)
    // Where the save dialog opens, and the name it suggests.
    Q_PROPERTY(QUrl saveFolder READ saveFolder NOTIFY resultChanged)
    Q_PROPERTY(QString suggestedName READ suggestedName NOTIFY resultChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusChanged)

public:
    static constexpr int minSide = 256;
    static constexpr int maxSide = 16384;

    explicit Studio(Backend *backend, QObject *parent = nullptr);
    ~Studio() override;

    QVariantList sizes() const { return m_sizes; }
    int sizeIndex() const { return m_sizeIndex; }
    void setSizeIndex(int index);
    bool customSize() const { return m_sizeIndex == m_sizes.size() - 1; }
    int customWidth() const { return m_custom.width(); }
    void setCustomWidth(int width);
    int customHeight() const { return m_custom.height(); }
    void setCustomHeight(int height);
    QSize target() const;
    int targetWidth() const { return target().width(); }
    int targetHeight() const { return target().height(); }
    QSize draftSize() const;
    int batchWidth() const { return m_drafts.isEmpty() ? 0 : m_batchTarget.width(); }
    int batchHeight() const { return m_drafts.isEmpty() ? 0 : m_batchTarget.height(); }
    int draftWidth() const { return draftSize().width(); }
    int draftHeight() const { return draftSize().height(); }
    QString sizeProblem() const;
    // Why `target` cannot be made from previews of `draft`: a side under
    // 256 pixels, a shape wider or taller than 4:1, or more than four times
    // the preview's size (the upscaler's factor). Empty when it can.
    static QString sizeProblemFor(const QSize &target, const QSize &draft);

    QStringList gridNames() const { return {"2x1", "2x2", "2x3", "3x3"}; }
    int gridIndex() const;
    void setGridIndex(int index);
    int count() const { return m_count; }
    static QList<int> counts() { return {2, 4, 6, 9}; }
    int varietyIndex() const { return int(m_variety); }
    void setVarietyIndex(int index);

    QVariantList palettes() const { return m_palettes; }
    int paletteIndex() const;
    void setPaletteIndex(int index);
    bool customPalette() const { return m_palette == "custom"; }
    QVariantList customColors() const;
    int maxColors() const { return Palette::maxColors; }
    // The chosen palette, as the batch reads it; invalid for none.
    Palette chosenPalette() const;
    QString paletteWords() const;

    QString prompt() const { return m_prompt; }
    void setPrompt(const QString &prompt);
    QString fullPrompt() const;

    bool running() const { return m_batch && m_batch->isRunning(); }
    bool upscaling() const { return m_keeper != nullptr; }
    bool saving() const { return m_saving.isRunning(); }
    bool busy() const { return running() || upscaling() || saving(); }
    bool canGenerate() const;
    QVariantList drafts() const { return m_drafts; }
    QString estimate() const { return m_estimate; }
    double upscaleFraction() const { return m_upscaleFraction; }
    int upscalingIndex() const { return m_upscalingIndex; }
    QVariantMap result() const { return m_result; }
    QUrl saveFolder() const;
    QString suggestedName() const;
    QString status() const { return m_status; }
    bool statusIsError() const { return m_statusIsError; }

public slots:
    void generate();
    // Stops the batch of previews, or the upscale.
    void cancel();
    // Before quitting: cancel(), and the save is waited for.
    void stop();
    // Upscales preview `index` to the target size; the result replaces any
    // earlier one.
    void upscale(int index);
    // Writes the result to `file`: PNG, or JPEG for a .jpg or .jpeg name.
    // A name with neither gets the suffix of `jpeg` (the dialog's filter),
    // and is refused if a file has that name: the dialog asked about
    // replacing the name typed, not that one.
    void saveImage(const QUrl &file, bool jpeg = false);
    // The colours chosen by hand: "#rrggbb", or "#aarrggbb" from a colour
    // dialog, whose alpha is dropped.
    void addColor(const QString &hex);
    void setColor(int index, const QString &hex);
    void removeColor(int index);
    // Reads the sizes (the monitor) and the themes' palettes again.
    void reload();

signals:
    void sizesChanged();
    void settingsChanged();
    void palettesChanged();
    void promptChanged();
    void stateChanged();
    void draftsChanged();
    void estimateChanged();
    void upscaleChanged();
    void resultChanged();
    void statusChanged();

private:
    struct Saved {
        QString file;
        QString error;
    };

    void reloadSizes();
    void reloadPalettes();
    void coloursChanged();
    void setStatus(const QString &status, bool error = false);
    void setDraft(int index, const QVariantMap &changes);
    void updateEstimate();
    void saveSettings();
    void holdGpu();
    static QString normalised(const QString &hex);

    Backend *m_backend;
    QVariantList m_sizes;
    int m_sizeIndex = 0;
    QSize m_custom{1920, 1080};
    int m_count = 4;
    Prompt::Variety m_variety = Prompt::Variety::Seeds;
    QVariantList m_palettes;
    QHash<QString, Palette> m_themePalettes;
    QString m_palette; // "" for none, "custom", or a theme's slug
    QStringList m_colors;
    QString m_prompt;

    std::unique_ptr<Batch> m_batch;
    QVariantList m_drafts;
    QSize m_batchTarget; // what the drafts on screen are upscaled to
    QString m_batchPrompt;
    QString m_estimate;
    QElapsedTimer m_batchClock;
    double m_batchEstimate = -1;
    double m_secondsPerStep = -1;
    int m_stepsDone = 0;
    QTimer m_estimateTimer;

    std::unique_ptr<Keeper> m_keeper;
    int m_upscalingIndex = -1;
    double m_upscaleFraction = 0;
    QVariantMap m_result;
    QFutureWatcher<Saved> m_saving;

    QString m_status;
    bool m_statusIsError = false;
};
