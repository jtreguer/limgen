#pragma once

#include <QColor>
#include <QImage>
#include <QList>

// The colour statistics a colour pass moves an image toward: the mean and
// standard deviation of each CIELAB channel (L, a, b).
struct LabStats {
    double mean[3] = {0, 0, 0};
    double deviation[3] = {0, 0, 0};
    // A palette is a handful of colours, not a picture: its lightness and its
    // spread say little about how an image of the theme looks, so a target
    // made from one moves only the colour cast (the a and b means).
    bool lightness = true;
    bool spread = true;
    bool valid = false;
};

// The colour pass (docs/design.md section 9, layer 3): Reinhard's colour
// transfer in CIELAB, blended at a strength. Pure: it works on images and
// colours it is given and does no I/O.
namespace ColorPass {

// CIELAB under D65, from sRGB and back; out-of-gamut colours are clipped.
void toLab(QRgb rgb, double lab[3]);
QRgb fromLab(const double lab[3]);

// The statistics of every pixel of the images together. Large images are
// sampled on a grid of about 256 pixels across, which gives the same
// figures far faster.
LabStats statistics(const QList<QImage> &images);
// The statistics of a theme palette's colours, equally weighted: the colour
// cast only (see LabStats).
LabStats statistics(const QList<QColor> &palette);

// Moves each channel's mean and spread toward the target, then blends the
// result with the image: 0 leaves it as it is, 1 is the full transfer. The
// spread is scaled by at most a factor of two either way, so a flat image
// (or a flat target) is not blown up.
QImage apply(const QImage &image, const LabStats &target, double strength);

} // namespace ColorPass
