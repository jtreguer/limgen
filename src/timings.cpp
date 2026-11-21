#include "timings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

double Timing::estimate(int count, int steps) const {
    if (!isKnown() || count < 1 || steps < 1)
        return -1;
    return overheadSeconds + count * (steps * secondsPerStep + decodeSecondsPerImage);
}

Timings::Timings(const QString &file) : m_file(file) {
    QFile input(m_file);
    if (input.open(QIODevice::ReadOnly))
        m_values = QJsonDocument::fromJson(input.readAll()).object();
}

QString Timings::defaultFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/limgen/bench.json";
}

QString Timings::key(const QString &model, const QString &level, const QString &backend) {
    return model + "/" + level + "/" + backend;
}

Timing Timings::get(const QString &key) const {
    const QJsonObject o = m_values.value(key).toObject();
    return {o.value("overheadSeconds").toDouble(-1), o.value("secondsPerStep").toDouble(-1),
            o.value("decodeSecondsPerImage").toDouble(-1)};
}

bool Timings::record(const QString &key, const Timing &measured) {
    const Timing old = get(key);
    auto blend = [](double previous, double now) {
        if (now < 0)
            return previous;
        return previous < 0 ? now : (previous + now) / 2;
    };
    const Timing merged{blend(old.overheadSeconds, measured.overheadSeconds),
                        blend(old.secondsPerStep, measured.secondsPerStep),
                        blend(old.decodeSecondsPerImage, measured.decodeSecondsPerImage)};
    m_values.insert(key, QJsonObject{{"overheadSeconds", merged.overheadSeconds},
                                     {"secondsPerStep", merged.secondsPerStep},
                                     {"decodeSecondsPerImage", merged.decodeSecondsPerImage}});
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile output(m_file);
    return output.open(QIODevice::WriteOnly) && output.write(QJsonDocument(m_values).toJson()) >= 0 &&
           output.commit();
}
