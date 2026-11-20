#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// What a draft was made from, for its sidecar (docs/technical.md section 8).
struct DraftRecord {
    QString themeSlug; // empty without an Omarchy theme
    QString themeMode;
    QJsonObject palette; // role to hex: {"accent": "#7aa2f7", "background": ...}
    QString subject, prompt, negative, style;
    QString modelId, level;
    QStringList filesSha256;
    QString engineName = "sd-cli", engineVersion, engineBackend;
    quint32 seed = 0;
    int steps = 0;
    double cfg = 1.0;
    QString sampler;
    int width = 0, height = 0;
};

struct LibraryEntry {
    QString id;
    QString image;   // the PNG
    QString sidecar; // the JSON beside it
    QJsonObject metadata;
    bool isValid() const { return !id.isEmpty(); }
};

// Every draft limgen made, with its metadata, in limgen's own data folder:
// <library>/<yyyy-mm>/<id>.png and <id>.json. Drafts are never modified once
// written, and nothing here touches Omarchy's folders.
class Library {
public:
    explicit Library(const QString &directory = defaultDirectory());

    // $XDG_DATA_HOME/limgen/library.
    static QString defaultDirectory();
    QString directory() const { return m_directory; }

    // "2026-11-20-a3f9c1": the UTC date, then six random hex digits.
    static bool isValidId(const QString &id);
    QString newId(const QDateTime &created) const;

    // Copies `image` into the library with a sidecar. The copy is written
    // beside its target and renamed into place, and an existing entry is
    // never overwritten. Returns an invalid entry and sets *error on failure.
    LibraryEntry addDraft(const QString &image, const DraftRecord &record,
                          const QDateTime &created = QDateTime::currentDateTimeUtc(), QString *error = nullptr);
    LibraryEntry find(const QString &id) const;

    static QJsonObject metadata(const QString &id, const QDateTime &created, const DraftRecord &record);

private:
    QString m_directory;
};
