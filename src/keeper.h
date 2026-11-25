#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QSize>
#include <memory>

#include "catalogue.h"
#include "library.h"
#include "omarchy.h"
#include "sdcppengine.h"

struct KeepSettings {
    QString draftId;
    // The theme whose background folder gets the image; empty for the
    // pictures folder (no Omarchy theme). The caller decides: the window
    // asks when the theme changed since the draft was made.
    QString theme;
    double colorPass = 0.3; // 0 turns the colour pass off
    QString format = "png"; // or "jpeg"
    int jpegQuality = 92;
    QSize size; // the final size; empty for a generic draft's target, else the largest monitor's
    // False for a generic image: the final is filed in the library and goes
    // nowhere else, with no colour pass; the user saves it where they like.
    bool install = true;
};

struct KeepResult {
    LibraryEntry draft;
    LibraryEntry final;     // invalid when the draft was already kept there
    QString installed;      // the image in the theme's background folder; empty when not installed
    QString theme;          // the theme it went to, empty for the pictures folder
    bool alreadyKept = false;
};
Q_DECLARE_METATYPE(KeepResult)

// Finishes a kept draft (docs/technical.md section 5, steps 4 to 6): an
// ESRGAN upscale through the engine, a Lanczos cover to the final size, the
// optional colour pass toward the theme, then PNG or JPEG with the prompt in
// the file. The final is filed in the library and a copy goes into the
// theme's background folder, written beside its name and renamed into
// place; nothing there is ever overwritten. Undo is the caller's: it knows
// the installed file from the result.
class Keeper : public QObject {
    Q_OBJECT

public:
    enum class Outcome { Ok, UnknownDraft, UnknownTheme, NotRunnable, Failed, Cancelled };
    Q_ENUM(Outcome)

    Keeper(const Catalogue &catalogue, const OmarchyPaths &paths,
           const QString &engineProgram = SdCppEngine::defaultProgram(), QObject *parent = nullptr);
    ~Keeper() override;

    // Checks, in this order: the draft is in the library, the theme exists
    // (the current one or a theme folder), the upscaler is installed, the
    // engine probe found a usable engine. Works out the final size and the
    // file name, and whether the draft is already in that folder; then
    // start() reports it at once instead of making it again.
    Outcome prepare(const KeepSettings &settings, const EngineProbe &probe, QString *message);
    void start();
    void cancel();
    bool isRunning() const { return m_running; }
    // Filled by prepare(): the draft, the theme, and where the image goes.
    const KeepResult &result() const { return m_result; }
    QSize finalSize() const { return m_size; }

    // "limgen-oil-rig-at-dusk-a3f9c1.png": the subject as ASCII words (or
    // "background"), at most 40 characters, then the draft id's last part.
    static QString fileName(const QString &subject, const QString &draftId, const QString &format);
    // The A1111-style text other image tools read from a PNG or JPEG.
    static QString parameters(const QJsonObject &metadata);
    // Writes a copy of a final (a PNG) to `path`: the same bytes for a .png,
    // encoded as JPEG at `jpegQuality` with `parameters` in it otherwise.
    // Written beside `path` and renamed into place, replacing a file there
    // (the user chose it). Returns what went wrong, or an empty string. For
    // the Generic screen's Save and limgen upscale --output.
    static QString saveCopy(const QString &final, const QString &path, const QString &parameters, int jpegQuality);

signals:
    // From 0 to 1: mostly the upscale's tiles, then the saving.
    void progress(double fraction);
    void finished(const KeepResult &result);
    void failed(Keeper::Outcome outcome, const QString &message);

private:
    struct Finished {
        QString file;  // the encoded final, in the work folder
        QString error; // why it could not be made
        QString colorTarget; // "backgrounds", "palette" or empty
    };

    void engineFinished(const QStringList &files);
    void engineFailed(const EngineError &error);
    void saved();
    void fail(Outcome outcome, const QString &message);
    void cleanUp();

    Catalogue m_catalogue;
    OmarchyPaths m_paths;
    std::unique_ptr<SdCppEngine> m_engine;
    KeepSettings m_settings;
    KeepResult m_result;
    EngineRequest m_request;
    QSize m_size;
    QStringList m_backgrounds;  // the theme's own, for the colour pass
    QList<QColor> m_palette;    // when it has none
    QFutureWatcher<Finished> m_saving;
    bool m_running = false;
    bool m_cancelled = false;
};
