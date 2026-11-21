#pragma once

#include <QJsonObject>
#include <QString>

// The user's choices that outlive a session, in ~/.config/limgen/config.json
// (docs/technical.md section 7): the model and the style for now; colour
// pass and output format arrive with keeping (L5). Unknown keys are kept, so
// a newer limgen's settings survive an older one.
class Config {
public:
    explicit Config(const QString &file = defaultFile());

    // $XDG_CONFIG_HOME/limgen/config.json.
    static QString defaultFile();
    QString file() const { return m_file; }

    QString modelId() const { return m_values.value("model").toString(); }
    void setModelId(const QString &id) { m_values.insert("model", id); }
    QString styleId() const { return m_values.value("style").toString(); }
    void setStyleId(const QString &id) { m_values.insert("style", id); }

    // Written beside the file and renamed into place.
    bool save(QString *error = nullptr) const;

private:
    QString m_file;
    QJsonObject m_values;
};
