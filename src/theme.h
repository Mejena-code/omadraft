#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

struct Theme {
    QColor background;
    QColor foreground;
    QColor muted;
    QColor accent;
    QColor selection;
    QColor selectedText;
    QColor surface;
    bool operator==(const Theme &other) const;
    static Theme fallback(bool dark = true);
    static Theme fromToml(const QByteArray &bytes, const Theme &fallback);
    static double contrast(const QColor &a, const QColor &b);
    static QColor readable(const QColor &color, const QColor &background, double minimum = 4.5);
};

class ThemeWatcher : public QObject {
    Q_OBJECT
public:
    explicit ThemeWatcher(QStringList paths = {}, QObject *parent = nullptr);
    Theme theme() const { return m_theme; }
    static QStringList defaultPaths();
public slots:
    void reload();
signals:
    void changed(const Theme &theme);
private:
    void rearm();
    QStringList m_paths;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    Theme m_theme;
};
