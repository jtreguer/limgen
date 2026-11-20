#include "library.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Library", text); }

QString monthOf(const QString &id) { return id.left(7); }

} // namespace

Library::Library(const QString &directory) : m_directory(directory) {}

QString Library::defaultDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/limgen/library";
}

bool Library::isValidId(const QString &id) {
    static const QRegularExpression valid(QRegularExpression::anchoredPattern("\\d{4}-\\d{2}-\\d{2}-[0-9a-f]{6}"));
    return valid.match(id).hasMatch();
}

QString Library::newId(const QDateTime &created) const {
    const QString date = created.toUTC().toString("yyyy-MM-dd");
    for (;;) {
        const QString id =
            QString("%1-%2").arg(date).arg(QRandomGenerator::global()->bounded(0x1000000), 6, 16, QChar('0'));
        const QString base = m_directory + "/" + monthOf(id) + "/" + id;
        if (!QFileInfo::exists(base + ".png") && !QFileInfo::exists(base + ".json") &&
            !QFileInfo::exists(base + ".png.part"))
            return id;
    }
}

QJsonObject Library::metadata(const QString &id, const QDateTime &created, const DraftRecord &record) {
    QJsonValue theme;
    if (!record.themeSlug.isEmpty())
        theme = QJsonObject{{"slug", record.themeSlug}, {"mode", record.themeMode}, {"palette", record.palette}};
    return {
        {"id", id},
        {"kind", "draft"},
        {"created", created.toUTC().toString(Qt::ISODate)},
        {"theme", theme},
        {"subject", record.subject},
        {"prompt", record.prompt},
        {"negative", record.negative},
        {"style", record.style},
        {"model", QJsonObject{{"id", record.modelId},
                              {"level", record.level},
                              {"files_sha256", QJsonArray::fromStringList(record.filesSha256)}}},
        {"engine", QJsonObject{{"name", record.engineName},
                               {"version", record.engineVersion},
                               {"backend", record.engineBackend}}},
        {"seed", double(record.seed)},
        {"steps", record.steps},
        {"cfg", record.cfg},
        {"sampler", record.sampler},
        {"size", QJsonArray{record.width, record.height}},
        {"source", QJsonValue()},
        {"finish", QJsonValue()},
    };
}

LibraryEntry Library::addDraft(const QString &image, const DraftRecord &record, const QDateTime &created,
                               QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return LibraryEntry{};
    };
    const QString id = newId(created);
    const QString folder = m_directory + "/" + monthOf(id);
    if (!QDir().mkpath(folder))
        return fail(tr("Cannot create %1").arg(folder));

    LibraryEntry entry{id, folder + "/" + id + ".png", folder + "/" + id + ".json", metadata(id, created, record)};
    const QString part = entry.image + ".part";
    QFile::remove(part);
    if (!QFile::copy(image, part))
        return fail(tr("Cannot copy %1 into the library").arg(image));
    // rename() refuses an existing target, so an entry is never overwritten.
    if (!QFile::rename(part, entry.image)) {
        QFile::remove(part);
        return fail(tr("Cannot write %1").arg(entry.image));
    }

    QSaveFile sidecar(entry.sidecar);
    if (!sidecar.open(QIODevice::WriteOnly) ||
        sidecar.write(QJsonDocument(entry.metadata).toJson()) < 0 || !sidecar.commit()) {
        QFile::remove(entry.image);
        return fail(tr("Cannot write %1").arg(entry.sidecar));
    }
    return entry;
}

LibraryEntry Library::find(const QString &id) const {
    if (!isValidId(id))
        return {};
    const QString folder = m_directory + "/" + monthOf(id);
    LibraryEntry entry{id, folder + "/" + id + ".png", folder + "/" + id + ".json", {}};
    QFile sidecar(entry.sidecar);
    if (!QFileInfo::exists(entry.image) || !sidecar.open(QIODevice::ReadOnly))
        return {};
    entry.metadata = QJsonDocument::fromJson(sidecar.readAll()).object();
    return entry.metadata.value("id").toString() == id ? entry : LibraryEntry{};
}
