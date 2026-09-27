#pragma once

#include "session.h"
#include "theme.h"
#include <QDialog>
#include <QMainWindow>
#include <QTabBar>
#include <QTimer>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class MarkdownEditor;

class NoteTabs : public QTabBar {
public:
    explicit NoteTabs(QWidget *parent = nullptr);
    void setTheme(const Theme &theme);
protected:
    QSize tabSizeHint(int index) const override;
    void paintEvent(QPaintEvent *event) override;
private:
    Theme m_theme;
};

class DiscardDialog : public QDialog {
    Q_OBJECT
public:
    explicit DiscardDialog(const Theme &theme, QWidget *parent = nullptr);
    void setTheme(const Theme &theme);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
private:
    bool handleKey(QKeyEvent *event);
    QPushButton *m_yes;
    QPushButton *m_no;
};

class HelpDialog : public QDialog {
    Q_OBJECT
public:
    explicit HelpDialog(const Theme &theme, QWidget *parent = nullptr);
    void setTheme(const Theme &theme);
signals:
    void syncRequested();
    void updatesRequested();
};

class Window : public QMainWindow {
    Q_OBJECT
public:
    Window(SessionStore &store, const Session &session, ThemeWatcher &theme, QWidget *parent = nullptr);
    Session snapshot() const;
    bool saveNow();
public slots:
    void newNote();
    void switchNote(int offset);
    void discardNote();
    void showHelp();
    void showSyncSetup();
    void focusNote();
signals:
    void restartRequested();
protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void applySession(const Session &session);
    void appendEditor(const Note &note);
    void setActive(int index);
    void scheduleSave();
    void applyTheme(const Theme &theme);
    void updateHint();
    SessionStore &m_store;
    Theme m_theme;
    NoteTabs *m_tabs;
    QStackedWidget *m_stack;
    QLabel *m_hint;
    QLabel *m_error;
    QVector<QString> m_ids;
    QVector<MarkdownEditor *> m_editors;
    QTimer m_saveTimer;
    QTimer m_syncTimer;
    bool m_loading = true;
    bool m_discarding = false;
    bool m_helpOpen = false;
    QString m_lastSaveError;
};
