#pragma once

#include <QJsonObject>
#include <QString>

// How long batches take on this machine, per model, level and backend, kept
// in ~/.local/state/limgen/bench.json so the window can say how long the
// next batch will take (docs/design.md section 5).
struct Timing {
    // Everything a batch spends besides its steps and decoding: starting
    // the engine, loading weights, encoding the prompt, and the slow first
    // step that copies the weights to the GPU.
    double overheadSeconds = -1;
    double secondsPerStep = -1;
    double decodeSecondsPerImage = -1;

    bool isKnown() const { return overheadSeconds >= 0 && secondsPerStep > 0 && decodeSecondsPerImage >= 0; }
    // The whole batch, or -1 when not known yet.
    double estimate(int count, int steps) const;
};

class Timings {
public:
    explicit Timings(const QString &file = defaultFile());

    // $XDG_STATE_HOME/limgen/bench.json.
    static QString defaultFile();
    static QString key(const QString &model, const QString &level, const QString &backend);

    Timing get(const QString &key) const;
    // Blends a new measurement into what is stored (an even mix with the
    // last value, so it follows a changed machine within a few batches) and
    // saves the file. A measurement with unknown parts leaves those parts.
    bool record(const QString &key, const Timing &measured);

private:
    QString m_file;
    QJsonObject m_values;
};
