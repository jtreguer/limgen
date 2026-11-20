#include "palette.h"

#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace {

struct Rgb {
    double r, g, b; // linear sRGB, 0 to 1
};

bool parseHex(const QString &hex, Rgb *rgb) {
    // anchoredPattern, because a plain `$` would also accept a final newline.
    static const QRegularExpression valid(QRegularExpression::anchoredPattern("#[0-9a-fA-F]{6}"));
    if (!valid.match(hex).hasMatch())
        return false;
    auto channel = [&](int at) {
        const double v = hex.mid(at, 2).toInt(nullptr, 16) / 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    *rgb = {channel(1), channel(3), channel(5)};
    return true;
}

// CIELAB under D65, the space the colour pass and the benchmark's palette
// distance work in.
void cielab(const Rgb &c, double out[3]) {
    const double x = (0.4124564 * c.r + 0.3575761 * c.g + 0.1804375 * c.b) / 0.95047;
    const double y = 0.2126729 * c.r + 0.7151522 * c.g + 0.0721750 * c.b;
    const double z = (0.0193339 * c.r + 0.1191920 * c.g + 0.9503041 * c.b) / 1.08883;
    const double d = 6.0 / 29.0;
    auto f = [d](double t) { return t > d * d * d ? std::cbrt(t) : t / (3 * d * d) + 4.0 / 29.0; };
    out[0] = 116 * f(y) - 16;
    out[1] = 500 * (f(x) - f(y));
    out[2] = 200 * (f(y) - f(z));
}

// OKLCH, with lightness and chroma scaled by 100. Colours are named in OKLab
// rather than CIELAB because CIELAB puts pure blue and violet only 6 degrees
// apart, which would make "blue" and "violet" unreliable words.
struct Lch {
    double l, c, h;
};

Lch oklch(const Rgb &c) {
    const double l = std::cbrt(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b);
    const double m = std::cbrt(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    const double s = std::cbrt(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    double h = std::atan2(b, a) * 180.0 / M_PI;
    if (h < 0)
        h += 360;
    return {L * 100, std::hypot(a, b) * 100, h};
}

// The hue family for an OKLCH colour, with the names people use for some
// families at the light or dark end ("lavender", "navy", "brown").
QString family(const Lch &c) {
    static const struct {
        double end;
        const char *name;
    } families[] = {{15, "rose"},  {40, "red"},       {70, "orange"}, {95, "amber"},
                    {120, "yellow"}, {165, "green"},  {210, "teal"},  {245, "sky blue"},
                    {275, "blue"},  {310, "violet"},  {345, "magenta"}, {361, "rose"}};
    QString name;
    for (const auto &f : families)
        if (c.h < f.end) {
            name = f.name;
            break;
        }
    if (name == "orange")
        return c.l < 55 ? "brown" : c.l > 82 ? "peach" : name;
    // Cream is a light yellow with little colour; a light saturated yellow,
    // such as the default accent, keeps its family and is called bright.
    if (name == "amber")
        return c.l < 60 ? "ochre" : c.l > 85 && c.c < 10 ? "cream" : name;
    if (name == "yellow")
        return c.l < 65 ? "olive" : c.l > 85 && c.c < 10 ? "cream" : name;
    if (name == "green" && c.l > 85)
        return "mint";
    if (name == "teal" && c.l > 78)
        return "cyan";
    if (name == "blue")
        return c.l < 35 ? "navy" : c.l > 72 && c.c < 8 ? "periwinkle" : name;
    if (name == "violet")
        return c.l < 40 ? "indigo" : c.l > 72 ? "lavender" : name;
    if (name == "magenta")
        return c.l < 50 ? "purple" : c.l > 80 ? "pink" : name;
    if (name == "rose" && c.l > 80)
        return "pink";
    return name;
}

// Names that already say how light or dark the colour is, so "pale" or
// "deep" in front of them would only repeat it.
bool impliesLightness(const QString &name) {
    static const QStringList names{"peach", "cream", "mint", "cyan", "periwinkle", "lavender", "pink",
                                   "brown", "ochre", "olive", "navy", "indigo", "purple"};
    return names.contains(name);
}

const char *const namedColours[] = {"red", "yellow", "orange", "green", "cyan", "blue", "magenta", "brown"};

} // namespace

QString Palette::describe(const QString &hex) {
    Rgb rgb;
    if (!parseHex(hex, &rgb))
        return {};
    const Lch c = oklch(rgb);

    // Greys, and very dark colours with only a hint of hue, are named by
    // lightness; the hue is kept as a tint on near-black, where theme
    // backgrounds usually sit.
    if (c.c < 2.5 || (c.l < 25 && c.c < 4.5)) {
        if (c.l < 25)
            return c.c >= 1.5 ? "near-black " + family(c) : "near-black";
        if (c.l > 96)
            return "near-white";
        if (c.l > 88)
            return "off-white";
        if (c.l < 40)
            return "charcoal";
        if (c.l < 62)
            return "dark grey";
        if (c.l < 80)
            return "grey";
        return "light grey";
    }

    const QString name = family(c);
    QStringList words;
    // Very light colours are "pale" when washed out and "bright" when
    // saturated; "bright" already says what "vivid" would.
    const bool bright = c.l > 84 && c.c >= 10;
    if (!impliesLightness(name)) {
        const char *lightness = bright ? "bright"
                              : c.l > 84 ? "pale"
                              : c.l > 72 ? "light"
                              : c.l < 35 ? "deep"
                              : c.l < 50 ? "dark"
                                         : "";
        if (*lightness)
            words << lightness;
    }
    if (c.c < 6)
        words << "muted";
    else if (c.c > 18 && !(bright && !impliesLightness(name)))
        words << "vivid";
    words << name;
    return words.join(' ');
}

QString Palette::moodFor(const QString &mode) {
    return mode == "light" ? "bright airy daylight, high-key lighting, soft shadows"
                           : "night, low-key lighting, deep shadows";
}

Palette Palette::fromToml(const QString &text) {
    Palette palette;
    QHash<QString, QString> values;
    static const QRegularExpression line(R"re(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*["']([^"']*)["']\s*(#.*)?$)re");
    for (const QString &raw : text.split('\n')) {
        const auto match = line.match(raw);
        if (match.hasMatch())
            values.insert(match.captured(1), match.captured(2).trimmed());
    }

    Rgb rgb;
    for (const char *role : {"background", "foreground", "accent"}) {
        const QString hex = values.value(role).toLower();
        if (!parseHex(hex, &rgb)) {
            palette.error = QCoreApplication::translate("Palette", "colors.toml has no valid %1 colour").arg(role);
            return palette;
        }
        PaletteColor colour{role, hex, describe(hex)};
        cielab(rgb, colour.lab);
        palette.colors << colour;
    }

    const QString mode = values.value("mode");
    if (mode == "dark" || mode == "light")
        palette.mode = mode;
    else
        palette.mode = palette.colors[0].lab[0] < 50 ? "dark" : "light";
    palette.mood = moodFor(palette.mode);

    // The two most saturated named colours carry the theme's character; a
    // colour already in the palette is not counted twice.
    struct Candidate {
        QString role, hex;
        double chroma;
    };
    QList<Candidate> candidates;
    for (const char *role : namedColours) {
        const QString hex = values.value(role).toLower();
        if (parseHex(hex, &rgb))
            candidates << Candidate{role, hex, oklch(rgb).c};
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.chroma > b.chroma; });
    for (const auto &candidate : candidates) {
        if (palette.colors.size() >= 5)
            break;
        const bool seen = std::any_of(palette.colors.begin(), palette.colors.end(),
                                      [&](const PaletteColor &c) { return c.hex == candidate.hex; });
        if (seen)
            continue;
        PaletteColor colour{candidate.role, candidate.hex, describe(candidate.hex)};
        parseHex(candidate.hex, &rgb);
        cielab(rgb, colour.lab);
        palette.colors << colour;
    }

    for (const auto &colour : palette.colors)
        if (!palette.words.contains(colour.words))
            palette.words << colour.words;
    return palette;
}
