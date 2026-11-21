#pragma once

#include <QJsonObject>
#include <QString>

// The user's choices that outlive a session, in ~/.config/limgen/config.json
// (docs/technical.md section 7): the model, the style, and how kept images
// are finished. Unknown keys are kept, so a newer limgen's settings survive
// an older one; a value out of range reads as the default.
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

    // The colour pass strength for kept images, 0 (off) to 1; 0.3 by default,
    // a light touch (docs/design.md section 9).
    static constexpr double defaultColorPass = 0.3;
    double colorPass() const;
    void setColorPass(double strength);
    // "png" (the default) or "jpeg".
    QString format() const;
    void setFormat(const QString &format);
    // 1 to 100; 92 by default.
    static constexpr int defaultJpegQuality = 92;
    int jpegQuality() const;

    // Written beside the file and renamed into place.
    bool save(QString *error = nullptr) const;

private:
    QString m_file;
    QJsonObject m_values;
};
