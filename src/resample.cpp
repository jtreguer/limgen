#include "resample.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double pi = 3.14159265358979323846;

double sinc(double x) {
    if (x == 0)
        return 1;
    x *= pi;
    return std::sin(x) / x;
}

double lanczos3(double x) {
    x = std::abs(x);
    return x < 3 ? sinc(x) * sinc(x / 3) : 0;
}

// Which input pixels make one output pixel, and how much each counts.
struct Tap {
    int index;
    float weight;
};

std::vector<std::vector<Tap>> taps(int in, int out) {
    const double scale = double(in) / out;
    // Shrinking widens the filter, so every input pixel is counted.
    const double stretch = std::max(1.0, scale);
    const double support = 3 * stretch;
    std::vector<std::vector<Tap>> all(out);
    for (int o = 0; o < out; ++o) {
        const double centre = (o + 0.5) * scale - 0.5;
        const int first = int(std::ceil(centre - support)), last = int(std::floor(centre + support));
        double total = 0;
        for (int i = first; i <= last; ++i) {
            const double weight = lanczos3((i - centre) / stretch);
            if (weight == 0)
                continue;
            // Edge pixels repeat beyond the border.
            all[o].push_back({std::clamp(i, 0, in - 1), float(weight)});
            total += weight;
        }
        for (Tap &tap : all[o])
            tap.weight = float(tap.weight / total);
    }
    return all;
}

int toByte(float value) { return std::clamp(int(std::lround(value)), 0, 255); }

} // namespace

namespace Resample {

QImage lanczos(const QImage &image, const QSize &size) {
    if (image.isNull() || size.isEmpty())
        return {};
    // Premultiplied, so transparent pixels do not bleed their colour.
    const QImage source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int inWidth = source.width(), inHeight = source.height();
    const int outWidth = size.width(), outHeight = size.height();

    // Across first, into a buffer of floats, then down.
    const auto across = taps(inWidth, outWidth);
    std::vector<float> buffer(size_t(inHeight) * outWidth * 4);
    for (int y = 0; y < inHeight; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        float *row = buffer.data() + size_t(y) * outWidth * 4;
        for (int x = 0; x < outWidth; ++x) {
            float a = 0, r = 0, g = 0, b = 0;
            for (const Tap &tap : across[x]) {
                const QRgb pixel = line[tap.index];
                a += tap.weight * qAlpha(pixel);
                r += tap.weight * qRed(pixel);
                g += tap.weight * qGreen(pixel);
                b += tap.weight * qBlue(pixel);
            }
            float *out = row + size_t(x) * 4;
            out[0] = a;
            out[1] = r;
            out[2] = g;
            out[3] = b;
        }
    }

    const auto down = taps(inHeight, outHeight);
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < outHeight; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < outWidth; ++x) {
            float a = 0, r = 0, g = 0, b = 0;
            for (const Tap &tap : down[y]) {
                const float *in = buffer.data() + (size_t(tap.index) * outWidth + x) * 4;
                a += tap.weight * in[0];
                r += tap.weight * in[1];
                g += tap.weight * in[2];
                b += tap.weight * in[3];
            }
            // The filter rings a little past the edges it sharpens; a
            // premultiplied colour may not exceed its alpha.
            const int alpha = toByte(a);
            line[x] = qRgba(std::min(toByte(r), alpha), std::min(toByte(g), alpha), std::min(toByte(b), alpha), alpha);
        }
    }
    return result.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
}

QImage cover(const QImage &image, const QSize &size) {
    if (image.isNull() || size.isEmpty())
        return {};
    const double scale = std::max(double(size.width()) / image.width(), double(size.height()) / image.height());
    const QSize scaled(std::max(size.width(), int(std::lround(image.width() * scale))),
                       std::max(size.height(), int(std::lround(image.height() * scale))));
    const QImage resized = scaled == image.size() ? image : lanczos(image, scaled);
    return resized.copy((scaled.width() - size.width()) / 2, (scaled.height() - size.height()) / 2, size.width(),
                        size.height());
}

} // namespace Resample
