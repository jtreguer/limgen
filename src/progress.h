#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// What sd-cli's output says about a batch, one event per fact.
struct ProgressEvent {
    enum Kind {
        Step,    // a sampling step of `image` finished
        Sampled, // `image` finished sampling
        Decoded, // `image` was decoded to pixels
        Saved,   // `image` was written to `text`
        Error,   // an "[E]" line; `text` holds it
    };
    Kind kind = Step;
    int image = 0; // 0-based index in the batch
    int step = 0, steps = 0;
    double secondsPerStep = 0;
    QString text;
};

// Turns sd-cli's output into events, fed in whatever chunks the process
// delivers. Pure: it keeps only the unfinished last line between calls.
//
// Progress is redrawn on one terminal line with carriage returns and an
// erase-line sequence, and the same printer also counts VAE tiles. With a
// batch, sd-cli samples every image before it decodes any, so a progress
// line is a sampling step only while some image is still unsampled.
class ProgressParser {
public:
    ProgressParser(int images, int steps) : m_images(images), m_steps(steps) {}

    QList<ProgressEvent> feed(const QByteArray &chunk);
    // Parses whatever is left once the process has ended.
    QList<ProgressEvent> finish();

    int sampled() const { return m_sampled; }

private:
    void parseLine(const QString &line, QList<ProgressEvent> &events);

    int m_images, m_steps;
    int m_sampled = 0;
    QByteArray m_pending;
};
