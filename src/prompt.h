#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

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

// How much the previews of one generic batch differ beyond their seeds. The
// turbo models limgen runs change little from one seed to the next, and
// neither a noise-adding sampler nor a stronger flow shift changed that in
// a test on the reference card; a different framing in each preview's
// prompt did (docs/models.md section 6). Seeds: one prompt for the batch.
// Framing: each preview gets its own camera and composition hint. Light:
// each also gets its own light and atmosphere.
enum class Variety { Seeds, Framing, Light };

// "seeds", "framing", "light": on the command line, in config.json and in
// metadata.
QStringList varietyIds();
QString varietyId(Variety variety);
// Returns false, leaving *variety alone, for an unknown identifier.
bool varietyFromId(const QString &id, Variety *variety);

// One hint per preview: empty for Seeds; otherwise `count` hints that
// differ from each other while the lists last, in an order `seed` decides,
// so a batch made again with the same seed gets the same hints. English,
// since they are model input.
QStringList varietyHints(Variety variety, int count, quint32 seed);
QStringList framingHints();
QStringList lightHints();

// For the Generic screen and limgen image: the user's prompt as typed, the
// preview's variety hint if any, then "<palette words> colour palette" and
// the palette's mood when it has one. No style words and no "desktop
// wallpaper": the image can be anything. An invalid or empty palette leaves
// the prompt as it is. The negative prompt is the one every style shares.
Text buildGeneric(const QString &prompt, const Palette &palette, const QString &hint = QString());

} // namespace Prompt
