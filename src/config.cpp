#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

Config::Config(const QString &file) : m_file(file) {
    QFile input(m_file);
    // A missing or unreadable file means defaults; it is rewritten on the
    // next save.
    if (input.open(QIODevice::ReadOnly))
        m_values = QJsonDocument::fromJson(input.readAll()).object();
}

double Config::colorPass() const {
    const QJsonValue value = m_values.value("colorPass");
    const double strength = value.toDouble(-1);
    return value.isDouble() && strength >= 0 && strength <= 1 ? strength : defaultColorPass;
}

void Config::setColorPass(double strength) { m_values.insert("colorPass", qBound(0.0, strength, 1.0)); }

QString Config::format() const { return m_values.value("format").toString() == "jpeg" ? "jpeg" : "png"; }

void Config::setFormat(const QString &format) { m_values.insert("format", format == "jpeg" ? "jpeg" : "png"); }

int Config::jpegQuality() const {
    const int quality = m_values.value("jpegQuality").toInt(-1);
    return quality >= 1 && quality <= 100 ? quality : defaultJpegQuality;
}

QString Config::defaultFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/limgen/config.json";
}

bool Config::save(QString *error) const {
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile output(m_file);
    if (output.open(QIODevice::WriteOnly) && output.write(QJsonDocument(m_values).toJson()) >= 0 && output.commit())
        return true;
    if (error)
        *error = QCoreApplication::translate("Config", "Cannot save %1: %2").arg(m_file, output.errorString());
    return false;
}
