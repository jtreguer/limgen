#include "prompt.h"

#include "palette.h"

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

} // namespace Prompt
