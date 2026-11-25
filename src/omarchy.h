#pragma once

#include <QFileSystemWatcher>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>

// Where limgen finds Omarchy and the tools it calls. Every path has an
// environment override so tests and experiments run against temp folders,
// never the user's real theme (docs/technical.md section 11).
struct OmarchyPaths {
    QString state;       // LIMGEN_OMARCHY_STATE, default $XDG_STATE_HOME/omarchy/current
    QString backgrounds; // LIMGEN_OMARCHY_BACKGROUNDS, default $XDG_CONFIG_HOME/omarchy/backgrounds
    QString omarchy;     // LIMGEN_OMARCHY_BIN, default `omarchy` on PATH
    QString hyprctl;     // LIMGEN_HYPRCTL, default `hyprctl` on PATH when Hyprland runs
    QString language;    // $XDG_CONFIG_HOME/omarchy/language
    // LIMGEN_OMARCHY_THEMES (folders separated by colons), default
    // $XDG_CONFIG_HOME/omarchy/themes (the user's) then
    // /usr/share/omarchy/themes (stock), searched in that order.
    QStringList themes;
    // LIMGEN_PICTURES, default ~/Pictures/limgen: where kept images go when
    // there is no Omarchy theme.
    QString pictures;

    static OmarchyPaths fromEnvironment();
};

struct Monitor {
    QString name;
    int width = 0;  // in pixels, as the monitor is mounted (a rotated monitor is taller than wide)
    int height = 0;
};

// The current Omarchy theme: its name, colours and backgrounds, and the
// monitors limgen makes backgrounds for. Reads only; the one place limgen
// writes, the user background folder, is handled where images are kept.
class Omarchy : public QObject {
    Q_OBJECT

public:
    explicit Omarchy(const OmarchyPaths &paths = OmarchyPaths::fromEnvironment(), QObject *parent = nullptr);

    const OmarchyPaths &paths() const { return m_paths; }

    // True when the state folder names a theme. Without it limgen still runs,
    // with the default accent and no palette (docs/design.md section 10).
    bool present() const { return !slug().isEmpty(); }

    // The theme's slug from theme.name, or an empty string when there is
    // none or it is not a safe folder name (it becomes part of a path that
    // limgen writes to).
    QString slug() const;
    QString colorsPath() const;
    // The text of colors.toml; on failure an empty string and *error set.
    QString readColors(QString *error = nullptr) const;

    // Images in the current theme's backgrounds folder, sorted by name.
    QStringList themeBackgrounds() const;
    // ~/.config/omarchy/backgrounds/<slug>, the folder kept images go to, or
    // an empty string when there is no theme.
    QString userBackgroundDirectory() const;
    // Images already in that folder, sorted by name.
    QStringList userBackgrounds() const;

    // The folder of theme `slug`: for the current theme, Omarchy's copy in
    // the state folder; otherwise the first theme folder that has it. Empty
    // when there is none.
    QString themeDirectory(const QString &slug) const;
    // Every theme with a colors.toml in the theme folders, and the current
    // one, each once, sorted by name. For the Generic screen's palettes.
    QStringList themeSlugs() const;
    // The images in that theme's backgrounds folder, sorted by name.
    QStringList themeBackgrounds(const QString &slug) const;
    // Where kept images go: ~/.config/omarchy/backgrounds/<slug>, or the
    // pictures folder for an empty slug (no Omarchy theme). Empty for a slug
    // that is not a safe folder name.
    QString keepDirectory(const QString &slug) const;

    // The file the desktop shows now (Omarchy's `background` link in the
    // state folder), or an empty string.
    QString currentBackground() const;
    // `omarchy theme bg set <file>`, the one way limgen changes the
    // background. Returns false with *error set when it fails or Omarchy is
    // not installed.
    bool setBackground(const QString &file, QString *error = nullptr) const;

    // Monitors from `hyprctl monitors -j`; empty when hyprctl is missing,
    // fails, or takes longer than two seconds.
    QList<Monitor> monitors() const;
    static QList<Monitor> parseMonitors(const QByteArray &json);
    // The monitor with the most pixels; the final size and ratio follow it.
    static Monitor largest(const QList<Monitor> &monitors);
    // That monitor's size, or 2560x1440 without a monitor to measure (no
    // Hyprland, or a command run over SSH), the size L0 measured the finish
    // at.
    QSize largestMonitorSize() const;

    static bool isSafeSlug(const QString &slug);

signals:
    // The theme was switched or its files changed. Fired once per change,
    // after the files have settled.
    void changed();

private:
    void rearm();

    OmarchyPaths m_paths;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
