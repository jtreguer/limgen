#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include "palette.h"

class Omarchy;

// The window's state for QML. It holds no file logic of its own: the theme
// comes from Omarchy and its words from Palette, and both are re-read when
// Omarchy reports a change.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString themeName READ themeName NOTIFY paletteChanged)
    Q_PROPERTY(QVariantList swatches READ swatches NOTIFY paletteChanged)
    Q_PROPERTY(QString paletteWords READ paletteWords NOTIFY paletteChanged)
    Q_PROPERTY(QString mood READ mood NOTIFY paletteChanged)
    Q_PROPERTY(QString status READ status NOTIFY paletteChanged)

public:
    explicit Backend(Omarchy *omarchy, QObject *parent = nullptr);

    QString themeName() const { return m_themeName; }
    // One map per palette colour: role, hex, words.
    QVariantList swatches() const;
    QString paletteWords() const { return m_palette.isValid() ? m_palette.words.join(", ") : QString(); }
    QString mood() const { return m_palette.isValid() ? m_palette.mood : QString(); }
    // What is wrong, said plainly, or an empty string.
    QString status() const { return m_status; }

    const Palette &palette() const { return m_palette; }

public slots:
    void reload();

signals:
    void paletteChanged();

private:
    Omarchy *m_omarchy;
    QString m_themeName;
    Palette m_palette;
    QString m_status;
};
