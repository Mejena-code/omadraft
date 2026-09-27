#include "window.h"
#include "shortcuts.h"
#include "markdown.h"
#include "sync.h"
#include "update.h"

#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QMimeData>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextBlockFormat>
#include <QTextDocument>
#include <QVBoxLayout>

namespace {
class CopyButton : public QPushButton {
public:
    explicit CopyButton(QWidget *parent) : QPushButton(parent) {}
protected:
    void paintEvent(QPaintEvent *event) override {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate((width() - 18) / 2.0, (height() - 18) / 2.0);
        painter.setPen(QPen(property("ink").value<QColor>(), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        if (property("copied").toBool()) {
            painter.drawLine(QPointF(3, 9), QPointF(7, 13));
            painter.drawLine(QPointF(7, 13), QPointF(15, 5));
        } else {
            painter.drawRoundedRect(QRectF(6, 6, 9, 10), 1.5, 1.5);
            painter.drawLine(QPointF(3, 12), QPointF(2, 12));
            painter.drawLine(QPointF(2, 12), QPointF(2, 2));
            painter.drawLine(QPointF(2, 2), QPointF(11, 2));
            painter.drawLine(QPointF(11, 2), QPointF(11, 3));
        }
    }
};

QString dialogStyle(const Theme &theme) {
    return QStringLiteral(
        "QDialog { background: %1; color: %2; } QLabel { color: %2; }"
        "QLabel#dialogHint, QLabel#helpLabel, QLabel#helpVersion { color: %3; }"
        "QPushButton { background: transparent; color: %3; border: 1px solid transparent; border-radius: 5px; padding: 5px 14px; }"
        "QPushButton:focus, QPushButton:hover { color: %4; border-color: %4; }")
        .arg(theme.background.name(), theme.foreground.name(), theme.muted.name(), theme.accent.name());
}
}

NoteTabs::NoteTabs(QWidget *parent) : QTabBar(parent), m_theme(Theme::fallback()) {
    setObjectName("noteTabs");
    setAccessibleName("Notes");
    setDrawBase(false);
    setExpanding(false);
    setUsesScrollButtons(true);
    setElideMode(Qt::ElideNone);
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(26);
}

void NoteTabs::setTheme(const Theme &theme) {
    m_theme = theme;
    update();
}

QSize NoteTabs::tabSizeHint(int index) const {
    return {qMax(24, fontMetrics().horizontalAdvance(tabText(index)) + 14), 26};
}

void NoteTabs::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < count(); ++i) {
        const QRect area = tabRect(i);
        if (!rect().intersects(area))
            continue;
        QFont labelFont = font();
        labelFont.setWeight(QFont::Normal);
        painter.setFont(labelFont);
        painter.setPen(i == currentIndex() ? m_theme.accent : m_theme.muted);
        painter.drawText(area.adjusted(0, 0, 0, -2), Qt::AlignCenter, tabText(i));
        if (i == currentIndex()) {
            painter.setPen(QPen(m_theme.accent, 1));
            painter.drawLine(area.center().x() - 4, area.bottom() - 3,
                             area.center().x() + 4, area.bottom() - 3);
        }
    }
}

DiscardDialog::DiscardDialog(const Theme &theme, QWidget *parent) : QDialog(parent) {
    setObjectName("discardDialog");
    setWindowTitle("Discard note");
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 28, 32, 24);
    layout->setSpacing(24);
    auto *question = new QLabel("Are you sure you want to dispose of this note?", this);
    question->setWordWrap(true);
    question->setAlignment(Qt::AlignCenter);
    layout->addWidget(question);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(12);
    buttons->addStretch();
    m_yes = new QPushButton("Yes", this);
    m_yes->setObjectName("discardYes");
    m_no = new QPushButton("No", this);
    m_no->setObjectName("discardNo");
    for (auto *button : {m_yes, m_no}) {
        button->setMinimumSize(78, 36);
        button->setAutoDefault(false);
        button->installEventFilter(this);
        buttons->addWidget(button);
    }
    buttons->addStretch();
    layout->addLayout(buttons);
    auto *hint = new QLabel("←/→  Choose     Enter  Confirm     Esc  Cancel", this);
    hint->setObjectName("dialogHint");
    hint->setAlignment(Qt::AlignCenter);
    QFont small = font();
    small.setPointSizeF(qMax(9.0, small.pointSizeF() - 1));
    hint->setFont(small);
    layout->addWidget(hint);
    connect(m_yes, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_no, &QPushButton::clicked, this, &QDialog::reject);
    setTheme(theme);
    resize(480, sizeHint().height());
    m_no->setFocus();
}

void DiscardDialog::setTheme(const Theme &theme) {
    setStyleSheet(dialogStyle(theme));
}

bool DiscardDialog::handleKey(QKeyEvent *event) {
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
        event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
        (m_yes->hasFocus() ? m_no : m_yes)->setFocus();
        return true;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_yes->hasFocus())
            accept();
        else
            reject();
        return true;
    }
    if (event->key() == Qt::Key_Escape) {
        reject();
        return true;
    }
    return false;
}

bool DiscardDialog::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::KeyPress && handleKey(static_cast<QKeyEvent *>(event)))
        return true;
    return QDialog::eventFilter(object, event);
}

void DiscardDialog::keyPressEvent(QKeyEvent *event) {
    if (!handleKey(event))
        QDialog::keyPressEvent(event);
}

CopyDialog::CopyDialog(const Theme &theme, QWidget *parent) : QDialog(parent) {
    setObjectName("copyDialog");
    setWindowTitle("Copy note");
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 28, 32, 24);
    layout->setSpacing(24);
    auto *title = new QLabel("Copy note as…", this);
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto *buttons = new QHBoxLayout;
    m_plain = new QPushButton("Plain text", this);
    m_plain->setObjectName("copyPlain");
    m_rich = new QPushButton("Formatted text", this);
    m_rich->setObjectName("copyFormatted");
    for (auto *button : {m_plain, m_rich}) {
        button->setAutoDefault(false);
        button->installEventFilter(this);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    auto *hint = new QLabel("←/→  Choose     Enter  Copy     Esc  Cancel", this);
    hint->setObjectName("dialogHint");
    hint->setAlignment(Qt::AlignCenter);
    QFont small = font();
    small.setPointSizeF(9.5);
    hint->setFont(small);
    layout->addWidget(hint);
    connect(m_plain, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_rich, &QPushButton::clicked, this, [this] {
        m_formatted = true;
        accept();
    });
    setTheme(theme);
    m_plain->setFocus();
}

void CopyDialog::setTheme(const Theme &theme) {
    setStyleSheet(dialogStyle(theme));
}

bool CopyDialog::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right ||
            key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            (m_plain->hasFocus() ? m_rich : m_plain)->setFocus();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            (m_rich->hasFocus() ? m_rich : m_plain)->click();
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}

HelpDialog::HelpDialog(const Theme &theme, QWidget *parent) : QDialog(parent) {
    setObjectName("helpDialog");
    setWindowTitle("Help");
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 24, 32, 20);
    layout->setSpacing(20);
    auto *title = new QLabel("Markdown basics", this);
    QFont titleFont = font();
    titleFont.setPointSizeF(13);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *examples = new QGridLayout;
    examples->setHorizontalSpacing(28);
    examples->setVerticalSpacing(12);
    const QList<QPair<QString, QString>> entries{
        {"Headings", "# Heading\n## Subheading\n### Smaller heading"},
        {"Bold", "**bold text**"},
        {"Italic", "*italic text*"},
        {"Strikethrough", "~~crossed out~~"},
        {"Bullet list", "- List item"},
        {"Numbered list", "1. List item"},
        {"Quote", "> Quoted text"},
        {"Link", "[link text](https://example.com)"},
        {"Inline code", "`code`"},
        {"Code block", "```\ncode goes here\n```"}
    };
    QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setPointSizeF(10);
    for (int row = 0; row < entries.size(); ++row) {
        auto *label = new QLabel(entries[row].first, this);
        label->setObjectName("helpLabel");
        examples->addWidget(label, row, 0, Qt::AlignTop);
        auto *example = new QLabel(entries[row].second, this);
        example->setTextFormat(Qt::PlainText);
        example->setFont(codeFont);
        example->setTextInteractionFlags(Qt::TextSelectableByMouse);
        example->setFocusPolicy(Qt::NoFocus);
        examples->addWidget(example, row, 1, Qt::AlignTop);
    }
    layout->addLayout(examples);
    auto *syncButton = new QPushButton("Set up sync…", this);
    syncButton->setObjectName("helpSync");
    syncButton->setAutoDefault(false);
    layout->addWidget(syncButton, 0, Qt::AlignHCenter);
    connect(syncButton, &QPushButton::clicked, this, &HelpDialog::syncRequested);
    auto *updateButton = new QPushButton("Check for updates…", this);
    updateButton->setObjectName("helpUpdates");
    updateButton->setAutoDefault(false);
    layout->addWidget(updateButton, 0, Qt::AlignHCenter);
    connect(updateButton, &QPushButton::clicked, this, &HelpDialog::updatesRequested);
    auto *closeButton = new QPushButton("Close", this);
    closeButton->setObjectName("helpClose");
    closeButton->setMinimumSize(78, 36);
    closeButton->setDefault(true);
    layout->addWidget(closeButton, 0, Qt::AlignHCenter);
    auto *hint = new QLabel("Esc / Enter  Close", this);
    hint->setObjectName("dialogHint");
    hint->setAlignment(Qt::AlignCenter);
    QFont hintFont = font();
    hintFont.setPointSizeF(9.5);
    hint->setFont(hintFont);
    layout->addWidget(hint);
    const QString version = QCoreApplication::applicationVersion();
    auto *versionLabel = new QLabel(version.isEmpty() ? "Omadraft" : "Omadraft " + version, this);
    versionLabel->setObjectName("helpVersion");
    versionLabel->setAlignment(Qt::AlignCenter);
    versionLabel->setFont(hintFont);
    layout->addWidget(versionLabel);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    auto *toggle = new QShortcut(QKeySequence("Alt+H"), this);
    toggle->setAutoRepeat(false);
    connect(toggle, &QShortcut::activated, this, &QDialog::accept);
    setTheme(theme);
    closeButton->setFocus();
}

void HelpDialog::setTheme(const Theme &theme) {
    setStyleSheet(dialogStyle(theme));
}

SearchDialog::SearchDialog(const Session &session, const Theme &theme, QWidget *parent)
    : QDialog(parent), m_session(session) {
    setObjectName("searchDialog");
    setWindowTitle("Search");
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(16);
    m_query = new QLineEdit(this);
    m_query->setObjectName("searchQuery");
    m_query->setPlaceholderText("Search all notes…");
    m_query->setAccessibleName("Search all notes");
    layout->addWidget(m_query);
    m_results = new QListWidget(this);
    m_results->setObjectName("searchResults");
    m_results->setAccessibleName("Search results");
    m_results->setWordWrap(true);
    layout->addWidget(m_results, 1);
    m_status = new QLabel(this);
    m_status->setObjectName("dialogHint");
    layout->addWidget(m_status);
    auto *hint = new QLabel("↑/↓  Choose     Enter  Open     Esc  Cancel", this);
    hint->setObjectName("dialogHint");
    QFont small = font();
    small.setPointSizeF(9.5);
    hint->setFont(small);
    layout->addWidget(hint);
    m_query->installEventFilter(this);
    m_results->installEventFilter(this);
    connect(m_query, &QLineEdit::textChanged, this, &SearchDialog::refresh);
    connect(m_results, &QListWidget::itemActivated, this, [this] { accept(); });
    setTheme(theme);
    refresh();
    resize(520, 360);
    m_query->setFocus();
}

void SearchDialog::setTheme(const Theme &theme) {
    setStyleSheet(dialogStyle(theme) + QStringLiteral(
        "QLineEdit, QListWidget { background: %1; color: %2; border: 1px solid %3; border-radius: 4px; }"
        "QLineEdit { padding: 7px; selection-background-color: %4; selection-color: %5; }"
        "QListWidget::item { padding: 8px; }"
        "QListWidget::item:selected { background: %4; color: %5; }")
        .arg(theme.background.name(), theme.foreground.name(), theme.muted.name(),
             theme.selection.name(), theme.selectedText.name()));
}

QString SearchDialog::noteId() const {
    auto *item = m_results->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QString SearchDialog::query() const { return m_query->text(); }

int SearchDialog::matchPosition() const {
    auto *item = m_results->currentItem();
    return item ? item->data(Qt::UserRole + 1).toInt() : -1;
}

void SearchDialog::setSession(const Session &session) {
    // Refresh only when note contents or order change, preserving keyboard navigation.
    bool changed = session.notes.size() != m_session.notes.size();
    for (int i = 0; !changed && i < session.notes.size(); ++i)
        changed = session.notes[i].id != m_session.notes[i].id || session.notes[i].text != m_session.notes[i].text;
    if (changed) {
        m_session = session;
        refresh();
    }
}

void SearchDialog::refresh() {
    const QString previousId = noteId();
    const int previousPosition = matchPosition();
    m_results->clear();
    const QString needle = query();
    if (needle.isEmpty()) {
        m_status->setText("Type to search all notes");
        return;
    }
    int selected = 0;
    for (int i = 0; i < m_session.notes.size() && m_results->count() < 100; ++i) {
        const auto &note = m_session.notes[i];
        int from = 0;
        while (m_results->count() < 100) {
            const int position = note.text.indexOf(needle, from, Qt::CaseInsensitive);
            if (position < 0) break;
            const int start = qMax(0, position - 35);
            QString snippet = note.text.mid(start, qMin(qsizetype(150), needle.size() + 70)).simplified();
            if (start > 0) snippet.prepend("…");
            if (start + qMin(qsizetype(150), needle.size() + 70) < note.text.size()) snippet.append("…");
            auto *item = new QListWidgetItem(QStringLiteral("%1  ·  %2").arg(i + 1).arg(snippet), m_results);
            item->setData(Qt::UserRole, note.id);
            item->setData(Qt::UserRole + 1, position);
            if (note.id == previousId && position == previousPosition)
                selected = m_results->count() - 1;
            from = position + needle.size();
        }
    }
    if (m_results->count()) m_results->setCurrentRow(selected);
    m_status->setText(m_results->count() == 0 ? "No matches" :
                     m_results->count() == 100 ? "Showing the first 100 matches" :
                     QStringLiteral("%1 matches").arg(m_results->count()));
}

bool SearchDialog::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            if (m_results->currentItem()) accept();
            return true;
        }
        if (object == m_query && (key == Qt::Key_Up || key == Qt::Key_Down)) {
            if (m_results->count())
                m_results->setCurrentRow(qBound(0, m_results->currentRow() + (key == Qt::Key_Down ? 1 : -1), m_results->count() - 1));
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}

Window::Window(SessionStore &store, const Session &session, ThemeWatcher &theme, QWidget *parent)
    : QMainWindow(parent), m_store(store), m_theme(theme.theme()), m_altLabel(altKeyLabel()) {
    setWindowTitle("Omadraft");
    setMinimumSize(420, 320);
    resize(960, 720);
    auto *root = new QWidget(this);
    root->setObjectName("appRoot");
    setCentralWidget(root);
    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(12, 4, 12, 14);
    outer->setSpacing(0);

    QFont interfaceFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    interfaceFont.setPointSizeF(11);
    setFont(interfaceFont);

    // Keep workspace numbers anchored to the window, independently of the writing column.
    m_tabs = new NoteTabs(root);
    QFont tabFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    tabFont.setPointSizeF(9);
    m_tabs->setFont(tabFont);
    auto *header = new QHBoxLayout;
    header->setSpacing(12);
    header->addWidget(m_tabs, 1);
    m_copy = new CopyButton(root);
    m_copy->setObjectName("copyNote");
    m_copy->setAccessibleName("Copy active note");
    m_copy->setToolTip(QStringLiteral("Copy the entire active note (%1+C)").arg(m_altLabel));
    QFont copyFont = interfaceFont;
    copyFont.setPointSizeF(9.5);
    m_copy->setFont(copyFont);
    m_copy->setFixedHeight(26);
    m_copy->setFixedWidth(32);
    header->addWidget(m_copy);
    outer->addLayout(header);
    connect(m_copy, &QPushButton::clicked, this, &Window::copyNote);
    m_copyTimer.setSingleShot(true);
    m_copyTimer.setInterval(1800);
    connect(&m_copyTimer, &QTimer::timeout, this, [this] { m_copy->setProperty("copied", false); m_copy->update(); });
    outer->addSpacing(38);

    auto *column = new QWidget(root);
    column->setMaximumWidth(820);
    auto *content = new QVBoxLayout(column);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    m_stack = new QStackedWidget(column);
    content->addWidget(m_stack, 1);
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(column);
    outer->addLayout(row, 1);

    m_error = new QLabel(root);
    m_error->setObjectName("saveError");
    m_error->setWordWrap(true);
    m_error->setAlignment(Qt::AlignCenter);
    m_error->setTextFormat(Qt::PlainText);
    m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_error->hide();
    outer->addWidget(m_error);
    m_hint = new QLabel(root);
    m_hint->setObjectName("keyboardHints");
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setWordWrap(true);
    m_hint->setContentsMargins(0, 14, 0, 0);
    QFont hintFont = interfaceFont;
    hintFont.setPointSizeF(9.5);
    m_hint->setFont(hintFont);
    outer->addWidget(m_hint);
    updateHint();

    m_saveTimer.setInterval(200);
    m_saveTimer.setSingleShot(true);
    connect(&m_saveTimer, &QTimer::timeout, this, &Window::saveNow);
    for (const Note &note : session.notes)
        appendEditor(note);
    connect(m_tabs, &QTabBar::currentChanged, this, &Window::setActive);
    m_tabs->setCurrentIndex(session.active);
    setActive(session.active);
    if (!session.geometry.isEmpty())
        restoreGeometry(session.geometry);

    const auto shortcut = [this](const QKeySequence &key, auto callback) {
        auto *action = new QShortcut(key, this);
        action->setAutoRepeat(false);
        connect(action, &QShortcut::activated, this, callback);
    };
    shortcut(QKeySequence("Alt+T"), &Window::newNote);
    shortcut(QKeySequence("Alt+Q"), &Window::discardNote);
    shortcut(QKeySequence("Alt+H"), &Window::showHelp);
    shortcut(QKeySequence("Alt+C"), &Window::copyNote);
    shortcut(QKeySequence("Alt+F"), &Window::showSearch);
    shortcut(QKeySequence("Alt+Left"), [this] { switchNote(-1); });
    shortcut(QKeySequence("Alt+Right"), [this] { switchNote(1); });
    connect(&theme, &ThemeWatcher::changed, this, &Window::applyTheme);
    applyTheme(m_theme);
    m_loading = false;
    m_syncTimer.setInterval(1000);
    connect(&m_syncTimer, &QTimer::timeout, this, [this] {
        if (!m_discarding) saveNow();
    });
    m_syncTimer.start();
    // Restore scrolling after the window has completed its first layout.
    QTimer::singleShot(0, this, [this, session] {
        m_loading = true;
        for (int i = 0; i < session.notes.size() && i < m_editors.size(); ++i)
            m_editors[i]->verticalScrollBar()->setValue(session.notes[i].scroll);
        m_loading = false;
        focusNote();
        saveNow();
    });
}

void Window::appendEditor(const Note &note) {
    auto *editor = new MarkdownEditor(m_theme, m_stack);
    editor->setObjectName("noteEditor");
    editor->setAccessibleName("Markdown note");
    editor->setFrameShape(QFrame::NoFrame);
    editor->setPlaceholderText("Write something…");
    QFont writingFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    writingFont.setPointSizeF(12.5);
    editor->setFont(writingFont);
    editor->document()->setDefaultFont(writingFont);
    editor->document()->setDocumentMargin(12);
    editor->setTabStopDistance(QFontMetricsF(writingFont).horizontalAdvance(' ') * 4);
    editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    editor->setPlainText(note.text);
    QTextCursor cursor = editor->textCursor();
    cursor.select(QTextCursor::Document);
    QTextBlockFormat spacing;
    spacing.setLineHeight(145, QTextBlockFormat::ProportionalHeight);
    cursor.mergeBlockFormat(spacing);
    cursor.setPosition(note.anchor);
    cursor.setPosition(note.cursor, QTextCursor::KeepAnchor);
    editor->setTextCursor(cursor);
    editor->document()->clearUndoRedoStacks();
    m_ids.append(note.id);
    m_editors.append(editor);
    m_stack->addWidget(editor);
    const int index = m_tabs->addTab(QString::number(m_tabs->count() + 1));
    m_tabs->setTabToolTip(index, QStringLiteral("Note %1").arg(index + 1));
    connect(editor, &QPlainTextEdit::textChanged, this, &Window::scheduleSave);
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, &Window::scheduleSave);
    connect(editor, &QPlainTextEdit::selectionChanged, this, &Window::scheduleSave);
    connect(editor->verticalScrollBar(), &QScrollBar::valueChanged, this, &Window::scheduleSave);
}

void Window::scheduleSave() {
    // Do not restart an active timer: continuous typing must still reach disk.
    if (!m_loading && !m_saveTimer.isActive())
        m_saveTimer.start();
}

Session Window::snapshot() const {
    Session session;
    for (int i = 0; i < m_editors.size(); ++i) {
        const auto *editor = m_editors[i];
        const QTextCursor cursor = editor->textCursor();
        session.notes.append(Note{m_ids[i], editor->toPlainText(), cursor.position(), cursor.anchor(),
                                  editor->verticalScrollBar()->value()});
    }
    session.active = m_tabs->currentIndex();
    session.geometry = saveGeometry();
    return session;
}

bool Window::saveNow() {
    m_saveTimer.stop();
    if (m_loading)
        return true;
    QString error;
    Session session = snapshot();
    if (!m_store.synchronize(session, error)) {
        m_lastSaveError = error;
        m_error->setText("Your latest changes could not be saved. Keep this window open.\n" + error);
        m_error->show();
        m_saveTimer.start(2000);
        return false;
    }
    applySession(session);
    m_lastSaveError.clear();
    m_error->hide();
    m_saveTimer.setInterval(200);
    return true;
}

void Window::setActive(int index) {
    if (index < 0 || index >= m_editors.size())
        return;
    m_stack->setCurrentIndex(index);
    m_editors[index]->setFocus();
    scheduleSave();
}

void Window::newNote() {
    if (m_discarding || m_helpOpen)
        return;
    appendEditor(Note::blank());
    applyTheme(m_theme);
    m_tabs->setCurrentIndex(m_tabs->count() - 1);
    saveNow();
}

void Window::switchNote(int offset) {
    if (!m_discarding && !m_helpOpen && m_tabs->count())
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + offset + m_tabs->count()) % m_tabs->count());
}

void Window::discardNote() {
    if (m_discarding || m_helpOpen)
        return;
    m_discarding = true;
    updateHint();
    DiscardDialog dialog(m_theme, this);
    const bool confirmed = dialog.exec() == QDialog::Accepted;
    m_discarding = false;
    updateHint();
    if (confirmed) {
        const int index = m_tabs->currentIndex();
        auto *editor = m_editors[index];
        m_loading = true;
        m_stack->removeWidget(editor);
        m_editors.removeAt(index);
        m_ids.removeAt(index);
        m_tabs->removeTab(index);
        delete editor;
        if (m_editors.isEmpty())
            appendEditor(Note::blank());
        for (int i = 0; i < m_tabs->count(); ++i) {
            m_tabs->setTabText(i, QString::number(i + 1));
            m_tabs->setTabToolTip(i, QStringLiteral("Note %1").arg(i + 1));
        }
        setActive(m_tabs->currentIndex());
        m_loading = false;
        applyTheme(m_theme);
        saveNow();
    }
    focusNote();
}

void Window::showSearch() {
    if (QApplication::activeModalWidget()) return;
    SearchDialog dialog(snapshot(), m_theme, this);
    connect(&m_syncTimer, &QTimer::timeout, &dialog, [this, &dialog] { dialog.setSession(snapshot()); });
    if (dialog.exec() == QDialog::Accepted) {
        const int index = m_ids.indexOf(dialog.noteId());
        if (index >= 0) {
            auto *editor = m_editors[index];
            const QString text = editor->toPlainText();
            int position = dialog.matchPosition();
            const QString query = dialog.query();
            // A sync may have replaced or deleted the selected result while the dialog was open.
            if (text.mid(position, query.size()).compare(query, Qt::CaseInsensitive) != 0)
                position = text.indexOf(query, 0, Qt::CaseInsensitive);
            if (position >= 0) {
                m_tabs->setCurrentIndex(index);
                QTextCursor cursor(editor->document());
                cursor.setPosition(position);
                cursor.setPosition(position + query.size(), QTextCursor::KeepAnchor);
                editor->setTextCursor(cursor);
                editor->ensureCursorVisible();
            }
        }
    }
    focusNote();
}

void Window::copyNote() {
    if (QApplication::activeModalWidget())
        return;
    auto *editor = qobject_cast<QPlainTextEdit *>(m_stack->currentWidget());
    if (!editor)
        return;
    // Capture the whole note before the dialog opens, independently of selection or sync.
    const QString source = editor->toPlainText();
    m_copyTimer.stop();
    m_copy->setProperty("copied", false);
    m_copy->update();
    CopyDialog dialog(m_theme, this);
    if (dialog.exec() == QDialog::Accepted) {
        QTextDocument document;
        document.setMarkdown(source, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
        auto *data = new QMimeData;
        data->setText(document.toPlainText());
        if (dialog.formatted())
            data->setHtml(document.toHtml());
        QApplication::clipboard()->setMimeData(data);
        m_copy->setProperty("copied", true);
        m_copy->update();
        m_copyTimer.start();
    }
    focusNote();
}

void Window::showHelp() {
    if (m_discarding || m_helpOpen)
        return;
    m_helpOpen = true;
    updateHint();
    HelpDialog dialog(m_theme, this);
    connect(&dialog, &HelpDialog::syncRequested, this, [this, &dialog] {
        SyncDialog setup(m_store.draftsDirectory(), m_theme, &dialog);
        setup.exec();
    });
    connect(&dialog, &HelpDialog::updatesRequested, this, [this, &dialog] {
        const QString path = qApp->property("installedExecutable").toString();
        UpdateDialog updates(path.isEmpty() ? QCoreApplication::applicationFilePath() : path, m_theme, &dialog);
        connect(&updates, &UpdateDialog::restartRequested, &dialog, [this, &dialog] {
            dialog.accept();
            QTimer::singleShot(0, this, [this] { emit restartRequested(); });
        });
        updates.exec();
    });
    dialog.exec();
    m_helpOpen = false;
    updateHint();
    focusNote();
}

void Window::updateHint() {
    if (m_discarding)
        m_hint->setText("←/→  Choose     Enter  Confirm     Esc  Cancel");
    else if (m_helpOpen)
        m_hint->setText("Esc / Enter  Close help");
    else
        m_hint->setText(QStringLiteral("%1+T  New     %1+Q  Discard     %1+←/→  Switch     %1+F  Search     %1+C  Copy     %1+H  Help").arg(m_altLabel));
}

void Window::focusNote() {
    if (auto *editor = qobject_cast<QPlainTextEdit *>(m_stack->currentWidget()))
        editor->setFocus();
}

void Window::applyTheme(const Theme &theme) {
    m_theme = theme;
    m_copy->setProperty("ink", theme.muted);
    m_copy->update();
    QPalette palette;
    palette.setColor(QPalette::Window, theme.background);
    palette.setColor(QPalette::WindowText, theme.foreground);
    palette.setColor(QPalette::Base, theme.background);
    palette.setColor(QPalette::Text, theme.foreground);
    palette.setColor(QPalette::PlaceholderText, theme.muted);
    palette.setColor(QPalette::Highlight, theme.selection);
    palette.setColor(QPalette::HighlightedText, theme.selectedText);
    setPalette(palette);
    centralWidget()->setAutoFillBackground(true);
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget#appRoot { background: %1; color: %2; }"
        "QPlainTextEdit { border: none; background: %1; color: %2; selection-background-color: %3; selection-color: %4; }"
        "QPushButton#copyNote { background: transparent; color: %5; border: 1px solid transparent; border-radius: 4px; padding: 2px 8px; }"
        "QPushButton#copyNote:hover, QPushButton#copyNote:focus { color: %2; border-color: %5; }"
        "QLabel#keyboardHints { color: %5; } QLabel#saveError { color: %2; padding: 8px; }"
        "QScrollBar:vertical { background: transparent; width: 5px; margin: 0; }"
        "QScrollBar::handle:vertical { background: %5; min-height: 24px; border-radius: 2px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QTabBar QToolButton { background: %1; color: %2; border: none; }")
        .arg(theme.background.name(), theme.foreground.name(), theme.selection.name(),
             theme.selectedText.name(), theme.muted.name()));
    for (auto *editor : m_editors)
        editor->setPalette(palette);
    m_tabs->setTheme(theme);
    for (auto *editor : m_editors)
        editor->setTheme(theme);
    if (auto *dialog = findChild<DiscardDialog *>())
        dialog->setTheme(theme);
    if (auto *dialog = findChild<SearchDialog *>())
        dialog->setTheme(theme);
    if (auto *dialog = findChild<CopyDialog *>())
        dialog->setTheme(theme);
    if (auto *dialog = findChild<HelpDialog *>())
        dialog->setTheme(theme);
    if (auto *dialog = findChild<SyncDialog *>())
        dialog->setTheme(theme);
    if (auto *dialog = findChild<UpdateDialog *>())
        dialog->setTheme(theme);
}

void Window::closeEvent(QCloseEvent *event) {
    if (!saveNow()) {
        QMessageBox::warning(this, "Notes not saved", "Omadraft could not save your latest changes.\n\n" +
                             m_lastSaveError + "\n\nKeep the app open and copy any important text to a safe location.");
        event->ignore();
        return;
    }
    event->accept();
}

void Window::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow() && !m_loading)
        saveNow();
}


void Window::applySession(const Session &session) {
    bool changed = m_ids.size() != session.notes.size();
    if (!changed) {
        for (int i = 0; i < m_ids.size(); ++i)
            if (m_ids[i] != session.notes[i].id || m_editors[i]->toPlainText() != session.notes[i].text) changed = true;
    }
    if (!changed) return;
    const bool wasEditing = m_stack->isAncestorOf(QApplication::focusWidget());
    const QSignalBlocker tabsBlocked(m_tabs);
    const QSignalBlocker stackBlocked(m_stack);
    m_loading = true;
    for (int i = m_ids.size() - 1; i >= 0; --i) {
        bool found = false;
        for (const Note &note : session.notes) if (note.id == m_ids[i]) found = true;
        if (!found) {
            auto *editor = m_editors.takeAt(i);
            m_stack->removeWidget(editor);
            m_ids.removeAt(i);
            m_tabs->removeTab(i);
            delete editor;
        }
    }
    for (const Note &note : session.notes) {
        const int index = m_ids.indexOf(note.id);
        if (index < 0) {
            appendEditor(note);
        } else if (m_editors[index]->toPlainText() != note.text) {
            auto *editor = m_editors[index];
            QTextCursor edit(editor->document());
            edit.select(QTextCursor::Document);
            edit.insertText(note.text);
            QTextCursor cursor = editor->textCursor();
            cursor.setPosition(qMin(note.anchor, int(note.text.size())));
            cursor.setPosition(qMin(note.cursor, int(note.text.size())), QTextCursor::KeepAnchor);
            editor->setTextCursor(cursor);
            editor->verticalScrollBar()->setValue(note.scroll);
        }
    }
    for (int i = 0; i < session.notes.size(); ++i) {
        const int from = m_ids.indexOf(session.notes[i].id);
        if (from != i) {
            m_ids.move(from, i);
            m_editors.move(from, i);
            auto *editor = m_editors[i];
            m_stack->removeWidget(editor);
            m_stack->insertWidget(i, editor);
            m_tabs->moveTab(from, i);
        }
    }
    for (int i = 0; i < m_ids.size(); ++i) {
        m_tabs->setTabText(i, QString::number(i + 1));
        const bool recovered = m_ids[i].startsWith("recovered-") || m_ids[i].contains(".sync-conflict-");
        m_tabs->setTabToolTip(i, recovered ? "Recovered conflict — both versions were kept" : QString("Note %1").arg(i + 1));
    }
    const int active = m_ids.indexOf(session.notes[session.active].id);
    m_tabs->setCurrentIndex(active);
    m_stack->setCurrentIndex(active);
    if (wasEditing) focusNote();
    applyTheme(m_theme);
    m_loading = false;
}

void Window::showSyncSetup() {
    if (m_discarding || m_helpOpen) return;
    m_helpOpen = true;
    SyncDialog dialog(m_store.draftsDirectory(), m_theme, this);
    dialog.exec();
    m_helpOpen = false;
    focusNote();
}
