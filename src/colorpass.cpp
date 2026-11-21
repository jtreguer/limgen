#include "colorpass.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

// D65 white, for the XYZ to Lab step.
constexpr double whiteX = 0.95047, whiteY = 1.0, whiteZ = 1.08883;
constexpr double delta = 6.0 / 29.0;

const std::array<double, 256> &linearTable() {
    static const std::array<double, 256> table = [] {
        std::array<double, 256> values{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            values[i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return values;
    }();
    return table;
}

double f(double t) { return t > delta * delta * delta ? std::cbrt(t) : t / (3 * delta * delta) + 4.0 / 29.0; }
double fInverse(double t) { return t > delta ? t * t * t : 3 * delta * delta * (t - 4.0 / 29.0); }

int toByte(double linear) {
    linear = std::clamp(linear, 0.0, 1.0);
    const double c = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
    return int(std::lround(c * 255));
}

// Sums for a mean and a standard deviation, per channel.
struct Sums {
    double sum[3] = {0, 0, 0};
    double squares[3] = {0, 0, 0};
    qint64 count = 0;

    void add(const double lab[3]) {
        for (int c = 0; c < 3; ++c) {
            sum[c] += lab[c];
            squares[c] += lab[c] * lab[c];
        }
        ++count;
    }
    LabStats stats() const {
        LabStats stats;
        if (count == 0)
            return stats;
        for (int c = 0; c < 3; ++c) {
            stats.mean[c] = sum[c] / count;
            stats.deviation[c] = std::sqrt(std::max(0.0, squares[c] / count - stats.mean[c] * stats.mean[c]));
        }
        stats.valid = true;
        return stats;
    }
};

} // namespace

namespace ColorPass {

void toLab(QRgb rgb, double lab[3]) {
    const auto &linear = linearTable();
    const double r = linear[qRed(rgb)], g = linear[qGreen(rgb)], b = linear[qBlue(rgb)];
    const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / whiteX;
    const double y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b) / whiteY;
    const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / whiteZ;
    const double fx = f(x), fy = f(y), fz = f(z);
    lab[0] = 116 * fy - 16;
    lab[1] = 500 * (fx - fy);
    lab[2] = 200 * (fy - fz);
}

QRgb fromLab(const double lab[3]) {
    const double fy = (lab[0] + 16) / 116;
    const double x = whiteX * fInverse(fy + lab[1] / 500);
    const double y = whiteY * fInverse(fy);
    const double z = whiteZ * fInverse(fy - lab[2] / 200);
    const double r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    const double g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    const double b = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
    return qRgb(toByte(r), toByte(g), toByte(b));
}

LabStats statistics(const QList<QImage> &images) {
    Sums sums;
    for (const QImage &image : images) {
        if (image.isNull())
            continue;
        const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
        const int step = std::max(1, std::max(rgb.width(), rgb.height()) / 256);
        for (int y = step / 2; y < rgb.height(); y += step) {
            const QRgb *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
            for (int x = step / 2; x < rgb.width(); x += step) {
                double lab[3];
                toLab(line[x], lab);
                sums.add(lab);
            }
        }
    }
    return sums.stats();
}

LabStats statistics(const QList<QColor> &palette) {
    Sums sums;
    for (const QColor &colour : palette) {
        if (!colour.isValid())
            continue;
        double lab[3];
        toLab(colour.rgb(), lab);
        sums.add(lab);
    }
    LabStats stats = sums.stats();
    stats.lightness = false;
    stats.spread = false;
    return stats;
}

QImage apply(const QImage &image, const LabStats &target, double strength) {
    strength = std::clamp(strength, 0.0, 1.0);
    if (image.isNull() || !target.valid || strength <= 0)
        return image;
    const LabStats source = statistics({image});
    if (!source.valid)
        return image;

    // Per channel, the transfer and the blend together are one straight
    // line: out = scale * value + shift.
    double scale[3] = {1, 1, 1}, shift[3] = {0, 0, 0};
    for (int c = 0; c < 3; ++c) {
        if (c == 0 && !target.lightness)
            continue;
        double ratio = 1;
        if (target.spread && source.deviation[c] > 1e-6 && target.deviation[c] > 1e-6)
            ratio = std::clamp(target.deviation[c] / source.deviation[c], 0.5, 2.0);
        scale[c] = 1 - strength + strength * ratio;
        shift[c] = strength * (target.mean[c] - source.mean[c] * ratio);
    }

    QImage result = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < result.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            double lab[3];
            toLab(line[x], lab);
            for (int c = 0; c < 3; ++c)
                lab[c] = scale[c] * lab[c] + shift[c];
            lab[0] = std::clamp(lab[0], 0.0, 100.0);
            const QRgb moved = fromLab(lab);
            line[x] = qRgba(qRed(moved), qGreen(moved), qBlue(moved), qAlpha(line[x]));
        }
    }
    return image.hasAlphaChannel() ? result : result.convertToFormat(QImage::Format_RGB32);
}

} // namespace ColorPass
