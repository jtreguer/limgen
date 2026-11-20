#pragma once

#include <QString>
#include <QStringList>

struct Palette;

// Builds the text sent to the image model from the user's subject, the theme
// palette and a style preset. Pure: no I/O, no state.
//
// The turbo models limgen uses ignore negative prompts, so each style works
// through its positive words; the negative prompt only matters to models
// that use guidance. Steps, guidance and sampler come from the model
// catalogue, not from the style.
namespace Prompt {

enum class Style { Photo, Cinematic, Painting, Minimal };

struct Text {
    QString positive;
    QString negative;
};

// Stable identifiers for the styles, used on the command line and in
// metadata: "photo", "cinematic", "painting", "minimal".
QStringList styleIds();
QString styleId(Style style);
// Returns false, leaving *style alone, for an unknown identifier.
bool styleFromId(const QString &id, Style *style);

constexpr Style defaultStyle = Style::Cinematic;

// "<subject>, <style lead>, <palette words> colour palette, <mood>,
// <style details>, desktop wallpaper". An invalid palette (no Omarchy theme)
// leaves out the palette and the mood rather than guessing.
Text build(const QString &subject, const Palette &palette, Style style = defaultStyle);

} // namespace Prompt
