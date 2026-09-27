#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>
#include <QStyleHints>
#include <cmath>
#include <utility>

namespace {
double luminance(const QColor &color) {
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}
QColor blend(const QColor &a, const QColor &b, double amount) {
    return QColor(qRound(a.red() + (b.red() - a.red()) * amount),
                  qRound(a.green() + (b.green() - a.green()) * amount),
                  qRound(a.blue() + (b.blue() - a.blue()) * amount));
}
}

bool Theme::operator==(const Theme &o) const {
    return background == o.background && foreground == o.foreground && muted == o.muted &&
           accent == o.accent && selection == o.selection && selectedText == o.selectedText && surface == o.surface;
}

double Theme::contrast(const QColor &a, const QColor &b) {
    const double first = luminance(a), second = luminance(b);
    return (qMax(first, second) + 0.05) / (qMin(first, second) + 0.05);
}

QColor Theme::readable(const QColor &color, const QColor &background, double minimum) {
    if (contrast(color, background) >= minimum)
        return color;
    const QColor target = contrast(Qt::white, background) > contrast(Qt::black, background) ? Qt::white : Qt::black;
    // Preserve the theme's hue as far as possible while meeting the contrast floor.
    for (int step = 1; step <= 100; ++step) {
        const QColor candidate = blend(color, target, step / 100.0).toRgb();
        if (contrast(candidate, background) >= minimum)
            return candidate;
    }
    return target;
}

Theme Theme::fallback(bool dark) {
    Theme theme;
    theme.background = QColor(dark ? "#202124" : "#faf9f6");
    theme.foreground = QColor(dark ? "#dddcd7" : "#30322e");
    theme.muted = readable(QColor(dark ? "#858780" : "#7a7c75"), theme.background);
    theme.accent = readable(QColor(dark ? "#b2c48d" : "#596a36"), theme.background);
    theme.selection = QColor(dark ? "#41473a" : "#dbe2cd");
    theme.selectedText = readable(theme.foreground, theme.selection);
    theme.surface = blend(theme.background, theme.foreground, 0.055);
    return theme;
}

Theme Theme::fromToml(const QByteArray &bytes, const Theme &fallback) {
    QHash<QString, QColor> colors;
    // Omarchy's palette uses flat, quoted hexadecimal colors. Ignore comments and sections.
    static const QRegularExpression colorLine(
        R"(^\s*([A-Za-z_][A-Za-z_0-9]*)\s*=\s*["'](#[0-9A-Fa-f]{6})["']\s*(?:#.*)?$)");
    bool inSection = false;
    for (const QString &line : QString::fromUtf8(bytes).split('\n')) {
        if (line.trimmed().startsWith('['))
            inSection = true;
        const auto match = colorLine.match(line);
        if (!inSection && match.hasMatch())
            colors.insert(match.captured(1), QColor(match.captured(2)));
    }
    if (!colors.contains("background") || !colors.contains("foreground"))
        return fallback;
    Theme theme;
    theme.background = colors.value("background");
    theme.foreground = readable(colors.value("foreground"), theme.background);
    theme.muted = readable(colors.value("muted", blend(theme.foreground, theme.background, 0.45)), theme.background);
    theme.accent = readable(colors.value("accent", theme.foreground), theme.background);
    theme.selection = colors.value("selection", blend(theme.background, theme.accent, 0.3));
    if (contrast(theme.selection, theme.background) < 1.2)
        theme.selection = blend(theme.background, theme.foreground, 0.22);
    theme.selectedText = readable(theme.foreground, theme.selection);
    theme.surface = blend(theme.background, theme.foreground, 0.055);
    return theme;
}

QStringList ThemeWatcher::defaultPaths() {
    const QString home = QDir::homePath();
    const QString state = qEnvironmentVariable("XDG_STATE_HOME", home + "/.local/state");
    const QString config = qEnvironmentVariable("XDG_CONFIG_HOME", home + "/.config");
    QStringList paths{state + "/omarchy/current/theme/colors.toml",
                      home + "/.local/state/omarchy/current/theme/colors.toml",
                      config + "/omarchy/current/theme/colors.toml"};
    paths.removeDuplicates();
    return paths;
}

ThemeWatcher::ThemeWatcher(QStringList paths, QObject *parent)
    : QObject(parent), m_paths(paths.isEmpty() ? defaultPaths() : std::move(paths)), m_theme(Theme::fallback()) {
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(100);
    connect(&m_reloadTimer, &QTimer::timeout, this, &ThemeWatcher::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadTimer.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reloadTimer.start(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &ThemeWatcher::reload);
#endif
    reload();
}

void ThemeWatcher::rearm() {
    QSet<QString> wanted;
    for (const QString &path : m_paths) {
        QString candidate = path;
        // Watch parents too: theme switches replace the directory, invalidating file watches.
        for (int level = 0; level < 5; ++level) {
            if (QFileInfo::exists(candidate))
                wanted.insert(candidate);
            const QString parent = QFileInfo(candidate).absolutePath();
            if (parent == candidate)
                break;
            candidate = parent;
        }
    }
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    // A watched name can still refer to a renamed inode until its queued event is handled.
    // Reattach even unchanged names so replacements are always watched at their new inode.
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    for (const QString &path : wanted)
        m_watcher.addPath(path);
}

void ThemeWatcher::reload() {
    bool dark = QGuiApplication::palette().color(QPalette::Window).lightnessF() < 0.5;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown)
        dark = scheme == Qt::ColorScheme::Dark;
#endif
    Theme next = Theme::fallback(dark);
    for (const QString &path : m_paths) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            next = Theme::fromToml(file.readAll(), next);
            break;
        }
    }
    rearm();
    if (!(next == m_theme)) {
        m_theme = next;
        emit changed(m_theme);
    }
}
