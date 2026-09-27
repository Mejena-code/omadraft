#include "markdown.h"

#include <QFocusEvent>
#include <QFont>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document, const Theme &theme)
    : QSyntaxHighlighter(document), m_theme(theme) {}

void MarkdownHighlighter::setTheme(const Theme &theme) {
    m_theme = theme;
    rehighlight();
}

void MarkdownHighlighter::setSourceRange(int firstBlock, int lastBlock) {
    if (firstBlock == m_sourceFirst && lastBlock == m_sourceLast)
        return;
    QSet<int> affected;
    for (int block = m_sourceFirst; block >= 0 && block <= m_sourceLast; ++block)
        affected.insert(block);
    for (int block = firstBlock; block >= 0 && block <= lastBlock; ++block)
        affected.insert(block);
    m_sourceFirst = firstBlock;
    m_sourceLast = lastBlock;
    for (int number : affected) {
        const QTextBlock block = document()->findBlockByNumber(number);
        if (block.isValid())
            rehighlightBlock(block);
    }
}

void MarkdownHighlighter::highlightBlock(const QString &text) {
    setCurrentBlockState(0);
    auto *data = new MarkdownBlockData;
    data->sourceVisible = currentBlock().blockNumber() >= m_sourceFirst &&
                          currentBlock().blockNumber() <= m_sourceLast && m_sourceFirst >= 0;
    setCurrentBlockUserData(data);
    QVector<QPair<int, int>> concealed;
    const auto conceal = [&](int start, int length) {
        if (length > 0)
            concealed.append({start, length});
    };
    const auto finish = [&] {
        if (data->sourceVisible)
            return;
        for (const auto &range : concealed) {
            QTextCharFormat hidden;
            hidden.setForeground(Qt::transparent);
            hidden.setBackground(Qt::transparent);
            hidden.setFontUnderline(false);
            hidden.setFontStrikeOut(false);
            // Qt treats 0% spacing as the default. 0.1% collapses each glyph to
            // the layout's subpixel minimum without changing the source or line height.
            hidden.setFontLetterSpacingType(QFont::PercentageSpacing);
            hidden.setFontLetterSpacing(0.1);
            setFormat(range.first, range.second, hidden);
        }
    };
    QTextCharFormat muted;
    muted.setForeground(m_theme.muted);
    QTextCharFormat code;
    code.setForeground(Theme::readable(m_theme.foreground, m_theme.surface));
    code.setBackground(m_theme.surface);
    static const QRegularExpression fence(R"(^ {0,3}(`{3,}|~{3,})(.*)$)");
    const auto fenceMatch = fence.match(text);
    if (previousBlockState() > 0) {
        const int state = previousBlockState();
        const QChar delimiter = (state & 1) ? '~' : '`';
        const bool closes = fenceMatch.hasMatch() && fenceMatch.captured(1).at(0) == delimiter &&
                            fenceMatch.capturedLength(1) >= (state >> 1) && fenceMatch.captured(2).trimmed().isEmpty();
        setFormat(0, int(text.size()), closes ? muted : code);
        setCurrentBlockState(closes ? 0 : state);
        if (closes)
            conceal(0, int(text.size()));
        finish();
        return;
    }
    if (fenceMatch.hasMatch()) {
        setFormat(0, int(text.size()), muted);
        setCurrentBlockState((fenceMatch.capturedLength(1) << 1) | (fenceMatch.captured(1).at(0) == '~' ? 1 : 0));
        conceal(0, int(text.size()));
        finish();
        return;
    }
    if (text.startsWith("    ") || text.startsWith('\t')) {
        setFormat(0, int(text.size()), code);
        return;
    }

    static const QRegularExpression heading(R"(^ {0,3}(#{1,6})(\s+)(.+)$)");
    const auto headingMatch = heading.match(text);
    if (headingMatch.hasMatch()) {
        QTextCharFormat title;
        title.setFontWeight(QFont::DemiBold);
        title.setForeground(m_theme.foreground);
        const double base = document()->defaultFont().pointSizeF();
        static constexpr double headingScales[] = {2.0, 1.6, 1.3, 1.15, 1.05, 1.0};
        title.setFontPointSize(base * headingScales[headingMatch.capturedLength(1) - 1]);
        setFormat(0, int(text.size()), title);
        QTextCharFormat marker = title;
        marker.setForeground(m_theme.muted);
        setFormat(headingMatch.capturedStart(1), headingMatch.capturedLength(1), marker);
        conceal(0, headingMatch.capturedStart(3));
        static const QRegularExpression closingHashes(R"(\s+#+\s*$)");
        const auto closing = closingHashes.match(text);
        if (closing.hasMatch())
            conceal(closing.capturedStart(), closing.capturedLength());
    }
    static const QRegularExpression quote(R"(^( {0,3})(>+)\s?)");
    const auto quoteMatch = quote.match(text);
    if (quoteMatch.hasMatch()) {
        setFormat(0, int(text.size()), muted);
        data->quote = true;
        data->indent = quoteMatch.capturedLength(1);
        conceal(quoteMatch.capturedStart(2), quoteMatch.capturedLength(2));
    }
    static const QRegularExpression bullet(R"(^(\s*)([-+*]|\d+[.)])\s+)");
    const auto bulletMatch = bullet.match(text);
    if (bulletMatch.hasMatch()) {
        setFormat(0, bulletMatch.capturedLength(), muted);
        if (bulletMatch.capturedLength(2) == 1) {
            data->bullet = true;
            data->indent = bulletMatch.capturedLength(1);
            conceal(bulletMatch.capturedStart(2), 1);
        }
    }

    QVector<bool> literal(text.size(), false);
    static const QRegularExpression inlineCode(R"((`+)(.+?)\1(?!`))");
    auto codeMatches = inlineCode.globalMatch(text);
    while (codeMatches.hasNext()) {
        const auto match = codeMatches.next();
        setFormat(match.capturedStart(), match.capturedLength(), code);
        for (int i = match.capturedStart(); i < match.capturedEnd(); ++i)
            literal[i] = true;
        conceal(match.capturedStart(), match.capturedLength(1));
        conceal(match.capturedEnd() - match.capturedLength(1), match.capturedLength(1));
    }
    const auto protectedRange = [&](int start, int end) {
        for (int i = start; i < end; ++i)
            if (literal[i])
                return true;
        return false;
    };
    const auto merge = [this](int start, int end, const QTextCharFormat &extra) {
        for (int i = start; i < end; ++i) {
            QTextCharFormat combined = format(i);
            combined.merge(extra);
            setFormat(i, 1, combined);
        }
    };
    const auto delimited = [&](const QRegularExpression &pattern, const QTextCharFormat &extra) {
        auto matches = pattern.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (protectedRange(match.capturedStart(), match.capturedEnd()))
                continue;
            merge(match.capturedStart(), match.capturedEnd(), extra);
            conceal(match.capturedStart(), match.capturedLength(1));
            conceal(match.capturedEnd() - match.capturedLength(1), match.capturedLength(1));
        }
    };
    QTextCharFormat bold;
    bold.setFontWeight(QFont::Bold);
    QTextCharFormat italic;
    italic.setFontItalic(true);
    QTextCharFormat both = bold;
    both.merge(italic);
    static const QRegularExpression tripleStars(R"((?<![\\*])(\*\*\*)(?!\*)(?=\S)(.+?\S|\S)(?<!\\)\1(?!\*))");
    static const QRegularExpression tripleUnderscores(R"((?<![\\\w])(___)(?!_)(?=\S)(.+?\S|\S)(?<!\\)\1(?![\w_]))");
    static const QRegularExpression strongStars(R"((?<![\\*])(\*\*)(?!\*)(?=\S)(.+?\S|\S)(?<!\\)\1(?!\*))");
    static const QRegularExpression strongUnderscores(R"((?<![\\\w])(__)(?!_)(?=\S)(.+?\S|\S)(?<!\\)\1(?![\w_]))");
    static const QRegularExpression emphasisStars(R"((?<![\\*])(\*)(?!\*)(?=\S)(.+?\S|\S)(?<!\\)\1(?!\*))");
    static const QRegularExpression emphasisUnderscores(R"((?<![\\\w])(_)(?!_)(?=\S)(.+?\S|\S)(?<!\\)\1(?![\w_]))");
    delimited(tripleStars, both);
    delimited(tripleUnderscores, both);
    delimited(strongStars, bold);
    delimited(strongUnderscores, bold);
    delimited(emphasisStars, italic);
    delimited(emphasisUnderscores, italic);
    QTextCharFormat strike;
    strike.setFontStrikeOut(true);
    static const QRegularExpression deleted(R"((?<!\\)(~~)(?=\S)(.+?\S|\S)(?<!\\)\1)");
    delimited(deleted, strike);

    QTextCharFormat link;
    link.setForeground(m_theme.accent);
    link.setFontUnderline(true);
    static const QRegularExpression links(R"((?<![\\!])\[([^\]]+)\]\(([^\s]+(?:\s+"[^"]*")?)\))");
    auto linkMatches = links.globalMatch(text);
    while (linkMatches.hasNext()) {
        const auto match = linkMatches.next();
        if (protectedRange(match.capturedStart(), match.capturedEnd()))
            continue;
        merge(match.capturedStart(1), match.capturedEnd(1), link);
        conceal(match.capturedStart(), 1);
        conceal(match.capturedEnd(1), match.capturedEnd() - match.capturedEnd(1));
    }
    finish();
}

MarkdownEditor::MarkdownEditor(const Theme &theme, QWidget *parent)
    : QPlainTextEdit(parent), m_highlighter(new MarkdownHighlighter(document(), theme)), m_theme(theme) {
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &MarkdownEditor::updateSourceRange);
    connect(this, &QPlainTextEdit::selectionChanged, this, &MarkdownEditor::updateSourceRange);
}

void MarkdownEditor::setTheme(const Theme &theme) {
    m_theme = theme;
    m_highlighter->setTheme(theme);
    viewport()->update();
}

void MarkdownEditor::updateSourceRange() {
    const QTextCursor cursor = textCursor();
    if (hasFocus()) {
        const int first = document()->findBlock(cursor.selectionStart()).blockNumber();
        const int end = cursor.hasSelection() ? cursor.selectionEnd() - 1 : cursor.position();
        const int last = document()->findBlock(end).blockNumber();
        m_highlighter->setSourceRange(first, last);
    } else {
        m_highlighter->setSourceRange(-1, -1);
    }
    viewport()->update();
}

void MarkdownEditor::focusInEvent(QFocusEvent *event) {
    QPlainTextEdit::focusInEvent(event);
    updateSourceRange();
}

void MarkdownEditor::focusOutEvent(QFocusEvent *event) {
    QPlainTextEdit::focusOutEvent(event);
    m_highlighter->setSourceRange(-1, -1);
    viewport()->update();
}

void MarkdownEditor::paintEvent(QPaintEvent *event) {
    QPlainTextEdit::paintEvent(event);
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    QTextBlock block = firstVisibleBlock();
    while (block.isValid()) {
        const QRectF area = blockBoundingGeometry(block).translated(contentOffset());
        if (area.top() > viewport()->height())
            break;
        const auto *data = dynamic_cast<MarkdownBlockData *>(block.userData());
        if (data && !data->sourceVisible && area.bottom() >= 0) {
            const qreal left = contentOffset().x() + document()->documentMargin() +
                               fontMetrics().horizontalAdvance(' ') * data->indent;
            if (data->quote) {
                painter.setPen(QPen(m_theme.muted, 2));
                painter.drawLine(QPointF(left + 1, area.top() + 2), QPointF(left + 1, area.bottom() - 2));
            } else if (data->bullet) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_theme.muted);
                painter.drawEllipse(QPointF(left + 3, area.top() + fontMetrics().height() / 2.0), 1.6, 1.6);
            }
        }
        block = block.next();
    }
}
