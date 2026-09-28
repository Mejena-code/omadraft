#include "session.h"
#include "theme.h"
#include "markdown.h"
#include "window.h"
#include "shortcuts.h"
#include "sync.h"
#include "update.h"
#include "restart.h"
#include <QLockFile>
#include <QLocalServer>
#include <QNetworkReply>
#include <QSysInfo>
#include <QScopeGuard>
#include <QProcess>
#include <QCryptographicHash>
#include <QJsonArray>

#include <QApplication>
#include <QFontDatabase>
#include <QClipboard>
#include <QMimeData>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextLayout>

namespace {
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
QPlainTextEdit *currentEditor(Window &window) {
    return qobject_cast<QPlainTextEdit *>(window.findChild<QStackedWidget *>()->currentWidget());
}
QTextCharFormat formatAt(const QTextBlock &block, int position) {
    for (const auto &range : block.layout()->formats())
        if (position >= range.start && position < range.start + range.length)
            return range.format;
    return {};
}
}


namespace {
class ReleaseReply : public QNetworkReply {
public:
    ReleaseReply(const QNetworkRequest &request, QByteArray bytes, int status, QObject *parent)
        : QNetworkReply(parent), m_bytes(std::move(bytes)) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            emit readyRead();
            setFinished(true);
            emit finished();
        });
    }
    void abort() override { setError(OperationCanceledError, "Canceled"); }
    qint64 bytesAvailable() const override { return m_bytes.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 maximum) override {
        const qint64 size = qMin(maximum, qint64(m_bytes.size()) - m_offset);
        if (!size) return -1;
        memcpy(data, m_bytes.constData() + m_offset, size_t(size));
        m_offset += size;
        return size;
    }
private:
    QByteArray m_bytes;
    qint64 m_offset = 0;
};
class ReleaseTransport : public QNetworkAccessManager {
public:
    QMap<QString, QByteArray> responses;
    int requests = 0;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        ++requests;
        const QString key = request.url().toString();
        return new ReleaseReply(request, responses.value(key), responses.contains(key) ? 200 : 404, this);
    }
};
QJsonObject releaseJson(const QString &version, const QString &arch, const QByteArray &bytes) {
    const QString name = "omadraft-" + version + "-linux-" + arch;
    return {{"tag_name", "v" + version}, {"draft", false}, {"prerelease", false},
            {"assets", QJsonArray{QJsonObject{{"name", name}, {"state", "uploaded"}, {"size", bytes.size()},
              {"digest", "sha256:" + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
              {"browser_download_url", "https://github.com/" + Updater::repository() + "/releases/download/v" + version + '/' + name}}}}};
}
}

class OmadraftTests : public QObject {
    Q_OBJECT
private slots:
    void sessionRoundTrip();
    void backupRecovery();
    void incompatibleSessionIsPreserved();
    void invalidSessionIsPreserved();
    void failedWriteCanBeRetried();
    void readableThemeColors();
    void paletteParsing();
    void themeDirectoryReplacement();
    void windowThemeAndHintContrast();
    void markdownLiteralCode();
    void fencedCodeSurface();
    void markdownConcealment();
    void markdownEditingPreservesSource();
    void keyboardAndAutosave();
    void markdownHelpShortcut();
    void discardDialog();
    void discardAndRestart();
    void restoreCursorAndScroll();
    void draftMigration();
    void liveDraftChanges();
    void synchronizedTabOrderAndIdentity();
    void concurrentDraftChanges();
    void deletedDraftAndOfflineEdit();
    void missingDraftFolder();
    void interruptedDraftSave();
    void syncFolderConfiguration();
    void syncthingSetupIntegration();
    void releaseValidation();
    void atomicUpdate();
    void updateFlow();
    void restartProcessPreservesSession();
    void copyNote();
    void searchNotes();
    void shortcutLabels();
    void preview();
};

void OmadraftTests::sessionRoundTrip() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = QString::fromUtf8("# Thoughts\nSwedish: åäö. 日本語. 🌿\n**Keep this.**");
    session.notes[0].cursor = 8;
    session.notes[0].anchor = 2;
    session.notes[0].scroll = 7;
    session.notes.append(Note::blank());
    session.notes[1].text = "Another note";
    session.active = 1;
    QVERIFY2(store.save(session, error), qPrintable(error));
    SessionStore reopened(directory.path());
    Session restored;
    QVERIFY(reopened.load(restored, error, notice));
    QCOMPARE(SessionStore::encode(restored), SessionStore::encode(session));
    const auto permissions = QFile::permissions(directory.path() + "/session.json");
    QVERIFY(!(permissions & QFile::ReadOther));
    QVERIFY(!(permissions & QFile::WriteOther));
}

void OmadraftTests::backupRecovery() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Safe previous snapshot";
    QVERIFY(store.save(session, error));
    session.notes[0].text = "New snapshot";
    QVERIFY(store.save(session, error));
    QVERIFY(writeFile(directory.path() + "/session.json", "{ damaged"));
    SessionStore recovered(directory.path());
    QVERIFY(recovered.load(session, error, notice));
    QCOMPARE(session.notes[0].text, QString("New snapshot"));
    // The independent Markdown file survives damage to local window state.
    QVERIFY(!notice.isEmpty());
    QCOMPARE(QDir(directory.path()).entryList({"session.json.damaged-*"}, QDir::Files).size(), 1);
    QVERIFY(recovered.save(session, error));
    Session decoded;
    QVERIFY(SessionStore::decode(readFile(directory.path() + "/session.json"), decoded, error));
}

void OmadraftTests::incompatibleSessionIsPreserved() {
    QTemporaryDir directory;
    const QByteArray future = "{\"version\":99,\"notes\":[{\"text\":\"future data\"}]}";
    QVERIFY(writeFile(directory.path() + "/session.json", future));
    QVERIFY(writeFile(directory.path() + "/session.json.bak", SessionStore::encode(Session{{Note::blank()}, 0, {}})));
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(!store.load(session, error, notice));
    QVERIFY(!store.save(session, error));
    QCOMPARE(readFile(directory.path() + "/session.json"), future);
}

void OmadraftTests::invalidSessionIsPreserved() {
    QTemporaryDir directory;
    QVERIFY(writeFile(directory.path() + "/session.json", "broken primary"));
    QVERIFY(writeFile(directory.path() + "/session.json.bak", "broken backup"));
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(!store.load(session, error, notice));
    QVERIFY(!store.save(session, error));
    QCOMPARE(readFile(directory.path() + "/session.json"), QByteArray("broken primary"));
    QCOMPARE(readFile(directory.path() + "/session.json.bak"), QByteArray("broken backup"));
}

void OmadraftTests::failedWriteCanBeRetried() {
    QTemporaryDir directory;
    const QString path = directory.path() + "/notes";
    SessionStore store(path);
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Original";
    QVERIFY(store.save(session, error));
    const QByteArray original = readFile(path + "/session.json");
    QVERIFY(QDir().rename(path, path + ".moved"));
    QVERIFY(writeFile(path, "directory temporarily unavailable"));
    session.notes[0].text = "Unsaved changes";
    QVERIFY(!store.save(session, error));
    QCOMPARE(readFile(path + ".moved/session.json"), original);
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir().rename(path + ".moved", path));
    QVERIFY(store.save(session, error));
    Session reopened;
    QVERIFY(SessionStore::decode(readFile(path + "/session.json"), reopened, error));
    QCOMPARE(reopened.notes[0].text, QString("Unsaved changes"));
}

void OmadraftTests::readableThemeColors() {
    for (int bg = 0; bg <= 255; bg += 17) {
        for (const QColor &foreground : {QColor("#222222"), QColor("#dddddd"), QColor("#78824b"), QColor("#bb3344")}) {
            const QColor background(bg, bg, bg);
            const QColor corrected = Theme::readable(foreground, background);
            QVERIFY2(Theme::contrast(corrected, background) >= 4.5,
                     qPrintable(corrected.name() + " on " + background.name()));
        }
    }
    for (bool dark : {false, true}) {
        const Theme theme = Theme::fallback(dark);
        QVERIFY(Theme::contrast(theme.muted, theme.background) >= 4.5);
        QVERIFY(Theme::contrast(theme.accent, theme.background) >= 4.5);
    }
}

void OmadraftTests::paletteParsing() {
    const Theme theme = Theme::fromToml(
        "background = '#ffffff' # light\nforeground = \"#fefefe\"\nmuted = '#fafafa'\n"
        "accent = '#ffffff'\nselection = '#ffffff'\n[other]\nbackground = '#000000'\n", Theme::fallback());
    QCOMPARE(theme.background.name(), QString("#ffffff"));
    QVERIFY(Theme::contrast(theme.muted, theme.background) >= 4.5);
    QVERIFY(Theme::contrast(theme.foreground, theme.background) >= 4.5);
    QVERIFY(Theme::contrast(theme.selectedText, theme.selection) >= 4.5);
    QVERIFY(Theme::fromToml("not a palette", Theme::fallback()) == Theme::fallback());
}

void OmadraftTests::themeDirectoryReplacement() {
    QTemporaryDir directory;
    const QString themeDir = directory.path() + "/current/theme";
    QVERIFY(QDir().mkpath(themeDir));
    const QString file = themeDir + "/colors.toml";
    QVERIFY(writeFile(file, "background = '#222222'\nforeground = '#eeeeee'\n"));
    ThemeWatcher watcher({file});
    QCOMPARE(watcher.theme().background.name(), QString("#222222"));
    QSignalSpy changed(&watcher, &ThemeWatcher::changed);
    QVERIFY(QDir().rename(themeDir, themeDir + ".old"));
    QVERIFY(QDir().mkpath(themeDir));
    QVERIFY(writeFile(file, "background = '#fafafa'\nforeground = '#111111'\n"));
    QTRY_COMPARE_WITH_TIMEOUT(watcher.theme().background.name(), QString("#fafafa"), 2000);
    QVERIFY(changed.count() >= 1);
    QVERIFY(writeFile(file, "background = '#112233'\nforeground = '#ffffff'\n"));
    QTRY_COMPARE_WITH_TIMEOUT(watcher.theme().background.name(), QString("#112233"), 2000);
}

void OmadraftTests::fencedCodeSurface() {
    const Theme theme = Theme::fallback();
    MarkdownEditor editor(theme);
    editor.resize(440, 420);
    const QString source = "```python\ndef greet():\n" + QString("word ").repeated(25) +
                           "\n\n**literal**\n```\nAfter the code\n";
    editor.setPlainText(source);
    editor.show();
    editor.activateWindow();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    QTest::qWait(30);
    QVERIFY(editor.extraSelections().size() >= 6);
    const QImage rendered = editor.viewport()->grab().toImage();
    for (int i = 0; i < 6; ++i) {
        const QTextBlock block = editor.document()->findBlockByNumber(i);
        QVERIFY(dynamic_cast<MarkdownBlockData *>(block.userData())->fencedCode);
        for (int line = 0; line < block.layout()->lineCount(); ++line) {
            QTextCursor cursor(block);
            cursor.setPosition(block.position() + block.layout()->lineAt(line).textStart());
            const int y = editor.cursorRect(cursor).center().y();
            QCOMPARE(rendered.pixelColor(rendered.width() - 20, y), theme.surface);
        }
    }
    QCOMPARE(formatAt(editor.document()->findBlockByNumber(0), 0).foreground().color().alpha(), 0);
    QTextCursor opening(editor.document()->firstBlock());
    editor.setTextCursor(opening);
    QVERIFY(formatAt(editor.document()->firstBlock(), 0).foreground().color().alpha() > 0);
    QVERIFY(formatAt(editor.document()->findBlockByNumber(4), 2).fontWeight() != QFont::Bold);
    QCOMPARE(editor.toPlainText(), source);
    editor.setPlainText("Ordinary text");
    QTest::qWait(10);
    QVERIFY(editor.extraSelections().isEmpty());
    editor.clear();
    QTest::keyClicks(&editor, "```");
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::keyClicks(&editor, "code");
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::keyClicks(&editor, "```");
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::keyClicks(&editor, "outside");
    QTest::qWait(20);
    QCOMPARE(editor.extraSelections().size(), 3);
    QCOMPARE(editor.document()->findBlockByNumber(3).userState(), 0);
    editor.setPlainText("```\n" + QString("word ").repeated(25));
    editor.moveCursor(QTextCursor::End);
    QTest::qWait(20);
    const QImage unfinished = editor.viewport()->grab().toImage();
    const QTextBlock last = editor.document()->lastBlock();
    for (int line = 0; line < last.layout()->lineCount(); ++line) {
        QTextCursor cursor(last);
        cursor.setPosition(last.position() + last.layout()->lineAt(line).textStart());
        QCOMPARE(unfinished.pixelColor(unfinished.width() - 20, editor.cursorRect(cursor).center().y()), theme.surface);
    }
}

void OmadraftTests::markdownLiteralCode() {
    QTextDocument document;
    QFont font;
    font.setPointSizeF(12);
    document.setDefaultFont(font);
    MarkdownHighlighter highlighter(&document, Theme::fallback());
    document.setPlainText("# Heading\n**bold** and `**literal**`\n````md\n**literal block**\n```\nStill code\n````\n**bold again**");
    highlighter.rehighlight();
    QVERIFY(formatAt(document.firstBlock(), 3).fontPointSize() > 12);
    const auto inlineBlock = document.findBlockByNumber(1);
    QCOMPARE(formatAt(inlineBlock, 3).fontWeight(), int(QFont::Bold));
    QVERIFY(formatAt(inlineBlock, 16).fontWeight() != QFont::Bold);
    QVERIFY(formatAt(document.findBlockByNumber(3), 3).fontWeight() != QFont::Bold);
    QVERIFY(document.findBlockByNumber(4).userState() > 0);
    QCOMPARE(document.findBlockByNumber(6).userState(), 0);
    QCOMPARE(formatAt(document.findBlockByNumber(7), 3).fontWeight(), int(QFont::Bold));
}

void OmadraftTests::windowThemeAndHintContrast() {
    QTemporaryDir directory;
    const QString paletteFile = directory.path() + "/colors.toml";
    QVERIFY(writeFile(paletteFile, "background = '#222222'\nforeground = '#dddddd'\nmuted = '#333333'\n"));
    SessionStore store(directory.path() + "/notes");
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    ThemeWatcher theme({paletteFile});
    Window window(store, session, theme);
    window.show();
    QTest::qWait(30);
    QCOMPARE(window.grab().toImage().pixelColor(10, 10).name(), QString("#222222"));
    auto *hint = window.findChild<QLabel *>("keyboardHints");
    QVERIFY(Theme::contrast(hint->palette().color(QPalette::WindowText), theme.theme().background) >= 4.5);
    QVERIFY(writeFile(paletteFile, "background = '#fafafa'\nforeground = '#202020'\nmuted = '#eeeeee'\n"));
    QTRY_COMPARE(theme.theme().background.name(), QString("#fafafa"));
    QTRY_COMPARE(window.grab().toImage().pixelColor(10, 10).name(), QString("#fafafa"));
    QVERIFY(Theme::contrast(hint->palette().color(QPalette::WindowText), theme.theme().background) >= 4.5);
    QVERIFY(window.close());
}

void OmadraftTests::markdownConcealment() {
    MarkdownEditor editor(Theme::fallback());
    QFont font = editor.font();
    font.setPointSizeF(12);
    editor.setFont(font);
    const QString source = "# Heading\n> Quoted words\n**bold** and *italic*\n"
                           "[link](https://example.com)\n`**literal**`\n\\*literal asterisk\n"
                           "- List item\n***both*** and snake_case_name\n";
    editor.setPlainText(source);
    editor.resize(600, 500);
    editor.show();
    editor.activateWindow();
    editor.moveCursor(QTextCursor::End);
    QTest::qWait(30);
    const QTextBlock heading = editor.document()->firstBlock();
    QCOMPARE(formatAt(heading, 0).foreground().color().alpha(), 0);
    QVERIFY(heading.layout()->lineCount() > 0);
    QVERIFY(heading.layout()->lineAt(0).cursorToX(2) - heading.layout()->lineAt(0).cursorToX(0) < 0.1);
    QVERIFY(formatAt(heading, 2).fontPointSize() > 12);
    const auto quote = editor.document()->findBlockByNumber(1);
    QVERIFY(dynamic_cast<MarkdownBlockData *>(quote.userData())->quote);
    QCOMPARE(formatAt(quote, 0).foreground().color().alpha(), 0);
    const auto emphasis = editor.document()->findBlockByNumber(2);
    QCOMPARE(formatAt(emphasis, 0).foreground().color().alpha(), 0);
    QCOMPARE(formatAt(emphasis, 2).fontWeight(), int(QFont::Bold));
    QVERIFY(formatAt(emphasis, 14).fontItalic());
    const auto link = editor.document()->findBlockByNumber(3);
    QCOMPARE(formatAt(link, 0).foreground().color().alpha(), 0);
    QVERIFY(formatAt(link, 1).fontUnderline());
    QCOMPARE(formatAt(link, 7).foreground().color().alpha(), 0);
    const auto code = editor.document()->findBlockByNumber(4);
    QCOMPARE(formatAt(code, 0).foreground().color().alpha(), 0);
    QVERIFY(formatAt(code, 1).foreground().color().alpha() > 0);
    QVERIFY(formatAt(code, 3).fontWeight() != QFont::Bold);
    const auto escaped = editor.document()->findBlockByNumber(5);
    QVERIFY(formatAt(escaped, 1).foreground().color().alpha() > 0);
    QVERIFY(dynamic_cast<MarkdownBlockData *>(editor.document()->findBlockByNumber(6).userData())->bullet);
    const auto both = editor.document()->findBlockByNumber(7);
    QVERIFY(formatAt(both, 3).fontItalic());
    QCOMPARE(formatAt(both, 3).fontWeight(), int(QFont::Bold));
    QVERIFY(formatAt(both, 21).foreground().color().alpha() > 0);
    QCOMPARE(editor.toPlainText(), source);
}

void OmadraftTests::markdownEditingPreservesSource() {
    MarkdownEditor editor(Theme::fallback());
    const QString source = "# Heading\n> Quote\n*italic*\n";
    editor.setPlainText(source);
    editor.show();
    editor.activateWindow();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    QTest::qWait(30);
    const QTextBlock heading = editor.document()->firstBlock();
    QCOMPARE(formatAt(heading, 0).foreground().color().alpha(), 0);
    QTextCursor cursor(editor.document());
    cursor.setPosition(2);
    editor.setTextCursor(cursor);
    QVERIFY(formatAt(heading, 0).foreground().color().alpha() > 0);
    QTest::qWait(10);
    QVERIFY(heading.layout()->lineCount() > 0);
    QVERIFY(heading.layout()->lineAt(0).cursorToX(2) - heading.layout()->lineAt(0).cursorToX(0) > 5);
    QTest::keyClicks(&editor, "New ");
    editor.moveCursor(QTextCursor::End);
    QCOMPARE(formatAt(heading, 0).foreground().color().alpha(), 0);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor.toPlainText(), source);
    editor.selectAll();
    for (QTextBlock block = editor.document()->firstBlock(); block.isValid() && !block.text().isEmpty(); block = block.next())
        QVERIFY(dynamic_cast<MarkdownBlockData *>(block.userData())->sourceVisible);
    editor.copy();
    QCOMPARE(QApplication::clipboard()->text(), source);
    editor.moveCursor(QTextCursor::End);
    editor.clearFocus();
    QCOMPARE(formatAt(heading, 0).foreground().color().alpha(), 0);
    QCOMPARE(editor.toPlainText(), source);
}

void OmadraftTests::keyboardAndAutosave() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    ThemeWatcher theme({directory.path() + "/no-theme"});
    Window window(store, session, theme);
    window.show();
    window.activateWindow();
    QTest::qWait(30);
    auto *first = currentEditor(window);
    QTest::keyClicks(first, "First note");
    QTest::keyClick(first, Qt::Key_T, Qt::AltModifier);
    QCOMPARE(window.snapshot().notes.size(), 2);
    QTest::keyClicks(currentEditor(window), "Second note");
    QTest::keyClick(currentEditor(window), Qt::Key_Left, Qt::AltModifier);
    QCOMPARE(currentEditor(window), first);
    QTest::keyClick(first, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(first->toPlainText() != "First note");
    QTest::keyClick(first, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(first->toPlainText(), QString("First note"));
    QTest::keyClick(first, Qt::Key_Left, Qt::ControlModifier);
    QCOMPARE(window.snapshot().active, 0);
    QVERIFY(first->textCursor().position() < first->toPlainText().size());
    QTest::keyClick(first, Qt::Key_Left, Qt::AltModifier);
    QCOMPARE(window.snapshot().active, 1);
    QTest::keyClicks(currentEditor(window), " autosaved", Qt::NoModifier, 30);
    QTRY_VERIFY_WITH_TIMEOUT(readFile(directory.path() + "/session.json").contains("Second note autosaved"), 1000);
    QVERIFY(window.close());
}

void OmadraftTests::markdownHelpShortcut() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Keep this **note** intact";
    session.notes[0].anchor = 5;
    session.notes[0].cursor = 9;
    ThemeWatcher theme({directory.path() + "/no-theme"});
    Window window(store, session, theme);
    window.show();
    window.activateWindow();
    QTest::qWait(30);
    const QByteArray before = SessionStore::encode(window.snapshot());
    for (const auto key : {Qt::Key_Escape, Qt::Key_Return, Qt::Key_H}) {
        bool opened = false;
        bool themeUpdated = false;
        bool hasHeadingExample = false;
        window.activateWindow();
        window.focusNote();
        QTest::qWait(20);
        QTimer::singleShot(20, &window, [&] {
            auto *dialog = qobject_cast<HelpDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            opened = true;
            QCOMPARE(dialog->windowTitle(), QString("Help"));
            for (auto *label : dialog->findChildren<QLabel *>())
                hasHeadingExample |= label->text().contains("## Subheading");
            theme.changed(Theme::fallback(false));
            themeUpdated = dialog->grab().toImage().pixelColor(10, 10) == Theme::fallback(false).background;
            QTest::keyClick(dialog->focusWidget(), Qt::Key_T, Qt::AltModifier);
            QTest::keyClick(dialog->focusWidget(), Qt::Key_Q, Qt::AltModifier);
            QTest::keyClick(dialog->focusWidget(), key, key == Qt::Key_H ? Qt::AltModifier : Qt::NoModifier);
        });
        QTest::keyClick(currentEditor(window), Qt::Key_H, Qt::AltModifier);
        QTRY_VERIFY(opened);
        QVERIFY(hasHeadingExample);
        QVERIFY(themeUpdated);
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(SessionStore::encode(window.snapshot()), before);
        QCOMPARE(window.focusWidget(), currentEditor(window));
    }
    QVERIFY(window.close());
}

void OmadraftTests::discardDialog() {
    DiscardDialog dialog(Theme::fallback());
    dialog.show();
    dialog.activateWindow();
    QTest::qWait(20);
    auto *no = dialog.findChild<QPushButton *>("discardNo");
    auto *yes = dialog.findChild<QPushButton *>("discardYes");
    QVERIFY(no->hasFocus());
    QTest::keyClick(no, Qt::Key_Return);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    dialog.show();
    dialog.activateWindow();
    QTest::qWait(20);
    no->setFocus();
    QTest::keyClick(no, Qt::Key_Left);
    QVERIFY(yes->hasFocus());
    QTest::keyClick(yes, Qt::Key_Return);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    dialog.show();
    QTest::keyClick(yes, Qt::Key_Escape);
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
}

void OmadraftTests::discardAndRestart() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Keep me";
    ThemeWatcher theme({directory.path() + "/no-theme"});
    Window window(store, session, theme);
    window.show();
    window.activateWindow();
    QTest::qWait(20);
    window.newNote();
    currentEditor(window)->setPlainText("Discard me");
    QTimer::singleShot(20, [] {
        auto *dialog = qobject_cast<DiscardDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QTest::keyClick(dialog->focusWidget(), Qt::Key_Return);
    });
    window.discardNote();
    QCOMPARE(window.snapshot().notes.size(), 2);
    window.activateWindow();
    window.focusNote();
    QTest::qWait(20);
    QTimer::singleShot(20, [] {
        auto *dialog = qobject_cast<DiscardDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QTest::keyClick(dialog->focusWidget(), Qt::Key_Left);
        QTest::keyClick(dialog->focusWidget(), Qt::Key_Return);
    });
    QTest::keyClick(currentEditor(window), Qt::Key_Q, Qt::AltModifier);
    QCOMPARE(window.snapshot().notes.size(), 1);
    QCOMPARE(currentEditor(window)->toPlainText(), QString("Keep me"));
    QVERIFY(window.close());
    Session restored;
    SessionStore reopened(directory.path());
    QVERIFY(reopened.load(restored, error, notice));
    QCOMPARE(restored.notes.size(), 1);
    QCOMPARE(restored.notes[0].text, QString("Keep me"));
    window.show();
    QTimer::singleShot(20, [] {
        auto *dialog = qobject_cast<DiscardDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        dialog->accept();
    });
    window.discardNote();
    QCOMPARE(window.snapshot().notes.size(), 1);
    QVERIFY(currentEditor(window)->toPlainText().isEmpty());
    QVERIFY(window.close());
}

void OmadraftTests::restoreCursorAndScroll() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    for (int i = 0; i < 160; ++i)
        session.notes[0].text += QString("Line %1 of a long note\n").arg(i);
    session.notes[0].cursor = 1300;
    session.notes[0].anchor = 1280;
    session.notes[0].scroll = 40;
    ThemeWatcher theme({directory.path() + "/no-theme"});
    Window window(store, session, theme);
    window.show();
    QTest::qWait(50);
    QCOMPARE(currentEditor(window)->textCursor().position(), 1300);
    QCOMPARE(currentEditor(window)->textCursor().anchor(), 1280);
    QCOMPARE(currentEditor(window)->verticalScrollBar()->value(), 40);
    QVERIFY(window.close());
}


void OmadraftTests::draftMigration() {
    QTemporaryDir directory;
    Session legacy{{Note::blank()}, 0, "geometry"};
    legacy.notes[0].text = "# Existing draft\nUnicode: åäö 🌿";
    legacy.notes[0].cursor = legacy.notes[0].anchor = 4;
    const QByteArray original = SessionStore::encode(legacy);
    QVERIFY(writeFile(directory.path() + "/session.json", original));
    SessionStore store(directory.path());
    Session loaded;
    QString error, notice;
    QVERIFY2(store.load(loaded, error, notice), qPrintable(error));
    QCOMPARE(SessionStore::encode(loaded), original);
    QCOMPARE(readFile(directory.path() + "/session.before-sync.json"), original);
    QCOMPARE(readFile(store.draftsDirectory() + '/' + loaded.notes[0].id + ".md"), loaded.notes[0].text.toUtf8());
    SessionStore reopened(directory.path());
    QVERIFY(reopened.load(loaded, error, notice));
    QCOMPARE(SessionStore::encode(loaded), original);
}

void OmadraftTests::liveDraftChanges() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Original";
    QVERIFY(store.save(session, error));
    ThemeWatcher theme({directory.path() + "/no-theme"});
    Window window(store, session, theme);
    window.show();
    QTest::qWait(30);
    const QString file = store.draftsDirectory() + '/' + session.notes[0].id + ".md";
    QVERIFY(writeFile(file, "Changed on the other computer"));
    QTRY_COMPARE_WITH_TIMEOUT(currentEditor(window)->toPlainText(), QString("Changed on the other computer"), 2500);
    QVERIFY(writeFile(store.draftsDirectory() + "/remote-note.md", "New remote note"));
    QTRY_COMPARE_WITH_TIMEOUT(window.snapshot().notes.size(), 2, 2500);
    QCOMPARE(window.snapshot().notes[window.snapshot().active].id, session.notes[0].id);
    QVERIFY(window.close());
}

void OmadraftTests::synchronizedTabOrderAndIdentity() {
    QTemporaryDir desktopDir, laptopDir;
    Session desktop{{Note{"z-existing", "Desktop text", 12, 12, 0}}, 0, {}};
    Session laptop{{Note{"a-empty", "", 0, 0, 0}}, 0, {}};
    QVERIFY(writeFile(desktopDir.path() + "/session.json", SessionStore::encode(desktop)));
    QVERIFY(writeFile(laptopDir.path() + "/session.json", SessionStore::encode(laptop)));
    SessionStore desktopStore(desktopDir.path()), laptopStore(laptopDir.path());
    QString error, notice;
    QVERIFY(desktopStore.load(desktop, error, notice));
    QVERIFY(laptopStore.load(laptop, error, notice));
    ThemeWatcher theme({desktopDir.path() + "/no-theme"});
    Window first(desktopStore, desktop, theme), second(laptopStore, laptop, theme);
    first.show();
    second.show();
    QTest::qWait(30);
    auto *textEditor = currentEditor(first);
    textEditor->insertPlainText(" plus an edit");
    QVERIFY(first.saveNow());
    QVERIFY(writeFile(desktopStore.draftsDirectory() + "/a-empty.md", ""));
    QVERIFY(writeFile(laptopStore.draftsDirectory() + "/z-existing.md", textEditor->toPlainText().toUtf8()));
    QVERIFY(first.saveNow());
    QVERIFY(second.saveNow());
    QCOMPARE(first.snapshot().notes.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(first.snapshot().notes[i].id, second.snapshot().notes[i].id);
        QCOMPARE(first.snapshot().notes[i].text, second.snapshot().notes[i].text);
    }
    QCOMPARE(first.snapshot().active, 1);
    QCOMPARE(second.snapshot().active, 0);
    QCOMPARE(currentEditor(first), textEditor);
    textEditor->undo();
    QCOMPARE(textEditor->toPlainText(), QString("Desktop text"));
    textEditor->redo();
    QCOMPARE(textEditor->toPlainText(), QString("Desktop text plus an edit"));
    // Discard the selected text note. The other computer must keep its empty note.
    QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<DiscardDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        dialog->accept();
    });
    first.discardNote();
    QVERIFY(writeFile(laptopStore.draftsDirectory() + "/z-existing.deleted",
                      readFile(desktopStore.draftsDirectory() + "/z-existing.deleted")));
    QVERIFY(second.saveNow());
    QCOMPARE(first.snapshot().notes.size(), 1);
    QCOMPARE(second.snapshot().notes.size(), 1);
    QCOMPARE(first.snapshot().notes[0].id, QString("a-empty"));
    QCOMPARE(second.snapshot().notes[0].id, QString("a-empty"));
    QVERIFY(first.close());
    QVERIFY(second.close());
}

void OmadraftTests::concurrentDraftChanges() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Common base";
    QVERIFY(store.synchronize(session, error));
    const QString id = session.notes[0].id;
    session.notes[0].text = "Local edit";
    QVERIFY(writeFile(store.draftsDirectory() + '/' + id + ".md", "Remote edit"));
    QVERIFY(store.synchronize(session, error));
    QCOMPARE(session.notes.size(), 2);
    QCOMPARE(session.notes[0].text, QString("Local edit"));
    QCOMPARE(session.notes[1].text, QString("Remote edit"));
    QVERIFY(session.notes[1].id.startsWith("recovered-"));
    QVERIFY(store.synchronize(session, error));
    QCOMPARE(session.notes.size(), 2);
    QVERIFY(writeFile(store.draftsDirectory() + '/' + id + ".sync-conflict-20260927-120000-DEVICE.md", "Offline edit"));
    QVERIFY(store.synchronize(session, error));
    QCOMPARE(session.notes.size(), 3);
    SessionStore reopened(directory.path());
    Session loaded;
    QVERIFY(reopened.load(loaded, error, notice));
    QCOMPARE(SessionStore::encode(loaded), SessionStore::encode(session));
}

void OmadraftTests::deletedDraftAndOfflineEdit() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Common base";
    QVERIFY(store.synchronize(session, error));
    const QString id = session.notes[0].id;
    const QString file = store.draftsDirectory() + '/' + id + ".md";
    session.notes = {Note::blank()};
    QVERIFY(writeFile(file, "An offline edit arriving during discard"));
    QVERIFY(store.synchronize(session, error));
    QCOMPARE(session.notes.size(), 2);
    QVERIFY(QFileInfo::exists(store.draftsDirectory() + '/' + id + ".deleted"));
    QCOMPARE(session.notes[1].text, QString("An offline edit arriving during discard"));
    // A delayed stale file must not resurrect the discarded note.
    QVERIFY(writeFile(file, "Common base"));
    QVERIFY(store.synchronize(session, error));
    QCOMPARE(session.notes.size(), 2);
    // A delayed copy of the same edit must not create duplicate recoveries.
    QVERIFY(writeFile(file, "An offline edit arriving during discard"));
    QVERIFY(store.synchronize(session, error));
    QCOMPARE(session.notes.size(), 2);
    session.notes.removeAt(1);
    QVERIFY(store.synchronize(session, error));
    QVERIFY(writeFile(file, "An offline edit arriving during discard"));
    QVERIFY(store.synchronize(session, error));
    QCOMPARE(session.notes.size(), 1);
}

void OmadraftTests::missingDraftFolder() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Keep this";
    QVERIFY(store.synchronize(session, error));
    QVERIFY(QDir().rename(store.draftsDirectory(), store.draftsDirectory() + ".moved"));
    QVERIFY(!store.synchronize(session, error));
    SessionStore reopened(directory.path());
    QVERIFY(!reopened.load(session, error, notice));
    QVERIFY(!QFileInfo::exists(store.draftsDirectory()));
}

void OmadraftTests::interruptedDraftSave() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Base";
    QVERIFY(store.synchronize(session, error));
    const QString id = session.notes[0].id;
    const QJsonObject baseline{{id, "Base"}};
    session.notes[0].text = "Local edit interrupted before shared file write";
    const QJsonObject pending{{"baseline", baseline}, {"session", QJsonDocument::fromJson(SessionStore::encode(session)).object()}};
    QVERIFY(writeFile(directory.path() + "/pending-save.json", QJsonDocument(pending).toJson()));
    QVERIFY(writeFile(store.draftsDirectory() + '/' + id + ".md", "Remote edit"));
    SessionStore reopened(directory.path());
    QVERIFY2(reopened.load(session, error, notice), qPrintable(error));
    QCOMPARE(session.notes.size(), 2);
    QCOMPARE(session.notes[0].text, QString("Local edit interrupted before shared file write"));
    QCOMPARE(session.notes[1].text, QString("Remote edit"));
    QVERIFY(!QFileInfo::exists(directory.path() + "/pending-save.json"));
}

void OmadraftTests::syncFolderConfiguration() {
    QTemporaryDir directory;
    const QString path = directory.path() + "/drafts";
    const QJsonObject defaults{{"rescanIntervalS", 3600}, {"devices", QJsonArray{QJsonObject{{"deviceID", "UNRELATED"}}}}};
    QJsonObject folder;
    QString error;
    QVERIFY(SyncthingClient::prepareFolder({}, defaults, path, "LOCAL", "PEER", folder, error));
    QCOMPARE(folder["id"].toString(), QString(SyncthingClient::folderId));
    QCOMPARE(folder["devices"].toArray().size(), 2);
    QCOMPARE(folder["maxConflicts"].toInt(), -1);
    folder["customFutureOption"] = true;
    QJsonObject repeated;
    QVERIFY(SyncthingClient::prepareFolder(QJsonArray{folder}, defaults, path, "LOCAL", "PEER", repeated, error));
    QCOMPARE(repeated, folder);
    QVERIFY(SyncthingClient::prepareFolder(QJsonArray{folder}, defaults, path, "LOCAL", "THIRD", repeated, error));
    QCOMPARE(repeated["devices"].toArray().size(), 3);
    QCOMPARE(repeated["customFutureOption"].toBool(), true);
    folder["path"] = directory.path() + "/other";
    QVERIFY(!SyncthingClient::prepareFolder(QJsonArray{folder}, defaults, path, "LOCAL", "PEER", repeated, error));
    folder["id"] = "unrelated";
    folder["path"] = directory.path();
    QVERIFY(!SyncthingClient::prepareFolder(QJsonArray{folder}, defaults, path, "LOCAL", "PEER", repeated, error));
    QCOMPARE(SyncthingClient::normalizeDeviceId("bad-id"), QString());
    QCOMPARE(SyncthingClient::normalizeDeviceId(QString(56, 'a')).size(), 63);
}

void OmadraftTests::syncthingSetupIntegration() {
    const QString home = qEnvironmentVariable("OMADRAFT_TEST_SYNCTHING_HOME");
    if (home.isEmpty()) QSKIP("Run tests/sync_smoke.py for isolated Syncthing integration.");
    QVERIFY(qEnvironmentVariable("STHOMEDIR") == home);
    const QString directory = qEnvironmentVariable("OMADRAFT_TEST_DRAFTS");
    QVERIFY(directory.startsWith(home + '/'));
    SyncDialog dialog(directory, Theme::fallback());
    dialog.show();
    auto *own = dialog.findChild<QLineEdit *>("syncOwnId");
    auto *enable = dialog.findChild<QPushButton *>("syncEnable");
    auto *status = dialog.findChild<QLabel *>("syncStatus");
    QTRY_VERIFY2_WITH_TIMEOUT(!own->text().isEmpty() && enable->isEnabled(), qPrintable(status->text()), 15000);
    auto *peers = dialog.findChild<QComboBox *>("syncPeers");
    const QString peer = qEnvironmentVariable("OMADRAFT_TEST_PEER");
    const int index = peers->findData(peer);
    if (index >= 0) peers->setCurrentIndex(index);
    else dialog.findChild<QLineEdit *>("syncPeerId")->setText(peer);
    QTest::mouseClick(enable, Qt::LeftButton);
    QTRY_VERIFY2_WITH_TIMEOUT(status->text().startsWith("This computer is ready."), qPrintable(status->text()), 15000);
    const QString screenshots = qEnvironmentVariable("OMADRAFT_SCREENSHOT_DIR");
    if (!screenshots.isEmpty()) {
        QVERIFY(dialog.grab().save(screenshots + "/omadraft-sync-dark.png"));
        dialog.setTheme(Theme::fallback(false));
        QTest::qWait(20);
        QVERIFY(dialog.grab().save(screenshots + "/omadraft-sync-light.png"));
    }
    dialog.close();
}


void OmadraftTests::releaseValidation() {
    QByteArray binary(128, '\0');
    binary.replace(0, 4, QByteArray("\x7f" "ELF", 4));
    binary[4] = 2;
    binary[5] = 1;
    binary[18] = 62;
    binary[19] = 0;
    UpdateRelease release;
    QString error;
    const QString repo = Updater::repository();
    QVERIFY(!repo.isEmpty());
    QJsonObject json = releaseJson("0.4.0", "x86_64", binary);
    QVERIFY(Updater::selectRelease(json, repo, "x86_64", release, error));
    QVERIFY(Updater::validateBinary(binary, release, error));
    QVERIFY(!Updater::validateBinary(binary + 'x', release, error));
    QVERIFY(!Updater::selectRelease(json, repo, "aarch64", release, error));
    QVERIFY(!Updater::selectRelease(json, "someone/else", "x86_64", release, error));
    json["prerelease"] = true;
    QVERIFY(!Updater::selectRelease(json, repo, "x86_64", release, error));
    json["prerelease"] = false;
    auto assets = json["assets"].toArray();
    auto asset = assets[0].toObject();
    asset["digest"] = QJsonValue();
    assets[0] = asset;
    json["assets"] = assets;
    QVERIFY(!Updater::selectRelease(json, repo, "x86_64", release, error));
    json = releaseJson("0.4.0", "aarch64", binary);
    QVERIFY(Updater::selectRelease(json, repo, "aarch64", release, error));
    QVERIFY(!Updater::validateBinary(binary, release, error));
}

void OmadraftTests::atomicUpdate() {
    QTemporaryDir directory;
    QVERIFY(QDir().mkpath(directory.path() + "/bin"));
    QVERIFY(QDir().mkpath(directory.path() + "/share/omadraft"));
    const QString path = directory.path() + "/bin/omadraft";
    QVERIFY(writeFile(path, "original executable"));
    const QByteArray hash = QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256).toHex();
    QString error;
    QVERIFY(!Updater::replaceExecutable(path, hash, "replacement", error));
    QCOMPARE(readFile(path), QByteArray("original executable"));
    QVERIFY(writeFile(directory.path() + "/share/omadraft/install.json", "{\"kind\":\"local\",\"format\":1}"));
    QVERIFY(Updater::replaceExecutable(path, hash, "replacement", error));
    QCOMPARE(readFile(path), QByteArray("replacement"));
    QCOMPARE(readFile(path + ".previous"), QByteArray("original executable"));
    QVERIFY(QFileInfo(path).isExecutable());
    QVERIFY(!Updater::replaceExecutable(path, hash, "stale update", error));
    QCOMPARE(readFile(path), QByteArray("replacement"));
    QCOMPARE(readFile(path + ".previous"), QByteArray("original executable"));
    QVERIFY(QFile::remove(path + ".previous"));
    const QString outside = directory.path() + "/keep";
    QVERIFY(writeFile(outside, "keep"));
    QVERIFY(QFile::link(outside, path + ".previous"));
    const QByteArray current = QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256).toHex();
    QVERIFY(!Updater::replaceExecutable(path, current, "replacement 2", error));
    QCOMPARE(readFile(outside), QByteArray("keep"));
}

void OmadraftTests::updateFlow() {
    const QString binaryPath = QDir(QCoreApplication::applicationDirPath()).filePath("../app/omadraft");
    if (!QFileInfo::exists(binaryPath)) QSKIP("Run bin/build before testing the executable update flow.");
    const QString originalVersion = QCoreApplication::applicationVersion();
    QCoreApplication::setApplicationVersion("0.0.1");
    const auto restore = qScopeGuard([&] { QCoreApplication::setApplicationVersion(originalVersion); });
    QTemporaryDir directory;
    QVERIFY(QDir().mkpath(directory.path() + "/bin"));
    QVERIFY(QDir().mkpath(directory.path() + "/share/omadraft"));
    QVERIFY(writeFile(directory.path() + "/share/omadraft/install.json", "{\"kind\":\"local\"}"));
    const QString path = directory.path() + "/bin/omadraft";
    const QByteArray original = readFile(binaryPath);
    QVERIFY(writeFile(path, original));
    QByteArray next = original + "update-test";
    QProcess probe;
    probe.start(binaryPath, {"--version"});
    QVERIFY(probe.waitForFinished(5000));
    QCOMPARE(probe.exitCode(), 0);
    const QString version = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().section(' ', 1);
    QVERIFY(!version.isEmpty());
    const QString arch = Updater::architecture();
    const QJsonObject json = releaseJson(version, arch, next);
    const QString api = "https://api.github.com/repos/" + Updater::repository() + "/releases/latest";
    const QString download = json["assets"].toArray()[0].toObject()["browser_download_url"].toString();
    ReleaseTransport transport;
    transport.responses[api] = QJsonDocument(json).toJson();
    transport.responses[download] = next;
    Updater updater(path, nullptr, &transport);
    QSignalSpy changes(&updater, &Updater::changed);
    QCOMPARE(transport.requests, 0); // Creating the updater must not access the network.
    updater.check();
    QTRY_COMPARE_WITH_TIMEOUT(updater.state(), Updater::Available, 6000);
    updater.install();
    QTRY_COMPARE_WITH_TIMEOUT(updater.state(), Updater::Installed, 15000);
    QCOMPARE(readFile(path), next);
    QCOMPARE(readFile(path + ".previous"), original);
    QCOMPARE(transport.requests, 2);

    UpdateDialog dialog(path, Theme::fallback(), nullptr, &transport);
    QSignalSpy restart(&dialog, &UpdateDialog::restartRequested);
    dialog.show();
    auto *action = dialog.findChild<QPushButton *>("updateAction");
    QTRY_COMPARE_WITH_TIMEOUT(action->text(), QString("Update"), 6000);
    const QString screenshots = qEnvironmentVariable("OMADRAFT_SCREENSHOT_DIR");
    if (!screenshots.isEmpty()) {
        QVERIFY(QDir().mkpath(screenshots));
        QVERIFY(dialog.grab().save(screenshots + "/omadraft-update-dark.png"));
        dialog.setTheme(Theme::fallback(false));
        QTest::qWait(20);
        QVERIFY(dialog.grab().save(screenshots + "/omadraft-update-light.png"));
    }
    QTest::mouseClick(action, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(action->text(), QString("Restart now"), 15000);
    QTest::mouseClick(action, Qt::LeftButton);
    QCOMPARE(restart.count(), 1);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));

    // A corrupted response never replaces the executable.
    transport.responses[download] = "corrupt download";
    Updater corrupt(path, nullptr, &transport);
    corrupt.check();
    QTRY_COMPARE_WITH_TIMEOUT(corrupt.state(), Updater::Available, 6000);
    corrupt.install();
    QTRY_COMPARE_WITH_TIMEOUT(corrupt.state(), Updater::Idle, 6000);
    QCOMPARE(readFile(path), next);

    // An otherwise valid release that cannot run must also leave the app intact.
    QByteArray incompatible(128, '\0');
    incompatible.replace(0, 4, QByteArray("\x7f" "ELF", 4));
    incompatible[4] = 2;
    incompatible[5] = 1;
    incompatible[18] = arch == "x86_64" ? 62 : char(183);
    incompatible[19] = 0;
    transport.responses[api] = QJsonDocument(releaseJson(version, arch, incompatible)).toJson();
    transport.responses[download] = incompatible;
    Updater incompatibleUpdate(path, nullptr, &transport);
    incompatibleUpdate.check();
    QTRY_COMPARE_WITH_TIMEOUT(incompatibleUpdate.state(), Updater::Available, 6000);
    incompatibleUpdate.install();
    QTRY_COMPARE_WITH_TIMEOUT(incompatibleUpdate.state(), Updater::Idle, 15000);
    QCOMPARE(readFile(path), next);
}

void OmadraftTests::restartProcessPreservesSession() {
    if (!qEnvironmentVariableIsSet("OMADRAFT_PROCESS_TESTS"))
        QSKIP("Set OMADRAFT_PROCESS_TESTS=1 to test process replacement with local sockets.");
    QTemporaryDir directory;
    QProcess process;
    process.start(QCoreApplication::applicationFilePath(), {"--restart-probe", directory.path()});
    QVERIFY(process.waitForFinished(15000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
    QCOMPARE(process.readAllStandardOutput().trimmed(), QByteArray("Restart preserved notes, PID, arguments, and session lock."));
}

void OmadraftTests::shortcutLabels() {
    QTemporaryDir root;
    QVERIFY(QDir().mkpath(root.path() + "/sys/firmware/devicetree/base"));
    QVERIFY(QDir().mkpath(root.path() + "/sys/class/dmi/id"));
    const QString model = root.path() + "/sys/firmware/devicetree/base/model";
    const QString product = root.path() + "/sys/class/dmi/id/product_name";
    QCOMPARE(altKeyLabel(root.path()), QString("Alt"));
    QVERIFY(writeFile(model, QByteArray("Apple MacBook Pro (13-inch, M1, 2020)") + char(0)));
    QCOMPARE(altKeyLabel(root.path()), QString("⌥"));
    QVERIFY(QFile::remove(model));
    QVERIFY(writeFile(product, "MacBookPro17,1\n"));
    QCOMPARE(altKeyLabel(root.path()), QString("⌥"));
    QVERIFY(writeFile(product, "Desktop\n"));
    QCOMPARE(altKeyLabel(root.path()), QString("Alt"));
}

void OmadraftTests::searchNotes() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "First MATCH";
    auto second = Note::blank();
    second.text = "Åäö match and another match";
    session.notes.append(second);
    ThemeWatcher theme({directory.path() + "/no-theme"});
    SearchDialog search(session, theme.theme());
    auto *query = search.findChild<QLineEdit *>("searchQuery");
    auto *results = search.findChild<QListWidget *>("searchResults");
    QCOMPARE(results->count(), 0);
    query->setText("match");
    QCOMPARE(results->count(), 3);
    results->setCurrentRow(1);
    QCOMPARE(search.noteId(), second.id);
    QCOMPARE(search.matchPosition(), 4);
    auto changed = session;
    changed.notes.removeFirst();
    search.setSession(changed);
    QCOMPARE(results->count(), 2);
    QCOMPARE(search.noteId(), second.id);
    QCOMPARE(search.matchPosition(), 4);
    query->setText("ÅÄÖ");
    QCOMPARE(results->count(), 1);
    query->setText("missing");
    QCOMPARE(results->count(), 0);
    QTest::keyClick(query, Qt::Key_Return);
    QCOMPARE(search.result(), int(QDialog::Rejected));
    query->setText("[.*]");
    QCOMPARE(results->count(), 0);

    Window window(store, session, theme);
    window.show();
    window.activateWindow();
    QTest::qWait(30);
    auto open = [&](bool cancel) {
        window.activateWindow();
        window.focusNote();
        QTest::qWait(20);
        QTimer::singleShot(30, &window, [&] {
            auto *dialog = qobject_cast<SearchDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto closeDialog = qScopeGuard([&] { if (dialog->isVisible()) dialog->reject(); });
            auto *input = dialog->findChild<QLineEdit *>("searchQuery");
            QVERIFY(input->hasFocus());
            QTest::keyClicks(input, "match");
            QTest::keyClick(input, Qt::Key_Down);
            QTest::keyClick(input, cancel ? Qt::Key_Escape : Qt::Key_Return);
        });
        QTest::keyClick(currentEditor(window), Qt::Key_F, Qt::AltModifier);
    };
    open(true);
    QCOMPARE(window.snapshot().active, 0);
    open(false);
    QCOMPARE(window.snapshot().active, 1);
    QCOMPARE(currentEditor(window)->textCursor().selectedText(), QString("match"));
    QCOMPARE(currentEditor(window)->textCursor().selectionStart(), 4);
    QCOMPARE(currentEditor(window)->toPlainText(), second.text);
    QVERIFY(window.close());
}

void OmadraftTests::copyNote() {
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "Do not copy this other note";
    auto note = Note::blank();
    note.text = "# Heading\n\n**Bold** and *italic* with `code`.\n\n> A quote\n\n- Item\n\n[Link](https://example.com)\n\nÅäö";
    session.notes.append(note);
    session.active = 1;
    ThemeWatcher theme({directory.path() + "/no-theme"});
    Window window(store, session, theme);
    window.show();
    QTest::qWait(30);
    auto *editor = currentEditor(window);
    auto cursor = editor->textCursor();
    cursor.setPosition(2);
    cursor.setPosition(5, QTextCursor::KeepAnchor);
    editor->setTextCursor(cursor);
    auto *button = window.findChild<QPushButton *>("copyNote");
    QVERIFY(button);
    auto choose = [&](bool rich, bool cancel) {
        window.activateWindow();
        window.focusNote();
        QTest::qWait(20);
        QTimer::singleShot(30, [&] {
            auto *dialog = qobject_cast<CopyDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto closeDialog = qScopeGuard([&] { if (dialog->isVisible()) dialog->reject(); });
            auto *plain = dialog->findChild<QPushButton *>("copyPlain");
            auto *formatted = dialog->findChild<QPushButton *>("copyFormatted");
            QVERIFY(plain->hasFocus());
            if (rich) {
                QTest::keyClick(plain, Qt::Key_Right);
                QVERIFY(formatted->hasFocus());
            }
            QTest::keyClick(rich ? formatted : plain, cancel ? Qt::Key_Escape : Qt::Key_Return);
        });
        if (rich)
            QTest::keyClick(editor, Qt::Key_C, Qt::AltModifier);
        else
            button->click();
    };
    choose(false, false);
    auto *clipboard = QApplication::clipboard();
    const QString plain = clipboard->text();
    QVERIFY(plain.startsWith("Heading"));
    QVERIFY(plain.contains("Bold and italic with code."));
    QVERIFY(plain.contains("A quote"));
    QVERIFY(plain.contains("Item"));
    QVERIFY(plain.contains("Link"));
    QVERIFY(plain.contains("Åäö"));
    QVERIFY(!plain.contains("**"));
    QVERIFY(!plain.contains("other note"));
    QVERIFY(!clipboard->mimeData()->hasHtml());
    QVERIFY(button->text().isEmpty());
    QVERIFY(button->property("copied").toBool());
    choose(true, false);
    QCOMPARE(clipboard->text(), plain);
    QVERIFY(clipboard->mimeData()->hasHtml());
    const QString html = clipboard->mimeData()->html();
    QVERIFY(html.contains("<h1"));
    QVERIFY(html.contains("font-weight:700"));
    QVERIFY(html.contains("font-style:italic"));
    QVERIFY(html.contains("<ul"));
    QVERIFY(html.contains("https://example.com"));
    choose(false, true);
    QCOMPARE(clipboard->mimeData()->html(), html);
    QCOMPARE(editor->toPlainText(), note.text);
    QCOMPARE(editor->textCursor().selectedText(), QString("Hea"));
    QVERIFY(button->text().isEmpty());
    QVERIFY(!button->property("copied").toBool());
    editor->clear();
    choose(false, false);
    QCOMPARE(clipboard->text(), QString());
    QVERIFY(!clipboard->mimeData()->hasHtml());
    QVERIFY(window.close());
}

void OmadraftTests::preview() {
    const QString output = qEnvironmentVariable("OMADRAFT_SCREENSHOT_DIR");
    if (output.isEmpty())
        QSKIP("Set OMADRAFT_SCREENSHOT_DIR to render previews.");
    QVERIFY(QDir().mkpath(output));
    const QString oldVersion = QCoreApplication::applicationVersion();
    const auto restoreVersion = qScopeGuard([&] { QCoreApplication::setApplicationVersion(oldVersion); });
    QProcess versionProbe;
    versionProbe.start(QDir(QCoreApplication::applicationDirPath()).filePath("../app/omadraft"), {"--version"});
    QVERIFY(versionProbe.waitForFinished(5000));
    QCOMPARE(versionProbe.exitCode(), 0);
    QCoreApplication::setApplicationVersion(QString::fromUtf8(versionProbe.readAllStandardOutput()).trimmed().section(' ', 1));
    QTemporaryDir directory;
    SessionStore store(directory.path());
    Session session;
    QString error, notice;
    QVERIFY(store.load(session, error, notice));
    session.notes[0].text = "# A little room to think\n\nCapture the thought. Get back to your day.\n\n"
                            "## Today\n- Write down the idea\n- Give the next step a name\n- Leave room for something unexpected\n\n"
                            "### Keep it simple\n**A little emphasis.** *A quieter aside.* ~~Overthinking.~~\n\n"
                            "> Good ideas deserve somewhere to land.\n\n"
                            "A [link to Omarchy](https://omarchy.org) and `just enough code`.\n\n";
    session.notes[0].cursor = int(session.notes[0].text.size());
    session.notes[0].anchor = session.notes[0].cursor;
    for (int i = 0; i < 4; ++i)
        session.notes.append(Note::blank());
    ThemeWatcher theme;
    Window window(store, session, theme);
    window.show();
    QTest::qWait(100);
    QVERIFY(window.grab().save(output + "/omadraft-dark.png"));
    SearchDialog search(session, theme.theme(), &window);
    search.findChild<QLineEdit *>("searchQuery")->setText("idea");
    search.show();
    QTest::qWait(30);
    QVERIFY(search.grab().save(output + "/omadraft-search.png"));
    search.close();
    CopyDialog copy(theme.theme(), &window);
    copy.show();
    QTest::qWait(30);
    QVERIFY(copy.grab().save(output + "/omadraft-copy.png"));
    copy.close();
    DiscardDialog dialog(theme.theme(), &window);
    dialog.show();
    QTest::qWait(30);
    QVERIFY(dialog.grab().save(output + "/omadraft-discard.png"));
    dialog.close();
    HelpDialog help(theme.theme(), &window);
    help.show();
    QTest::qWait(30);
    QVERIFY(help.grab().save(output + "/omadraft-help-dark.png"));
    help.setTheme(Theme::fallback(false));
    QTest::qWait(20);
    QVERIFY(help.grab().save(output + "/omadraft-help-light.png"));
    help.close();
    theme.changed(Theme::fallback(false));
    QTest::qWait(20);
    QVERIFY(window.grab().save(output + "/omadraft-light.png"));
    window.resize(420, 400);
    QTest::qWait(20);
    QVERIFY(window.grab().save(output + "/omadraft-narrow.png"));
    QVERIFY(window.close());

    Window codeWindow(store, session, theme);
    codeWindow.resize(720, 600);
    auto *codeEditor = currentEditor(codeWindow);
    codeEditor->setPlainText("# Code block\n\nText before the block.\n\n```python\ndef greet(name):\n    message = f'Hello, {name}!'\n\n    print(message)\n\ngreet('Omadraft')\n```\n\nText after the block.\n");
    codeWindow.show();
    theme.changed(Theme::fallback(false));
    codeWindow.activateWindow();
    codeEditor->setFocus();
    codeEditor->moveCursor(QTextCursor::End);
    QTest::qWait(30);
    QVERIFY(codeWindow.grab().save(output + "/omadraft-code-light.png"));
    theme.changed(Theme::fallback(true));
    QTest::qWait(30);
    QVERIFY(codeWindow.grab().save(output + "/omadraft-code-dark.png"));
    QTextCursor codeCursor(codeEditor->document()->findBlockByNumber(5));
    codeEditor->setTextCursor(codeCursor);
    QTest::qWait(30);
    QVERIFY(codeWindow.grab().save(output + "/omadraft-code-editing.png"));
    QVERIFY(codeWindow.close());

    MarkdownEditor headings(theme.theme());
    QPalette headingPalette = headings.palette();
    headingPalette.setColor(QPalette::Base, theme.theme().background);
    headingPalette.setColor(QPalette::Text, theme.theme().foreground);
    headings.setPalette(headingPalette);
    QFont headingFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    headingFont.setPointSizeF(12.5);
    headings.setFont(headingFont);
    headings.setPlainText("# Heading 1\n## Heading 2\n### Heading 3\n#### Heading 4\n##### Heading 5\n###### Heading 6\n\nBody text\n\n");
    headings.moveCursor(QTextCursor::End);
    headings.resize(600, 500);
    headings.show();
    headings.clearFocus();
    QTest::qWait(30);
    QVERIFY(headings.grab().save(output + "/omadraft-headings.png"));
}

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    const QStringList args = application.arguments();
    if (args.size() == 3 && (args[1] == "--restart-probe" || args[1] == "--restart-after")) {
        const QString directory = args[2];
        QLockFile lock(directory + "/session.lock");
        if (!lock.tryLock()) return 10;
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen("omadraft-restart-test-" + QFileInfo(directory).fileName())) return 11;
        SessionStore store(directory);
        Session session;
        QString error, notice;
        if (!store.load(session, error, notice)) return 12;
        if (args[1] == "--restart-probe") {
            session.notes[0].text = "Notes saved before restart: åäö";
            if (!store.save(session, error)) return 13;
            if (!writeFile(directory + "/pid", QByteArray::number(application.applicationPid()))) return 14;
            replaceProcess(application.applicationFilePath(), {"--restart-after", directory}, lock, server, error);
            return 15;
        }
        if (readFile(directory + "/pid") != QByteArray::number(application.applicationPid()) ||
            session.notes[0].text != "Notes saved before restart: åäö") return 16;
        fprintf(stdout, "Restart preserved notes, PID, arguments, and session lock.\n");
        return 0;
    }
    OmadraftTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "test_omadraft.moc"
