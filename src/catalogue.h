#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// One downloadable file: a model's weights, a text encoder, a VAE, a preview
// decoder or the upscaler. A file several models use is listed once.
struct CatalogueFile {
    QString key;     // "qwen3-4b-q4_k_m"
    QString folder;  // where it lives under the models directory
    QString name;    // file name in that folder
    qint64 bytes = 0;
    QString sha256;
    QString url;     // pinned to the publisher's revision
    QString licence; // SPDX id
    QString source;  // the publisher's page
};

// One quantisation level of a model. files maps an sd-cli role
// ("diffusion-model", "llm", "vae", "tae", "model") to a file key.
struct ModelLevel {
    QString id;
    int minFreeMiB = 0; // the least free video memory it was measured to run with
    QMap<QString, QString> files;
};

struct CatalogueModel {
    QString id, name, publisher;
    bool isDefault = false;
    QString licenceId, licenceSummary, licenceUrl;
    int width = 0, height = 0; // native draft size
    int steps = 0;
    double cfg = 1.0;
    QString sampler;
    QList<ModelLevel> levels;
};

// The models limgen can run, compiled in from src/models.json. Parsing is
// separate from the disk: load() reads JSON it is given, and only the
// installed-state helpers look at files.
class Catalogue {
public:
    enum class FileState { Missing, WrongSize, Corrupt, Ok };

    // Every role a level may name; each maps to the sd-cli option of the
    // same name (SdCppEngine::arguments).
    static const QStringList roles;

    static Catalogue load(const QByteArray &json, QString *error);
    static Catalogue builtIn();
    // LIMGEN_MODELS, or $XDG_DATA_HOME/limgen/models.
    static QString defaultModelsDirectory();

    bool isValid() const { return !m_models.isEmpty(); }
    QString engine() const { return m_engine; }
    const QList<CatalogueModel> &models() const { return m_models; }
    const CatalogueModel *model(const QString &id) const;
    const CatalogueModel *defaultModel() const;
    const CatalogueFile *file(const QString &key) const;
    const CatalogueFile *upscaler() const { return file(m_upscaler); }

    static QString path(const CatalogueFile &file, const QString &modelsDirectory);
    // Size only unless verify is set, which also checks the SHA-256 (slow:
    // several gigabytes are read).
    static FileState state(const CatalogueFile &file, const QString &modelsDirectory, bool verify = false);
    // Files of a level that are not installed (Missing, WrongSize, or
    // Corrupt when verifying), as keys. Roles in `skip` are not checked: a
    // command draws no previews and needs no "tae" file.
    QStringList missingFiles(const ModelLevel &level, const QString &modelsDirectory, bool verify = false,
                             const QStringList &skip = {}) const;

private:
    QString m_engine, m_upscaler;
    QList<CatalogueModel> m_models;
    QHash<QString, CatalogueFile> m_files;
};
