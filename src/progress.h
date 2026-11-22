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
    // With steps 0, a line counts whatever its total says: an upscale counts
    // tiles, and how many depends on the image and the tile size.
    ProgressParser(int images, int steps) : m_images(images), m_steps(steps) {}

    QList<ProgressEvent> feed(const QByteArray &chunk);
    // Parses whatever is left once the process has ended.
    QList<ProgressEvent> finish();

    int sampled() const { return m_sampled; }
    // The sampling steps per image: as constructed, or as an image-to-image
    // run announced them.
    int steps() const { return m_steps; }

private:
    void parseLine(const QString &line, QList<ProgressEvent> &events);

    int m_images, m_steps;
    int m_sampled = 0;
    // Image to image encodes its source between the t_enc line and
    // "encode_first_stage completed"; with tiling, that prints tile lines
    // in the same format as steps, which are not steps.
    bool m_encoding = false;
    QByteArray m_pending;
};
