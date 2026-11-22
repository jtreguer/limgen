#include "progress.h"

#include <QRegularExpression>

QList<ProgressEvent> ProgressParser::feed(const QByteArray &chunk) {
    m_pending += chunk;
    QList<ProgressEvent> events;
    // A line ends at a newline or at a carriage return (a progress redraw).
    // The last piece may be unfinished, so it waits for the next chunk.
    qsizetype start = 0;
    for (qsizetype i = 0; i < m_pending.size(); ++i) {
        const char c = m_pending.at(i);
        if (c == '\n' || c == '\r') {
            parseLine(QString::fromUtf8(m_pending.mid(start, i - start)), events);
            start = i + 1;
        }
    }
    m_pending.remove(0, start);
    return events;
}

QList<ProgressEvent> ProgressParser::finish() {
    QList<ProgressEvent> events;
    if (!m_pending.isEmpty())
        parseLine(QString::fromUtf8(m_pending), events);
    m_pending.clear();
    return events;
}

void ProgressParser::parseLine(const QString &raw, QList<ProgressEvent> &events) {
    static const QRegularExpression ansi("\x1b\\[[0-9;]*[A-Za-z]");
    const QString line = QString(raw).remove(ansi).trimmed();
    if (line.isEmpty())
        return;

    static const QRegularExpression step(R"(\|\s*(\d+)/(\d+)\s*-\s*([0-9.]+)\s*(s/it|it/s))");
    static const QRegularExpression decoded(R"(\[I\] latent (\d+) decoded)");
    static const QRegularExpression saved(R"(\[I\] save result image (\d+) to '(.*)' \(success\))");
    static const QRegularExpression target(R"(\[I\] target t_enc is (\d+) steps)");

    // Image to image runs t_enc + 1 sampling steps; the engine's own count
    // wins over the one worked out beforehand.
    if (const auto m = target.match(line); m.hasMatch()) {
        if (m_steps > 0) {
            m_steps = m.captured(1).toInt() + 1;
            m_encoding = true;
        }
        return;
    }
    if (line.startsWith("[I] encode_first_stage completed")) {
        m_encoding = false;
        return;
    }

    if (const auto m = step.match(line); m.hasMatch()) {
        const int n = m.captured(1).toInt(), total = m.captured(2).toInt();
        if (m_encoding || m_sampled >= m_images || (m_steps > 0 && total != m_steps) || n < 1 || n > total)
            return;
        double seconds = m.captured(3).toDouble();
        if (m.captured(4) == "it/s" && seconds > 0)
            seconds = 1.0 / seconds;
        ProgressEvent event;
        event.kind = ProgressEvent::Step;
        event.image = m_sampled;
        event.step = n;
        event.steps = total;
        event.secondsPerStep = seconds;
        events << event;
        return;
    }
    if (line.startsWith("[I] sampling completed")) {
        if (m_sampled < m_images) {
            ProgressEvent event;
            event.kind = ProgressEvent::Sampled;
            event.image = m_sampled++;
            events << event;
        }
        return;
    }
    if (const auto m = decoded.match(line); m.hasMatch()) {
        ProgressEvent event;
        event.kind = ProgressEvent::Decoded;
        event.image = m.captured(1).toInt() - 1;
        events << event;
        return;
    }
    if (const auto m = saved.match(line); m.hasMatch()) {
        ProgressEvent event;
        event.kind = ProgressEvent::Saved;
        event.image = m.captured(1).toInt();
        event.text = m.captured(2);
        events << event;
        return;
    }
    if (line.contains("[E] ")) {
        ProgressEvent event;
        event.kind = ProgressEvent::Error;
        event.text = line.mid(line.indexOf("[E] ") + 4);
        events << event;
    }
}
