#pragma once

#include "theme.h"
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTextBlockUserData>

struct MarkdownBlockData : QTextBlockUserData {
    bool sourceVisible = false;
    bool quote = false;
    bool bullet = false;
    bool fencedCode = false;
    int indent = 0;
};

class MarkdownHighlighter : public QSyntaxHighlighter {
public:
    explicit MarkdownHighlighter(QTextDocument *document, const Theme &theme);
    void setTheme(const Theme &theme);
    void setSourceRange(int firstBlock, int lastBlock);
protected:
    void highlightBlock(const QString &text) override;
private:
    Theme m_theme;
    int m_sourceFirst = -1;
    int m_sourceLast = -1;
};

class MarkdownEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit MarkdownEditor(const Theme &theme, QWidget *parent = nullptr);
    void setTheme(const Theme &theme);
protected:
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    void updateSourceRange();
    void updateCodeBackgrounds();
    MarkdownHighlighter *m_highlighter;
    Theme m_theme;
};
