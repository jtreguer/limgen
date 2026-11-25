#include "omarchy.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

namespace {

QString fromEnv(const char *name, const QString &fallback) {
    const QString value = qEnvironmentVariable(name);
    return value.isEmpty() ? fallback : value;
}

QStringList imagesIn(const QString &directory) {
    if (directory.isEmpty())
        return {};
    static const QStringList suffixes{"jpg", "jpeg", "png", "webp"};
    QStringList images;
    const auto entries = QDir(directory).entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &entry : entries)
        if (suffixes.contains(entry.suffix().toLower()))
            images << entry.absoluteFilePath();
    return images;
}

} // namespace

OmarchyPaths OmarchyPaths::fromEnvironment() {
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString state = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    OmarchyPaths paths;
    paths.state = fromEnv("LIMGEN_OMARCHY_STATE", state + "/omarchy/current");
    paths.backgrounds = fromEnv("LIMGEN_OMARCHY_BACKGROUNDS", config + "/omarchy/backgrounds");
    paths.omarchy = fromEnv("LIMGEN_OMARCHY_BIN", QStandardPaths::findExecutable("omarchy"));
    // Without a running Hyprland, hyprctl has nothing to answer, so it is not
    // used even when installed.
    paths.hyprctl = fromEnv("LIMGEN_HYPRCTL", qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                                                  ? QString()
                                                  : QStandardPaths::findExecutable("hyprctl"));
    paths.language = config + "/omarchy/language";
    const QString themes = qEnvironmentVariable("LIMGEN_OMARCHY_THEMES");
    paths.themes = themes.isEmpty() ? QStringList{config + "/omarchy/themes", "/usr/share/omarchy/themes"}
                                    : themes.split(':', Qt::SkipEmptyParts);
    paths.pictures = fromEnv("LIMGEN_PICTURES",
                             QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/limgen");
    return paths;
}

Omarchy::Omarchy(const OmarchyPaths &paths, QObject *parent) : QObject(parent), m_paths(paths) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        rearm();
        emit changed();
    });
    rearm();
}

bool Omarchy::isSafeSlug(const QString &slug) {
    // anchoredPattern, because a plain `$` would also accept a final newline.
    static const QRegularExpression valid(QRegularExpression::anchoredPattern("[A-Za-z0-9][A-Za-z0-9._-]*"));
    return valid.match(slug).hasMatch() && !slug.contains("..");
}

QString Omarchy::slug() const {
    QFile file(m_paths.state + "/theme.name");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString slug = QString::fromUtf8(file.readLine()).trimmed();
    return isSafeSlug(slug) ? slug : QString();
}

QString Omarchy::colorsPath() const { return m_paths.state + "/theme/colors.toml"; }

QString Omarchy::readColors(QString *error) const {
    QFile file(colorsPath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QCoreApplication::translate("Omarchy", "Cannot read %1: %2").arg(colorsPath(), file.errorString());
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QStringList Omarchy::themeBackgrounds() const { return imagesIn(m_paths.state + "/theme/backgrounds"); }

QString Omarchy::userBackgroundDirectory() const {
    const QString name = slug();
    return name.isEmpty() ? QString() : m_paths.backgrounds + "/" + name;
}

QStringList Omarchy::userBackgrounds() const { return imagesIn(userBackgroundDirectory()); }

QString Omarchy::themeDirectory(const QString &slug) const {
    if (!isSafeSlug(slug))
        return {};
    if (slug == this->slug())
        return m_paths.state + "/theme";
    for (const QString &root : m_paths.themes)
        if (QFileInfo(root + "/" + slug).isDir())
            return root + "/" + slug;
    return {};
}

QStringList Omarchy::themeSlugs() const {
    QStringList slugs;
    for (const QString &root : m_paths.themes)
        for (const QString &name : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (isSafeSlug(name) && !slugs.contains(name) && QFileInfo(root + "/" + name + "/colors.toml").isFile())
                slugs << name;
    const QString current = slug();
    if (!current.isEmpty() && !slugs.contains(current) && QFileInfo(colorsPath()).isFile())
        slugs << current;
    slugs.sort(Qt::CaseInsensitive);
    return slugs;
}

QStringList Omarchy::themeBackgrounds(const QString &slug) const {
    const QString directory = themeDirectory(slug);
    return directory.isEmpty() ? QStringList() : imagesIn(directory + "/backgrounds");
}

QString Omarchy::keepDirectory(const QString &slug) const {
    if (slug.isEmpty())
        return m_paths.pictures;
    return isSafeSlug(slug) ? m_paths.backgrounds + "/" + slug : QString();
}

QString Omarchy::currentBackground() const {
    const QFileInfo link(m_paths.state + "/background");
    return link.isSymLink() ? link.symLinkTarget() : QString();
}

bool Omarchy::setBackground(const QString &file, QString *error) const {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (m_paths.omarchy.isEmpty())
        return fail(QCoreApplication::translate("Omarchy", "Omarchy is not installed, so limgen cannot set the "
                                                           "background. The image is at %1.")
                        .arg(file));
    QProcess process;
    process.start(m_paths.omarchy, {"theme", "bg", "set", file});
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished(1000);
        return fail(QCoreApplication::translate("Omarchy", "omarchy theme bg set did not finish."));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return fail(QCoreApplication::translate("Omarchy", "omarchy theme bg set failed: %1")
                        .arg(QString::fromUtf8(process.readAllStandardError()).trimmed()));
    return true;
}

QList<Monitor> Omarchy::monitors() const {
    if (m_paths.hyprctl.isEmpty())
        return {};
    QProcess process;
    process.start(m_paths.hyprctl, {"monitors", "-j"});
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return parseMonitors(process.readAllStandardOutput());
}

QList<Monitor> Omarchy::parseMonitors(const QByteArray &json) {
    QList<Monitor> monitors;
    for (const QJsonValue &value : QJsonDocument::fromJson(json).array()) {
        const QJsonObject object = value.toObject();
        if (object.value("disabled").toBool())
            continue;
        Monitor monitor{object.value("name").toString(), object.value("width").toInt(),
                        object.value("height").toInt()};
        if (monitor.width <= 0 || monitor.height <= 0)
            continue;
        // Hyprland reports the mode's size; transforms 1, 3, 5 and 7 turn the
        // monitor a quarter, so its background is taller than wide.
        if (object.value("transform").toInt() % 2 == 1)
            std::swap(monitor.width, monitor.height);
        monitors << monitor;
    }
    return monitors;
}

Monitor Omarchy::largest(const QList<Monitor> &monitors) {
    Monitor best;
    for (const Monitor &monitor : monitors)
        if (qint64(monitor.width) * monitor.height > qint64(best.width) * best.height)
            best = monitor;
    return best;
}

QSize Omarchy::largestMonitorSize() const {
    const Monitor monitor = largest(monitors());
    return monitor.width > 0 && monitor.height > 0 ? QSize(monitor.width, monitor.height) : QSize(2560, 1440);
}

void Omarchy::rearm() {
    const auto watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);

    // A theme switch replaces files and symlinks, which drops them from the
    // watch, so the parents are watched too and everything is re-armed after
    // each change. Watching the nearest existing ancestor means Omarchy
    // installed later is noticed without a restart.
    QString ancestor = m_paths.state;
    while (!ancestor.isEmpty() && !QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, m_paths.state, m_paths.state + "/theme", m_paths.state + "/theme.name",
                           colorsPath(), m_paths.state + "/theme/backgrounds", m_paths.backgrounds,
                           userBackgroundDirectory()};
    candidates.removeAll(QString());
    candidates.removeDuplicates();
    for (const QString &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}
