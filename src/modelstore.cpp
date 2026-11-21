#include "modelstore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <cstdio>

namespace {

// The hex SHA-256 of a file, or an empty array when it cannot be read.
QByteArray sha256Of(const QString &path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
        return {};
    return hash.result().toHex();
}

// LIMGEN_DOWNLOAD_BASE sends every download to another server, keeping the
// path: tests point it at a local server, or at a port where nothing
// listens so that no test can reach the internet. The SHA-256 check still
// decides what is installed.
QUrl downloadUrl(const QString &url) {
    QUrl result(url);
    const QUrl base(qEnvironmentVariable("LIMGEN_DOWNLOAD_BASE"));
    if (base.isValid() && !base.host().isEmpty()) {
        result.setScheme(base.scheme());
        result.setHost(base.host());
        result.setPort(base.port());
    }
    return result;
}

QString partPath(const CatalogueFile &file, const QString &directory) {
    return Catalogue::path(file, directory) + ".part";
}

// Errors that another try will not fix: the file is not there, or not ours
// to fetch (a gated file asks for a login, which limgen never gives).
bool isFinal(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::ContentNotFoundError:
    case QNetworkReply::ContentAccessDenied:
    case QNetworkReply::ContentGoneError:
    case QNetworkReply::AuthenticationRequiredError:
    case QNetworkReply::ProtocolUnknownError:
    case QNetworkReply::ProtocolInvalidOperationError:
    // A redirect to plain http, or one that never ends, will not change.
    case QNetworkReply::InsecureRedirectError:
    case QNetworkReply::TooManyRedirectsError:
        return true;
    default:
        return false;
    }
}

} // namespace

bool ModelStore::Plan::fitsOnDisk() const {
    return availableBytes < 0 || availableBytes >= downloadBytes() + headroomBytes;
}

ModelStore::ModelStore(const QString &modelsDirectory, QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_directory(modelsDirectory), m_network(network) {
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(2000);
    connect(&m_retryTimer, &QTimer::timeout, this, &ModelStore::request);
    connect(&m_hash, &QFutureWatcher<QByteArray>::finished, this, &ModelStore::verified);
}

ModelStore::~ModelStore() {
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        delete m_reply;
    }
    m_part.close();
    m_hash.waitForFinished();
}

ModelStore::Plan ModelStore::plan(const Catalogue &catalogue, const ModelLevel &level, bool withUpscaler) const {
    Plan plan;
    QStringList keys;
    for (const QString &key : level.files)
        if (!keys.contains(key))
            keys << key;
    if (withUpscaler && catalogue.upscaler())
        keys << catalogue.upscaler()->key;
    for (const QString &key : keys) {
        const CatalogueFile *file = catalogue.file(key);
        if (!file || Catalogue::state(*file, m_directory) == Catalogue::FileState::Ok)
            continue;
        plan.files << *file;
        plan.totalBytes += file->bytes;
        const QFileInfo part(partPath(*file, m_directory));
        if (part.isFile() && part.size() <= file->bytes)
            plan.partBytes += part.size();
    }
    plan.availableBytes = availableBytes(m_directory);
    return plan;
}

qint64 ModelStore::availableBytes(const QString &directory) {
    QString existing = directory;
    while (!existing.isEmpty() && !QFileInfo::exists(existing) && existing != "/")
        existing = QFileInfo(existing).absolutePath();
    const QStorageInfo storage(existing);
    return storage.isValid() ? storage.bytesAvailable() : -1;
}

void ModelStore::start(const QList<CatalogueFile> &files) {
    if (m_running)
        return;
    m_files = files;
    m_index = -1;
    m_totalBytes = 0;
    for (const auto &file : files)
        m_totalBytes += file.bytes;
    m_installedBytes = 0;
    m_running = true;
    m_cancelling = false;
    next();
}

void ModelStore::cancel() {
    if (!m_running)
        return;
    m_cancelling = true;
    if (m_reply) {
        m_reply->abort(); // replyFinished reports the cancel
    } else if (!m_hashing) {
        // Waiting between retries: nothing is in flight.
        m_retryTimer.stop();
        m_part.close();
        m_running = false;
        emit cancelled();
    } // while hashing, verified() reports the cancel
}

void ModelStore::next() {
    ++m_index;
    if (m_index >= m_files.size()) {
        m_running = false;
        emit finished();
        return;
    }
    const CatalogueFile &file = m_files.at(m_index);
    m_attempt = 0;
    QDir().mkpath(m_directory + "/" + file.folder);
    // A .part larger than the file can only be wrong; resuming would keep it
    // wrong, so it starts again.
    const QString part = partPath(file, m_directory);
    if (QFileInfo(part).size() > file.bytes)
        QFile::remove(part);
    if (QFileInfo(part).size() == file.bytes) {
        verify();
        return;
    }
    request();
}

void ModelStore::request() {
    if (m_cancelling) {
        m_running = false;
        emit cancelled();
        return;
    }
    const CatalogueFile &file = m_files.at(m_index);
    m_part.setFileName(partPath(file, m_directory));
    if (!m_part.open(QIODevice::WriteOnly | QIODevice::Append)) {
        stop(tr("Cannot write %1: %2").arg(m_part.fileName(), m_part.errorString()));
        return;
    }
    if (!m_network) {
        m_ownNetwork = std::make_unique<QNetworkAccessManager>();
        m_network = m_ownNetwork.get();
    }
    QNetworkRequest request{downloadUrl(file.url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "limgen/" + QCoreApplication::applicationVersion());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // A stalled connection is given up after a minute and retried.
    request.setTransferTimeout(60000);
    if (m_part.size() > 0)
        request.setRawHeader("Range", "bytes=" + QByteArray::number(m_part.size()) + "-");
    m_rangeChecked = false;
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &ModelStore::readyRead);
    connect(m_reply, &QNetworkReply::finished, this, &ModelStore::replyFinished);
    emit progress(doneBytes(), m_totalBytes, file.name);
}

void ModelStore::readyRead() {
    if (!m_reply)
        return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200 && status != 206) {
        m_reply->readAll(); // an error page, never written to the model file
        return;
    }
    if (!m_rangeChecked) {
        m_rangeChecked = true;
        // A server that ignores the range sends the whole file again, and one
        // that resumes elsewhere would corrupt it; either way, start over.
        const QByteArray expected = "bytes " + QByteArray::number(m_part.size()) + "-";
        if (status == 200 || !m_reply->rawHeader("Content-Range").startsWith(expected))
            m_part.resize(0);
    }
    const CatalogueFile &file = m_files.at(m_index);
    const QByteArray data = m_reply->readAll();
    if (m_part.size() + data.size() > file.bytes) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
        m_part.close();
        QFile::remove(m_part.fileName());
        stop(tr("%1 is larger than limgen expects; the download was removed.").arg(file.name));
        return;
    }
    // Progress means the link works, so a later drop gets fresh retries; a
    // big file on a flaky connection must not fail on its fourth drop.
    if (!data.isEmpty())
        m_attempt = 0;
    if (m_part.write(data) != data.size()) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
        stop(tr("Cannot write %1: %2").arg(m_part.fileName(), m_part.errorString()));
        return;
    }
    emit progress(doneBytes(), m_totalBytes, file.name);
}

void ModelStore::replyFinished() {
    if (!m_reply)
        return;
    if (m_reply->bytesAvailable() > 0)
        readyRead();
    if (!m_reply)
        return; // readyRead stopped the download
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    m_part.close();

    if (m_cancelling) {
        m_running = false;
        emit cancelled();
        return;
    }
    const CatalogueFile &file = m_files.at(m_index);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const qint64 size = QFileInfo(m_part.fileName()).size();
    // 416: the range starts at the end, so the file is already complete.
    if (status == 416 || (reply->error() == QNetworkReply::NoError && size == file.bytes)) {
        verify();
        return;
    }
    if (reply->error() != QNetworkReply::NoError && isFinal(reply->error())) {
        stop(tr("Cannot download %1: %2").arg(file.name, reply->errorString()));
        return;
    }
    // A dropped connection, a timeout or a short body: try again from where
    // the .part file ends.
    if (m_attempt < retries) {
        ++m_attempt;
        m_retryTimer.start();
        return;
    }
    stop(reply->error() == QNetworkReply::HostNotFoundError
             ? tr("Cannot reach %1. Check the network connection; the download resumes where it stopped.")
                   .arg(QUrl(file.url).host())
             : tr("Download of %1 failed: %2. Run the same command again to resume.")
                   .arg(file.name, reply->error() == QNetworkReply::NoError ? tr("the connection ended early")
                                                                           : reply->errorString()));
}

void ModelStore::verify() {
    const CatalogueFile &file = m_files.at(m_index);
    // Set before the signal, so a cancel from a progress handler waits for
    // verified() instead of reporting the cancel a second time.
    m_hashing = true;
    emit progress(doneBytes(), m_totalBytes, file.name);
    // Hashing gigabytes takes seconds, so it runs off the event loop.
    m_hash.setFuture(QtConcurrent::run(sha256Of, partPath(file, m_directory)));
}

void ModelStore::verified() {
    m_hashing = false;
    const CatalogueFile &file = m_files.at(m_index);
    const QString part = partPath(file, m_directory);
    const QByteArray hash = m_hash.result();
    // A file that cannot be read is not known to be bad; it is kept, so a
    // passing disk error does not throw away gigabytes.
    if (hash.isEmpty()) {
        stop(tr("Cannot read %1 to check it.").arg(part));
        return;
    }
    if (hash != file.sha256.toLatin1()) {
        QFile::remove(part);
        stop(tr("%1 failed its checksum and was removed. Run the same command again to download it anew.")
                 .arg(file.name));
        return;
    }
    // A complete file is installed even when a cancel came in meanwhile.
    // rename(2) replaces a wrong-sized leftover in one step, so the target
    // is never missing in between.
    const QString target = Catalogue::path(file, m_directory);
    if (::rename(QFile::encodeName(part).constData(), QFile::encodeName(target).constData()) != 0) {
        stop(tr("Cannot move %1 into place.").arg(target));
        return;
    }
    m_installedBytes += file.bytes;
    emit fileInstalled(file.key);
    if (m_cancelling) {
        m_running = false;
        emit cancelled();
        return;
    }
    next();
}

void ModelStore::stop(const QString &message) {
    m_retryTimer.stop();
    m_part.close();
    m_running = false;
    emit failed(message);
}

qint64 ModelStore::doneBytes() const {
    if (m_index < 0 || m_index >= m_files.size())
        return m_installedBytes;
    const QFileInfo part(partPath(m_files.at(m_index), m_directory));
    return m_installedBytes + (part.isFile() ? part.size() : 0);
}
