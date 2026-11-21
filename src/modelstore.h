#pragma once

#include <QFile>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QTimer>
#include <memory>

#include "catalogue.h"

class QNetworkAccessManager;
class QNetworkReply;

// Downloads catalogue files into the models directory: each to a .part file
// beside its target, resumed with HTTP range requests after an interruption
// or a restart, checked for size and SHA-256, and only then renamed into
// place. It never decides to download on its own; its caller starts it
// after the user confirmed (docs/design.md section 5).
class ModelStore : public QObject {
    Q_OBJECT

public:
    // What installing a level would take.
    struct Plan {
        QList<CatalogueFile> files; // not installed yet, in download order
        qint64 totalBytes = 0;      // their full size
        qint64 partBytes = 0;       // already downloaded into .part files
        qint64 availableBytes = -1; // free space where they go, -1 when unknown
        qint64 downloadBytes() const { return totalBytes - partBytes; }
        // Enough room for what is left, plus some headroom.
        bool fitsOnDisk() const;
    };

    static constexpr qint64 headroomBytes = 256ll * 1024 * 1024;

    // `network` is for tests, which point downloads at a local server; by
    // default the store makes its own when a download starts.
    explicit ModelStore(const QString &modelsDirectory, QNetworkAccessManager *network = nullptr,
                        QObject *parent = nullptr);
    ~ModelStore() override;

    QString modelsDirectory() const { return m_directory; }

    // The level's files that are not installed at the right size, and the
    // upscaler when `withUpscaler` and it is missing; a file shared with an
    // installed model is not downloaded twice.
    Plan plan(const Catalogue &catalogue, const ModelLevel &level, bool withUpscaler = true) const;
    // Free space for the nearest existing folder of `directory`.
    static qint64 availableBytes(const QString &directory);

    void start(const QList<CatalogueFile> &files);
    // Stops the download and keeps the .part file, so the next start
    // resumes where this one stopped.
    void cancel();
    bool isRunning() const { return m_running; }

    // How many times a failed request is retried before giving up.
    static constexpr int retries = 3;

signals:
    // Bytes on disk for the whole list so far, out of its total.
    void progress(qint64 done, qint64 total, const QString &file);
    void fileInstalled(const QString &key);
    void finished();
    // `message` is for the user; .part files are kept so a later start resumes.
    void failed(const QString &message);
    void cancelled();

private:
    void next();
    void request();
    void readyRead();
    void replyFinished();
    void verify();
    void verified();
    void stop(const QString &message);
    qint64 doneBytes() const;

    QString m_directory;
    QNetworkAccessManager *m_network;
    std::unique_ptr<QNetworkAccessManager> m_ownNetwork;
    QList<CatalogueFile> m_files;
    int m_index = -1;
    qint64 m_totalBytes = 0;
    qint64 m_installedBytes = 0; // files of this list already finished
    QFile m_part;
    QNetworkReply *m_reply = nullptr;
    bool m_rangeChecked = false;
    // From verify() until verified() runs; the watcher alone says "not
    // running" while its finished signal is still queued.
    bool m_hashing = false;
    int m_attempt = 0;
    bool m_running = false;
    bool m_cancelling = false;
    QTimer m_retryTimer;
    QFutureWatcher<QByteArray> m_hash;
};
