#include "backend.h"

#include "omarchy.h"

#include <QVariantMap>

Backend::Backend(Omarchy *omarchy, QObject *parent) : QObject(parent), m_omarchy(omarchy) {
    connect(m_omarchy, &Omarchy::changed, this, &Backend::reload);
    reload();
}

QVariantList Backend::swatches() const {
    QVariantList list;
    if (!m_palette.isValid())
        return list;
    for (const PaletteColor &colour : m_palette.colors)
        list << QVariantMap{{"role", colour.role}, {"hex", colour.hex}, {"words", colour.words}};
    return list;
}

void Backend::reload() {
    m_themeName = m_omarchy->slug();
    if (m_themeName.isEmpty()) {
        m_palette = Palette{};
        m_palette.error = tr("No Omarchy theme found");
        m_status = tr("No Omarchy theme found in %1. limgen still runs, with the default accent and no palette.")
                       .arg(m_omarchy->paths().state);
    } else {
        QString error;
        const QString text = m_omarchy->readColors(&error);
        m_palette = error.isEmpty() ? Palette::fromToml(text) : Palette{};
        if (!error.isEmpty())
            m_palette.error = error;
        m_status = m_palette.isValid() ? QString() : m_palette.error;
    }
    emit paletteChanged();
}
