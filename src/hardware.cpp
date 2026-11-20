#include "hardware.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace Hardware {

namespace {

QString readTrimmed(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

} // namespace

Sources Sources::fromEnvironment() {
    Sources sources;
    sources.nvidiaSmi = qEnvironmentVariable("LIMGEN_NVIDIA_SMI");
    if (sources.nvidiaSmi.isEmpty())
        sources.nvidiaSmi = QStandardPaths::findExecutable("nvidia-smi");
    sources.drmRoot = qEnvironmentVariable("LIMGEN_DRM_ROOT");
    if (sources.drmRoot.isEmpty())
        sources.drmRoot = "/sys/class/drm";
    return sources;
}

QList<Gpu> parseNvidiaSmi(const QByteArray &csv) {
    QList<Gpu> gpus;
    for (const QString &line : QString::fromUtf8(csv).split('\n', Qt::SkipEmptyParts)) {
        // The name may itself contain commas, so the two numbers are taken
        // from the end.
        const QStringList fields = line.split(',');
        if (fields.size() < 3)
            continue;
        bool totalOk = false, usedOk = false;
        const qint64 total = fields.at(fields.size() - 2).trimmed().toLongLong(&totalOk);
        const qint64 used = fields.last().trimmed().toLongLong(&usedOk);
        if (!totalOk || !usedOk || total <= 0 || used < 0)
            continue;
        const QString name = fields.mid(0, fields.size() - 2).join(',').trimmed();
        gpus << Gpu{"nvidia", name, total, qMax<qint64>(0, total - used)};
    }
    return gpus;
}

QList<Gpu> readDrm(const QString &root) {
    QList<Gpu> gpus;
    static const QRegularExpression card(QRegularExpression::anchoredPattern("card[0-9]+"));
    const auto entries = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name);
    for (const QString &entry : entries) {
        if (!card.match(entry).hasMatch())
            continue;
        const QString device = root + "/" + entry + "/device";
        const QString vendor = readTrimmed(device + "/vendor").toLower();
        if (vendor == "0x1002") {
            bool totalOk = false, usedOk = false;
            const qint64 total = readTrimmed(device + "/mem_info_vram_total").toLongLong(&totalOk);
            const qint64 used = readTrimmed(device + "/mem_info_vram_used").toLongLong(&usedOk);
            Gpu gpu{"amd", readTrimmed(device + "/product_name"), -1, -1};
            if (gpu.name.isEmpty())
                gpu.name = "AMD GPU";
            // An APU's dedicated memory is a small carve-out, and the GPU
            // also uses shared memory, so under 2 GiB the figure says
            // nothing about what fits and is left unknown.
            if (totalOk && usedOk && total >= qint64(2048) * 1024 * 1024) {
                gpu.totalMiB = total / (1024 * 1024);
                gpu.freeMiB = qMax<qint64>(0, total - used) / (1024 * 1024);
            }
            gpus << gpu;
        } else if (vendor == "0x8086") {
            gpus << Gpu{"intel", "Intel GPU", -1, -1};
        }
    }
    return gpus;
}

QList<Gpu> detect(const Sources &sources) {
    QList<Gpu> gpus;
    if (!sources.nvidiaSmi.isEmpty()) {
        QProcess process;
        process.start(sources.nvidiaSmi,
                      {"--query-gpu=name,memory.total,memory.used", "--format=csv,noheader,nounits"});
        if (process.waitForFinished(3000) && process.exitStatus() == QProcess::NormalExit &&
            process.exitCode() == 0)
            gpus << parseNvidiaSmi(process.readAllStandardOutput());
        else {
            process.kill();
            process.waitForFinished(1000);
        }
    }
    gpus << readDrm(sources.drmRoot);
    return gpus;
}

Gpu matching(const QList<Gpu> &gpus, const QString &deviceDescription) {
    for (const Gpu &gpu : gpus)
        if (!gpu.name.isEmpty() && deviceDescription.contains(gpu.name, Qt::CaseInsensitive))
            return gpu;
    // With several GPUs and no match, which one the engine uses is unknown,
    // and another card's memory must not stand in for it.
    return gpus.size() == 1 ? gpus.first() : Gpu{};
}

} // namespace Hardware
