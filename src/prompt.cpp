#include "prompt.h"

#include "palette.h"

#include <QRandomGenerator>

namespace Prompt {

namespace {

struct Preset {
    Style style;
    const char *id;
    const char *lead;    // what kind of picture, right after the subject
    const char *details; // how it is made, after the palette and mood
    const char *avoid;   // added to the shared negative prompt
};

// The cinematic words are the ones the L0 benchmark used, so its results
// (docs/models.md section 6) describe what the default style produces.
const Preset presets[] = {
    {Style::Photo, "photo", "photograph, wide angle", "realistic lighting, sharp focus, highly detailed",
     "painting, illustration, drawing"},
    {Style::Cinematic, "cinematic", "cinematic wide shot", "atmospheric, highly detailed", ""},
    {Style::Painting, "painting", "oil painting, wide landscape format",
     "visible brush strokes, painterly, rich texture", "photograph"},
    {Style::Minimal, "minimal", "minimalist composition", "simple shapes, large calm areas, flat colour, clean",
     "clutter, busy detail"},
};

const char *const sharedAvoid = "text, watermark, signature, logo, frame, border, blurry, lowres, jpeg artifacts";

const Preset &preset(Style style) {
    for (const auto &p : presets)
        if (p.style == style)
            return p;
    return presets[1];
}

} // namespace

QStringList styleIds() {
    QStringList ids;
    for (const auto &p : presets)
        ids << p.id;
    return ids;
}

QString styleId(Style style) { return preset(style).id; }

bool styleFromId(const QString &id, Style *style) {
    for (const auto &p : presets)
        if (id == QLatin1String(p.id)) {
            *style = p.style;
            return true;
        }
    return false;
}

Text build(const QString &subject, const Palette &palette, Style style) {
    const Preset &p = preset(style);
    QStringList parts;
    if (!subject.trimmed().isEmpty())
        parts << subject.simplified();
    parts << p.lead;
    if (palette.isValid() && !palette.words.isEmpty()) {
        parts << palette.words.join(", ") + " colour palette";
        parts << palette.mood;
    }
    parts << p.details << "desktop wallpaper";

    Text text;
    text.positive = parts.join(", ");
    text.negative = *p.avoid ? QString("%1, %2").arg(sharedAvoid, p.avoid) : QString(sharedAvoid);
    return text;
}

QStringList varietyIds() { return {"seeds", "framing", "light"}; }

QString varietyId(Variety variety) { return varietyIds().at(int(variety)); }

bool varietyFromId(const QString &id, Variety *variety) {
    const int index = varietyIds().indexOf(id);
    if (index < 0)
        return false;
    *variety = Variety(index);
    return true;
}

// Framings that suit almost any subject: how far, from where, and how the
// picture is laid out. None names a time of day or a weather, so they do
// not fight a prompt that does.
QStringList framingHints() {
    return {"wide shot, the subject small in a vast scene",
            "close-up, the subject filling the frame",
            "low angle, looking up",
            "high angle, looking down",
            "aerial view from far above",
            "seen from far away through a telephoto lens",
            "off-centre composition following the rule of thirds",
            "symmetrical composition, centred",
            "framed by foreground elements",
            "side view",
            "panoramic view",
            "strong diagonal composition"};
}

QStringList lightHints() {
    return {"golden hour light", "soft overcast light", "blue hour", "at night under moonlight",
            "in morning fog", "dramatic stormy light", "bright midday sun", "backlit, against the light",
            "warm sunset glow", "light rain"};
}

namespace {

// A Fisher-Yates shuffle on QRandomGenerator's own numbers rather than
// std::shuffle, whose order differs between standard libraries: the same
// seed then gives the same hints everywhere.
QStringList shuffled(QStringList list, quint32 seed) {
    QRandomGenerator random(seed);
    for (qsizetype i = list.size() - 1; i > 0; --i)
        list.swapItemsAt(i, qsizetype(random.bounded(quint32(i + 1))));
    return list;
}

} // namespace

QStringList varietyHints(Variety variety, int count, quint32 seed) {
    QStringList hints;
    if (count <= 0)
        return hints;
    if (variety == Variety::Seeds) {
        for (int i = 0; i < count; ++i)
            hints << QString();
        return hints;
    }
    const QStringList framings = shuffled(framingHints(), seed);
    // Another stream for the light, so it does not pair with the framing
    // the same way in every batch.
    const QStringList lights = shuffled(lightHints(), seed ^ 0x9e3779b9u);
    for (int i = 0; i < count; ++i) {
        QString hint = framings.at(i % framings.size());
        if (variety == Variety::Light)
            hint += ", " + lights.at(i % lights.size());
        hints << hint;
    }
    return hints;
}

Text buildGeneric(const QString &prompt, const Palette &palette, const QString &hint) {
    QStringList parts;
    if (!prompt.trimmed().isEmpty())
        parts << prompt.simplified();
    if (!hint.trimmed().isEmpty())
        parts << hint.simplified();
    if (palette.isValid() && !palette.words.isEmpty()) {
        parts << palette.words.join(", ") + " colour palette";
        if (!palette.mood.isEmpty())
            parts << palette.mood;
    }
    Text text;
    text.positive = parts.join(", ");
    text.negative = sharedAvoid;
    return text;
}

} // namespace Prompt
