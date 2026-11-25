#pragma once

#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QStringList>

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
    // How far the window's variations move from their source, 0.1 to 0.9;
    // 0.5 by default.
    static constexpr double defaultStrength = 0.5;
    double strength() const;
    void setStrength(double strength);

    // The engine build in use, by backend ("vulkan", "cuda"); empty for the
    // first one installed.
    QString engine() const { return m_values.value("engine").toString(); }
    void setEngine(const QString &backend) { m_values.insert("engine", backend); }

    // The Generic screen's last choices, under "generic": the target size
    // (empty when none was chosen, which means the largest monitor's), how
    // many previews (2, 4, 6 or 9; 4 by default), the palette ("" for none,
    // "custom", or a theme's slug) and the custom colours ("#rrggbb", at
    // most five), and how much the previews differ.
    QSize genericSize() const;
    void setGenericSize(const QSize &size);
    static constexpr int defaultGenericCount = 4;
    int genericCount() const;
    void setGenericCount(int count);
    QString genericPalette() const;
    void setGenericPalette(const QString &palette);
    QStringList genericColors() const;
    void setGenericColors(const QStringList &colors);
    // "seeds" (the default), "framing" or "light" (Prompt::varietyIds()).
    QString genericVariety() const;
    void setGenericVariety(const QString &variety);

    // Written beside the file and renamed into place.
    bool save(QString *error = nullptr) const;

private:
    QString m_file;
    QJsonObject m_values;
};
