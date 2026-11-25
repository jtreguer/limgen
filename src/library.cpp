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
#include <algorithm>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Library", text); }

QString monthOf(const QString &id) { return id.left(7); }

// Copies `image` to `target` through a .part file beside it. rename()
// refuses an existing target, so nothing is ever overwritten.
bool copyInto(const QString &image, const QString &target) {
    const QString part = target + ".part";
    QFile::remove(part);
    if (!QFile::copy(image, part))
        return false;
    if (!QFile::rename(part, target)) {
        QFile::remove(part);
        return false;
    }
    return true;
}

bool writeSidecar(const QString &path, const QJsonObject &metadata) {
    QSaveFile sidecar(path);
    return sidecar.open(QIODevice::WriteOnly) && sidecar.write(QJsonDocument(metadata).toJson()) >= 0 &&
           sidecar.commit();
}

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
        bool taken = false;
        for (const char *suffix : {".png", ".jpg", ".json", ".png.part", ".jpg.part"})
            taken = taken || QFileInfo::exists(base + suffix);
        if (!taken)
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
        {"style", record.style.isEmpty() ? QJsonValue() : QJsonValue(record.style)},
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
        {"source", record.source.isEmpty() ? QJsonValue() : QJsonValue(record.source)},
        {"generic", record.generic.isEmpty() ? QJsonValue() : QJsonValue(record.generic)},
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
    if (!copyInto(image, entry.image))
        return fail(tr("Cannot copy %1 into the library").arg(image));
    if (!writeSidecar(entry.sidecar, entry.metadata)) {
        QFile::remove(entry.image);
        return fail(tr("Cannot write %1").arg(entry.sidecar));
    }
    return entry;
}

LibraryEntry Library::addFinal(const QString &image, QJsonObject metadata, const QDateTime &created,
                               QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return LibraryEntry{};
    };
    const QString suffix = QFileInfo(image).suffix().toLower() == "png" ? "png" : "jpg";
    const QString id = newId(created);
    const QString folder = m_directory + "/" + monthOf(id);
    if (!QDir().mkpath(folder))
        return fail(tr("Cannot create %1").arg(folder));
    metadata.insert("id", id);
    metadata.insert("kind", "final");
    metadata.insert("created", created.toUTC().toString(Qt::ISODate));
    LibraryEntry entry{id, folder + "/" + id + "." + suffix, folder + "/" + id + ".json", metadata};
    if (!copyInto(image, entry.image))
        return fail(tr("Cannot copy %1 into the library").arg(image));
    if (!writeSidecar(entry.sidecar, entry.metadata)) {
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
    if (!QFileInfo::exists(entry.image))
        entry.image = folder + "/" + id + ".jpg";
    QFile sidecar(entry.sidecar);
    if (!QFileInfo::exists(entry.image) || !sidecar.open(QIODevice::ReadOnly))
        return {};
    entry.metadata = QJsonDocument::fromJson(sidecar.readAll()).object();
    return entry.metadata.value("id").toString() == id ? entry : LibraryEntry{};
}

QList<LibraryEntry> Library::list() const {
    static const QRegularExpression month(QRegularExpression::anchoredPattern("\\d{4}-\\d{2}"));
    QList<LibraryEntry> entries;
    const QDir root(m_directory);
    for (const QString &folder : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!month.match(folder).hasMatch())
            continue;
        for (const QFileInfo &sidecar : QDir(root.filePath(folder)).entryInfoList({"*.json"}, QDir::Files)) {
            const LibraryEntry entry = find(sidecar.completeBaseName());
            if (entry.isValid())
                entries << entry;
        }
    }
    // Ids start with the date, so they sort by day; the time breaks ties.
    std::sort(entries.begin(), entries.end(), [](const LibraryEntry &a, const LibraryEntry &b) {
        const QString first = a.metadata.value("created").toString(), second = b.metadata.value("created").toString();
        return first != second ? first > second : a.id > b.id;
    });
    return entries;
}

bool Library::discard(const QString &id, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const LibraryEntry entry = find(id);
    if (!entry.isValid())
        return fail(tr("No draft %1 in the library").arg(id));
    if (entry.metadata.value("kind").toString() != "draft")
        return fail(tr("%1 is not a draft").arg(id));
    const QString bin = m_directory + "/.discarded";
    if (!QDir().mkpath(bin))
        return fail(tr("Cannot create %1").arg(bin));
    // Both files are renamed within the library, so nothing is copied.
    if (!QFile::rename(entry.image, bin + "/" + id + ".png"))
        return fail(tr("Cannot move %1").arg(entry.image));
    if (!QFile::rename(entry.sidecar, bin + "/" + id + ".json")) {
        QFile::rename(bin + "/" + id + ".png", entry.image);
        return fail(tr("Cannot move %1").arg(entry.sidecar));
    }
    // The modification time says when it was discarded, for purgeDiscarded;
    // the contents stay as they were.
    for (const char *suffix : {".png", ".json"}) {
        QFile file(bin + "/" + id + suffix);
        if (file.open(QIODevice::ReadWrite))
            file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    }
    return true;
}

bool Library::restore(const QString &id, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!isValidId(id))
        return fail(tr("No draft %1 in the library").arg(id));
    const QString bin = m_directory + "/.discarded", folder = m_directory + "/" + monthOf(id);
    if (!QFileInfo::exists(bin + "/" + id + ".png") || !QFileInfo::exists(bin + "/" + id + ".json"))
        return fail(tr("Draft %1 is no longer there to restore").arg(id));
    if (!QDir().mkpath(folder))
        return fail(tr("Cannot create %1").arg(folder));
    if (!QFile::rename(bin + "/" + id + ".png", folder + "/" + id + ".png"))
        return fail(tr("Cannot restore %1").arg(id));
    if (!QFile::rename(bin + "/" + id + ".json", folder + "/" + id + ".json")) {
        QFile::rename(folder + "/" + id + ".png", bin + "/" + id + ".png");
        return fail(tr("Cannot restore %1").arg(id));
    }
    return true;
}

void Library::purge(const QString &id) {
    if (!isValidId(id))
        return;
    const QString bin = m_directory + "/.discarded/" + id;
    QFile::remove(bin + ".png");
    QFile::remove(bin + ".json");
}

void Library::purgeDiscarded(const QDateTime &before) {
    const QDir bin(m_directory + "/.discarded");
    for (const QFileInfo &file : bin.entryInfoList(QDir::Files))
        if (file.lastModified() < before)
            QFile::remove(file.absoluteFilePath());
}
