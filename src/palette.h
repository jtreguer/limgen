#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// One colour of a theme palette, with the words an image model is given for
// it and its CIELAB value for the colour pass.
struct PaletteColor {
    QString role;  // "background", "foreground", "accent", or a named colour such as "red"
    QString hex;   // "#rrggbb", lower case
    QString words; // "near-black indigo", "muted lavender", ...
    double lab[3] = {0, 0, 0}; // CIELAB L, a, b under D65
};

// A theme palette read from Omarchy's colors.toml, turned into words a model
// understands. Pure: it parses text it is given and does no I/O.
//
// The words are always English, since they go into the prompt, and image
// models are trained on English captions; they are not translated with the UI.
struct Palette {
    QString mode;                // "dark" or "light"
    QList<PaletteColor> colors;  // background, foreground, accent, then the two most saturated named colours
    QStringList words;           // the colours' words, duplicates removed, in that order
    QString mood;                // words for the mode
    QString error;               // why the text could not be read; empty when it was

    bool isValid() const { return error.isEmpty(); }

    // Parses the flat `key = "value"` lines of a colors.toml. Unknown keys are
    // ignored; a missing background, foreground or accent is an error.
    static Palette fromToml(const QString &text);

    // A palette chosen by hand, for the Generic screen and limgen image: up
    // to five "#rrggbb" colours, in the order given, with roles "colour 1"
    // to "colour 5". It has no mode and no mood, since a handful of colours
    // does not say whether the image should be dark or light. An empty list
    // or a colour that is not "#rrggbb" is an error.
    static constexpr int maxColors = 5;
    static Palette fromColors(const QStringList &hexes);

    // The words for one colour, for example "deep muted teal". Returns an
    // empty string for anything that is not "#rrggbb".
    static QString describe(const QString &hex);

    static QString moodFor(const QString &mode);
};
