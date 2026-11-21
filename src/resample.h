#pragma once

#include <QImage>
#include <QSize>

// Resizing for the finish (docs/technical.md section 5, step 4). Pure: it
// works on images it is given and does no I/O.
namespace Resample {

// Resizes to exactly `size` with a Lanczos-3 filter, which keeps the edges
// ESRGAN sharpened where Qt's smooth scaling would soften them. The ratio is
// not kept.
QImage lanczos(const QImage &image, const QSize &size);

// Scales the image, keeping its ratio, until it covers `size`, then crops
// the centre to exactly `size`.
QImage cover(const QImage &image, const QSize &size);

} // namespace Resample
