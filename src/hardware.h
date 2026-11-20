#pragma once

#include <QList>
#include <QString>

// A GPU and its video memory. Memory figures are -1 when unknown, as on
// Intel GPUs, which share system memory.
struct Gpu {
    QString vendor; // "nvidia", "amd" or "intel"
    QString name;
    qint64 totalMiB = -1;
    qint64 freeMiB = -1;
};

// Finds the GPUs and how much video memory is free right now. Free memory
// is read before every batch, since other programs hold some of it.
namespace Hardware {

// Where the readings come from; overridable so tests use fakes.
struct Sources {
    QString nvidiaSmi; // LIMGEN_NVIDIA_SMI, default `nvidia-smi` on PATH
    QString drmRoot;   // LIMGEN_DRM_ROOT, default /sys/class/drm

    static Sources fromEnvironment();
};

QList<Gpu> detect(const Sources &sources = Sources::fromEnvironment());

// `nvidia-smi --query-gpu=name,memory.total,memory.used --format=csv,noheader,nounits`
QList<Gpu> parseNvidiaSmi(const QByteArray &csv);
// AMD and Intel cards under a /sys/class/drm-like folder. NVIDIA cards are
// skipped there; nvidia-smi reports them with their memory.
QList<Gpu> readDrm(const QString &root);

// The GPU whose name appears in the engine's device description, or the only
// GPU there is; otherwise an empty Gpu, whose memory is unknown.
Gpu matching(const QList<Gpu> &gpus, const QString &deviceDescription);

} // namespace Hardware
