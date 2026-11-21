#include "catalogue.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

const QStringList Catalogue::roles{"diffusion-model", "llm", "vae", "tae", "model"};

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Catalogue", text); }

// Folder and file names become paths under the models directory, so they
// must be single, plain path components.
bool isPlainName(const QString &name) {
    static const QRegularExpression valid(QRegularExpression::anchoredPattern("[A-Za-z0-9][A-Za-z0-9._-]*"));
    return valid.match(name).hasMatch() && !name.contains("..");
}

} // namespace

Catalogue Catalogue::load(const QByteArray &json, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return Catalogue{};
    };
    QJsonParseError parseError;
    const QJsonObject root = QJsonDocument::fromJson(json, &parseError).object();
    if (parseError.error != QJsonParseError::NoError)
        return fail(tr("The model catalogue is not valid JSON: %1").arg(parseError.errorString()));

    Catalogue catalogue;
    catalogue.m_engine = root.value("engine").toString();
    catalogue.m_upscaler = root.value("upscaler").toString();

    const QJsonObject files = root.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        CatalogueFile file{it.key(),
                           o.value("folder").toString(),
                           o.value("name").toString(),
                           o.value("bytes").toInteger(),
                           o.value("sha256").toString(),
                           o.value("url").toString(),
                           o.value("licence").toString(),
                           o.value("source").toString()};
        if (!isPlainName(file.folder) || !isPlainName(file.name) || file.bytes <= 0 ||
            file.sha256.size() != 64 || !file.url.startsWith("https://") || file.licence.isEmpty())
            return fail(tr("Catalogue file %1 is incomplete").arg(it.key()));
        catalogue.m_files.insert(file.key, file);
    }

    int defaults = 0;
    for (const QJsonValue &value : root.value("models").toArray()) {
        const QJsonObject o = value.toObject();
        CatalogueModel model;
        model.id = o.value("id").toString();
        model.name = o.value("name").toString();
        model.publisher = o.value("publisher").toString();
        model.isDefault = o.value("default").toBool();
        const QJsonObject licence = o.value("licence").toObject();
        model.licenceId = licence.value("id").toString();
        model.licenceSummary = licence.value("summary").toString();
        model.licenceUrl = licence.value("url").toString();
        const QJsonArray native = o.value("native").toArray();
        model.width = native.at(0).toInt();
        model.height = native.at(1).toInt();
        model.steps = o.value("steps").toInt();
        model.cfg = o.value("cfg").toDouble(1.0);
        model.sampler = o.value("sampler").toString();
        if (model.id.isEmpty() || catalogue.model(model.id) || model.licenceId.isEmpty() || model.width <= 0 ||
            model.height <= 0 || model.steps <= 0)
            return fail(tr("Catalogue model %1 is incomplete or listed twice").arg(model.id));

        for (const QJsonValue &levelValue : o.value("levels").toArray()) {
            const QJsonObject l = levelValue.toObject();
            ModelLevel level{l.value("id").toString(), l.value("minFreeMiB").toInt(), {}};
            const QJsonObject levelFiles = l.value("files").toObject();
            for (auto it = levelFiles.begin(); it != levelFiles.end(); ++it) {
                if (!roles.contains(it.key()) || !catalogue.m_files.contains(it.value().toString()))
                    return fail(tr("Catalogue model %1 names an unknown role or file: %2")
                                    .arg(model.id, it.key()));
                level.files.insert(it.key(), it.value().toString());
            }
            // A model is either one file (-m) or a diffusion model with its
            // text encoder and VAE beside it.
            const bool whole = level.files.contains("model");
            const bool split = level.files.contains("diffusion-model") && level.files.contains("vae");
            if (level.id.isEmpty() || whole == split || level.minFreeMiB <= 0)
                return fail(tr("Catalogue model %1 has an incomplete level %2").arg(model.id, level.id));
            model.levels << level;
        }
        if (model.levels.isEmpty())
            return fail(tr("Catalogue model %1 has no level").arg(model.id));
        defaults += model.isDefault;
        catalogue.m_models << model;
    }
    if (catalogue.m_models.isEmpty() || defaults != 1)
        return fail(tr("The model catalogue must list models and exactly one default"));
    if (!catalogue.m_upscaler.isEmpty() && !catalogue.m_files.contains(catalogue.m_upscaler))
        return fail(tr("The model catalogue names an unknown upscaler"));
    return catalogue;
}

Catalogue Catalogue::builtIn(QString *error) {
    QFile file(":/models.json");
    QString reason = tr("it is missing from the program");
    Catalogue catalogue = file.open(QIODevice::ReadOnly) ? load(file.readAll(), &reason) : Catalogue{};
    // The catalogue is compiled in and covered by tests, so a broken one is a
    // build mistake, and the message says so.
    if (!catalogue.isValid() && error)
        *error = tr("The built-in model catalogue is broken, which is a bug in limgen: %1").arg(reason);
    return catalogue;
}

Catalogue Catalogue::fromEnvironment(QString *error) {
    const QString path = qEnvironmentVariable("LIMGEN_CATALOGUE");
    if (path.isEmpty())
        return builtIn(error);
    QFile file(path);
    QString reason = file.errorString();
    Catalogue catalogue = file.open(QIODevice::ReadOnly) ? load(file.readAll(), &reason) : Catalogue{};
    if (!catalogue.isValid() && error)
        *error = tr("LIMGEN_CATALOGUE names %1, which cannot be used: %2").arg(path, file.isOpen() ? reason : file.errorString());
    return catalogue;
}

QString Catalogue::defaultModelsDirectory() {
    const QString overridden = qEnvironmentVariable("LIMGEN_MODELS");
    if (!overridden.isEmpty())
        return overridden;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/limgen/models";
}

const CatalogueModel *Catalogue::model(const QString &id) const {
    for (const auto &model : m_models)
        if (model.id == id)
            return &model;
    return nullptr;
}

const CatalogueModel *Catalogue::defaultModel() const {
    for (const auto &model : m_models)
        if (model.isDefault)
            return &model;
    return nullptr;
}

const CatalogueFile *Catalogue::file(const QString &key) const {
    const auto it = m_files.constFind(key);
    return it == m_files.constEnd() ? nullptr : &it.value();
}

QString Catalogue::path(const CatalogueFile &file, const QString &modelsDirectory) {
    return modelsDirectory + "/" + file.folder + "/" + file.name;
}

Catalogue::FileState Catalogue::state(const CatalogueFile &file, const QString &modelsDirectory, bool verify) {
    const QFileInfo info(path(file, modelsDirectory));
    if (!info.isFile())
        return FileState::Missing;
    if (info.size() != file.bytes)
        return FileState::WrongSize;
    if (!verify)
        return FileState::Ok;
    QFile data(info.filePath());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!data.open(QIODevice::ReadOnly) || !hash.addData(&data))
        return FileState::Corrupt;
    return hash.result().toHex() == file.sha256.toLatin1() ? FileState::Ok : FileState::Corrupt;
}

QStringList Catalogue::missingFiles(const ModelLevel &level, const QString &modelsDirectory, bool verify,
                                    const QStringList &skip) const {
    QStringList missing;
    for (auto it = level.files.begin(); it != level.files.end(); ++it) {
        if (skip.contains(it.key()))
            continue;
        const QString &key = it.value();
        const CatalogueFile *f = file(key);
        if (!f || state(*f, modelsDirectory, verify) != FileState::Ok)
            missing << key;
    }
    missing.removeDuplicates();
    return missing;
}
