#include "config.h"

#include "palette.h"
#include "prompt.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
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

double Config::strength() const {
    const QJsonValue value = m_values.value("strength");
    const double strength = value.toDouble(-1);
    return value.isDouble() && strength >= 0.1 && strength <= 0.9 ? strength : defaultStrength;
}

void Config::setStrength(double strength) { m_values.insert("strength", qBound(0.1, strength, 0.9)); }

namespace {

// One generic setting; the others under "generic" are kept as they are.
QJsonValue generic(const QJsonObject &values, const char *key) { return values.value("generic").toObject().value(key); }

void setGeneric(QJsonObject &values, const char *key, const QJsonValue &value) {
    QJsonObject object = values.value("generic").toObject();
    object.insert(key, value);
    values.insert("generic", object);
}

} // namespace

QSize Config::genericSize() const {
    const QJsonArray size = generic(m_values, "size").toArray();
    const int width = size.at(0).toInt(), height = size.at(1).toInt();
    return width > 0 && height > 0 ? QSize(width, height) : QSize();
}

void Config::setGenericSize(const QSize &size) {
    setGeneric(m_values, "size", size.isEmpty() ? QJsonValue() : QJsonValue(QJsonArray{size.width(), size.height()}));
}

int Config::genericCount() const {
    const int count = generic(m_values, "count").toInt();
    return count == 2 || count == 4 || count == 6 || count == 9 ? count : defaultGenericCount;
}

void Config::setGenericCount(int count) { setGeneric(m_values, "count", count); }

QString Config::genericPalette() const { return generic(m_values, "palette").toString(); }

void Config::setGenericPalette(const QString &palette) { setGeneric(m_values, "palette", palette); }

QStringList Config::genericColors() const {
    static const QRegularExpression hex(QRegularExpression::anchoredPattern("#[0-9a-f]{6}"));
    QStringList colors;
    for (const QJsonValue &value : generic(m_values, "colors").toArray())
        if (const QString color = value.toString().toLower(); hex.match(color).hasMatch() && colors.size() < Palette::maxColors)
            colors << color;
    return colors;
}

void Config::setGenericColors(const QStringList &colors) {
    setGeneric(m_values, "colors", QJsonArray::fromStringList(colors));
}

QString Config::genericVariety() const {
    const QString variety = generic(m_values, "variety").toString();
    return Prompt::varietyIds().contains(variety) ? variety : Prompt::varietyIds().first();
}

void Config::setGenericVariety(const QString &variety) { setGeneric(m_values, "variety", variety); }

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
