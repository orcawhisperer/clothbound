#include "audio/speechcleaner.h"

#include "audio/speech.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QtGlobal>

namespace {

const QHash<QString, QString> &spokenLatexSymbols() {
    static const QHash<QString, QString> syms{
        // Greek lowercase
        {QStringLiteral("alpha"), QStringLiteral("alpha")},
        {QStringLiteral("beta"), QStringLiteral("beta")},
        {QStringLiteral("gamma"), QStringLiteral("gamma")},
        {QStringLiteral("delta"), QStringLiteral("delta")},
        {QStringLiteral("epsilon"), QStringLiteral("epsilon")},
        {QStringLiteral("varepsilon"), QStringLiteral("epsilon")},
        {QStringLiteral("zeta"), QStringLiteral("zeta")},
        {QStringLiteral("eta"), QStringLiteral("eta")},
        {QStringLiteral("theta"), QStringLiteral("theta")},
        {QStringLiteral("vartheta"), QStringLiteral("theta")},
        {QStringLiteral("iota"), QStringLiteral("iota")},
        {QStringLiteral("kappa"), QStringLiteral("kappa")},
        {QStringLiteral("varkappa"), QStringLiteral("kappa")},
        {QStringLiteral("lambda"), QStringLiteral("lambda")},
        {QStringLiteral("mu"), QStringLiteral("mu")},
        {QStringLiteral("nu"), QStringLiteral("nu")},
        {QStringLiteral("xi"), QStringLiteral("xi")},
        {QStringLiteral("omicron"), QStringLiteral("omicron")},
        {QStringLiteral("pi"), QStringLiteral("pi")},
        {QStringLiteral("varpi"), QStringLiteral("pi")},
        {QStringLiteral("rho"), QStringLiteral("rho")},
        {QStringLiteral("varrho"), QStringLiteral("rho")},
        {QStringLiteral("sigma"), QStringLiteral("sigma")},
        {QStringLiteral("varsigma"), QStringLiteral("sigma")},
        {QStringLiteral("tau"), QStringLiteral("tau")},
        {QStringLiteral("upsilon"), QStringLiteral("upsilon")},
        {QStringLiteral("phi"), QStringLiteral("phi")},
        {QStringLiteral("varphi"), QStringLiteral("phi")},
        {QStringLiteral("chi"), QStringLiteral("chi")},
        {QStringLiteral("psi"), QStringLiteral("psi")},
        {QStringLiteral("omega"), QStringLiteral("omega")},
        // Greek uppercase
        {QStringLiteral("Alpha"), QStringLiteral("Alpha")},
        {QStringLiteral("Beta"), QStringLiteral("Beta")},
        {QStringLiteral("Gamma"), QStringLiteral("Gamma")},
        {QStringLiteral("Delta"), QStringLiteral("Delta")},
        {QStringLiteral("Epsilon"), QStringLiteral("Epsilon")},
        {QStringLiteral("Zeta"), QStringLiteral("Zeta")},
        {QStringLiteral("Eta"), QStringLiteral("Eta")},
        {QStringLiteral("Theta"), QStringLiteral("Theta")},
        {QStringLiteral("Iota"), QStringLiteral("Iota")},
        {QStringLiteral("Kappa"), QStringLiteral("Kappa")},
        {QStringLiteral("Lambda"), QStringLiteral("Lambda")},
        {QStringLiteral("Mu"), QStringLiteral("Mu")},
        {QStringLiteral("Nu"), QStringLiteral("Nu")},
        {QStringLiteral("Xi"), QStringLiteral("Xi")},
        {QStringLiteral("Omicron"), QStringLiteral("Omicron")},
        {QStringLiteral("Pi"), QStringLiteral("Pi")},
        {QStringLiteral("Rho"), QStringLiteral("Rho")},
        {QStringLiteral("Sigma"), QStringLiteral("Sigma")},
        {QStringLiteral("Tau"), QStringLiteral("Tau")},
        {QStringLiteral("Upsilon"), QStringLiteral("Upsilon")},
        {QStringLiteral("Phi"), QStringLiteral("Phi")},
        {QStringLiteral("Chi"), QStringLiteral("Chi")},
        {QStringLiteral("Psi"), QStringLiteral("Psi")},
        {QStringLiteral("Omega"), QStringLiteral("Omega")},
        // Calculus & operators
        {QStringLiteral("nabla"), QStringLiteral("gradient")},
        {QStringLiteral("partial"), QStringLiteral("partial")},
        {QStringLiteral("sum"), QStringLiteral("sum")},
        {QStringLiteral("prod"), QStringLiteral("product")},
        {QStringLiteral("int"), QStringLiteral("integral")},
        {QStringLiteral("iint"), QStringLiteral("double integral")},
        {QStringLiteral("iiint"), QStringLiteral("triple integral")},
        {QStringLiteral("oint"), QStringLiteral("contour integral")},
        {QStringLiteral("infty"), QStringLiteral("infinity")},
        // Relations & logic
        {QStringLiteral("approx"), QStringLiteral("approximately")},
        {QStringLiteral("sim"), QStringLiteral("similar to")},
        {QStringLiteral("simeq"), QStringLiteral("approximately equal to")},
        {QStringLiteral("cong"), QStringLiteral("congruent to")},
        {QStringLiteral("equiv"), QStringLiteral("equivalent to")},
        {QStringLiteral("propto"), QStringLiteral("proportional to")},
        {QStringLiteral("le"), QStringLiteral("less than or equal to")},
        {QStringLiteral("leq"), QStringLiteral("less than or equal to")},
        {QStringLiteral("ge"), QStringLiteral("greater than or equal to")},
        {QStringLiteral("geq"), QStringLiteral("greater than or equal to")},
        {QStringLiteral("ne"), QStringLiteral("not equal to")},
        {QStringLiteral("neq"), QStringLiteral("not equal to")},
        {QStringLiteral("in"), QStringLiteral("in")},
        {QStringLiteral("notin"), QStringLiteral("not in")},
        {QStringLiteral("subset"), QStringLiteral("subset of")},
        {QStringLiteral("subseteq"), QStringLiteral("subset of")},
        {QStringLiteral("supset"), QStringLiteral("superset of")},
        {QStringLiteral("supseteq"), QStringLiteral("superset of")},
        {QStringLiteral("cup"), QStringLiteral("union")},
        {QStringLiteral("cap"), QStringLiteral("intersection")},
        {QStringLiteral("emptyset"), QStringLiteral("empty set")},
        {QStringLiteral("varnothing"), QStringLiteral("empty set")},
        {QStringLiteral("forall"), QStringLiteral("for all")},
        {QStringLiteral("exists"), QStringLiteral("there exists")},
        {QStringLiteral("nexists"), QStringLiteral("there does not exist")},
        {QStringLiteral("neg"), QStringLiteral("not")},
        {QStringLiteral("lnot"), QStringLiteral("not")},
        {QStringLiteral("land"), QStringLiteral("and")},
        {QStringLiteral("lor"), QStringLiteral("or")},
        // Arrows
        {QStringLiteral("to"), QStringLiteral("to")},
        {QStringLiteral("rightarrow"), QStringLiteral("to")},
        {QStringLiteral("longrightarrow"), QStringLiteral("to")},
        {QStringLiteral("leftarrow"), QStringLiteral("from")},
        {QStringLiteral("leftrightarrow"), QStringLiteral("if and only if")},
        {QStringLiteral("Rightarrow"), QStringLiteral("implies")},
        {QStringLiteral("implies"), QStringLiteral("implies")},
        {QStringLiteral("Leftrightarrow"), QStringLiteral("if and only if")},
        {QStringLiteral("iff"), QStringLiteral("if and only if")},
        {QStringLiteral("mapsto"), QStringLiteral("maps to")},
        // Binary & misc
        {QStringLiteral("times"), QStringLiteral("times")},
        {QStringLiteral("cdot"), QStringLiteral("dot")},
        {QStringLiteral("div"), QStringLiteral("divided by")},
        {QStringLiteral("pm"), QStringLiteral("plus or minus")},
        {QStringLiteral("mp"), QStringLiteral("minus or plus")},
        {QStringLiteral("oplus"), QStringLiteral("direct sum")},
        {QStringLiteral("otimes"), QStringLiteral("tensor product")},
        {QStringLiteral("perp"), QStringLiteral("perpendicular to")},
        {QStringLiteral("parallel"), QStringLiteral("parallel to")},
        {QStringLiteral("ell"), QStringLiteral("l")},
        {QStringLiteral("hbar"), QStringLiteral("h bar")},
        {QStringLiteral("degree"), QStringLiteral("degrees")},
        {QStringLiteral("cdots"), QStringLiteral(" ")},
        {QStringLiteral("ldots"), QStringLiteral(" ")},
        {QStringLiteral("dots"), QStringLiteral(" ")},
        {QStringLiteral("quad"), QStringLiteral(" ")},
        {QStringLiteral("qquad"), QStringLiteral(" ")},
    };
    return syms;
}

class LatexSpeechCleaner {
public:
    explicit LatexSpeechCleaner(const QString &src) : m_src(src) {}

    QString parseAll(int depth = 0) {
        if (depth > 24) {
            const qsizetype start = m_pos;
            int braces = 0;
            while (m_pos < m_src.size()) {
                if (m_src[m_pos] == QLatin1Char('{'))
                    ++braces;
                else if (m_src[m_pos] == QLatin1Char('}')) {
                    if (braces == 0)
                        break;
                    --braces;
                }
                ++m_pos;
            }
            return m_src.mid(start, m_pos - start);
        }
        QString out;
        while (m_pos < m_src.size()) {
            if (m_src[m_pos] == QLatin1Char('}'))
                break;
            appendPiece(out, parseAtom(depth));
        }
        return out;
    }

    QString parseCommandAt(qsizetype &pos, int depth = 0) {
        m_pos = pos;
        const QString out = parseAtom(depth);
        pos = m_pos;
        return out;
    }

private:
    static void appendPiece(QString &out, const QString &piece) {
        if (piece.isEmpty())
            return;
        if (!out.isEmpty()) {
            const QChar prev = out.back();
            const QChar next = piece.front();
            const bool needSpace = (prev.isLetterOrNumber() && next.isLetterOrNumber()) ||
                                   (prev == QLatin1Char(')') && next.isLetterOrNumber());
            if (needSpace)
                out += QLatin1Char(' ');
        }
        out += piece;
    }

    void skipSpaces() {
        while (m_pos < m_src.size() && m_src[m_pos].isSpace())
            ++m_pos;
    }

    QString readRawGroup() {
        skipSpaces();
        if (m_pos >= m_src.size())
            return {};
        if (m_src[m_pos] != QLatin1Char('{'))
            return QString(m_src[m_pos++]);
        ++m_pos;
        int depth = 1;
        const qsizetype start = m_pos;
        while (m_pos < m_src.size() && depth > 0) {
            if (m_src[m_pos] == QLatin1Char('\\') && m_pos + 1 < m_src.size()) {
                m_pos += 2;
                continue;
            }
            if (m_src[m_pos] == QLatin1Char('{'))
                ++depth;
            else if (m_src[m_pos] == QLatin1Char('}'))
                --depth;
            if (depth > 0)
                ++m_pos;
        }
        const QString inner = m_src.mid(start, m_pos - start);
        if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('}'))
            ++m_pos;
        return inner;
    }

    QString parseGroupOrAtom(int depth) {
        skipSpaces();
        if (m_pos >= m_src.size())
            return {};
        if (m_src[m_pos] == QLatin1Char('{')) {
            ++m_pos;
            const QString inner = parseAll(depth + 1);
            if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('}'))
                ++m_pos;
            return inner.trimmed();
        }
        return parseBase(depth + 1).trimmed();
    }

    QString parseAtom(int depth) {
        QString base = parseBase(depth);
        QString sub;
        QString sup;
        for (int pass = 0; pass < 2; ++pass) {
            const qsizetype saved = m_pos;
            skipSpaces();
            if (m_pos < m_src.size() && (m_src[m_pos] == QLatin1Char('^') || m_src[m_pos] == QLatin1Char('_'))) {
                const bool isSup = m_src[m_pos++] == QLatin1Char('^');
                const QString script = parseGroupOrAtom(depth + 1);
                if (isSup)
                    sup = script;
                else
                    sub = script;
            } else {
                m_pos = saved;
                break;
            }
        }
        const bool isBigOp = base == QLatin1String("sum") || base == QLatin1String("product") ||
                             base.endsWith(QLatin1String("integral"));
        if (isBigOp && (!sub.isEmpty() || !sup.isEmpty())) {
            if (!sub.isEmpty())
                base += QStringLiteral(" from ") + sub;
            if (!sup.isEmpty())
                base += QStringLiteral(" to ") + sup;
            return base;
        }
        if (!sub.isEmpty())
            appendPiece(base, sub);
        if (!sup.isEmpty()) {
            if (sup == QLatin1String("2"))
                appendPiece(base, QStringLiteral("squared"));
            else if (sup == QLatin1String("3"))
                appendPiece(base, QStringLiteral("cubed"));
            else
                appendPiece(base, QStringLiteral("to the power of ") + sup);
        }
        return base;
    }

    QString parseBase(int depth) {
        if (m_pos >= m_src.size() || depth > 24)
            return {};
        const QChar c = m_src[m_pos];
        if (c == QLatin1Char('{')) {
            ++m_pos;
            const QString inner = parseAll(depth + 1);
            if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('}'))
                ++m_pos;
            return inner;
        }
        if (c == QLatin1Char('\\'))
            return parseCommand(depth);
        if (c == QLatin1Char('^') || c == QLatin1Char('_'))
            return {};
        if (c == QLatin1Char('&') || c == QLatin1Char('~')) {
            ++m_pos;
            return QStringLiteral(" ");
        }
        if (c.isSpace()) {
            while (m_pos < m_src.size() && m_src[m_pos].isSpace())
                ++m_pos;
            return QStringLiteral(" ");
        }
        ++m_pos;
        return QString(c);
    }

    QString parseCommand(int depth) {
        ++m_pos;
        if (m_pos >= m_src.size())
            return {};
        const QChar next = m_src[m_pos];
        if (!next.isLetter()) {
            ++m_pos;
            switch (next.unicode()) {
            case '\\':
            case ',':
            case ';':
            case ':':
            case ' ':
            case '!':
                return QStringLiteral(" ");
            case '%':
            case '$':
            case '#':
                return QString(next);
            case '&':
                return QStringLiteral(" and ");
            default:
                return QStringLiteral(" ");
            }
        }
        const qsizetype start = m_pos;
        while (m_pos < m_src.size() && m_src[m_pos].isLetter())
            ++m_pos;
        const QString cmd = m_src.mid(start, m_pos - start);

        if (cmd == QLatin1String("frac") || cmd == QLatin1String("dfrac") || cmd == QLatin1String("tfrac")) {
            const QString num = parseGroupOrAtom(depth + 1).trimmed();
            const QString den = parseGroupOrAtom(depth + 1).trimmed();
            if (!num.isEmpty() && !den.isEmpty())
                return num + QStringLiteral(" over ") + den;
            return num + den;
        }
        if (cmd == QLatin1String("binom") || cmd == QLatin1String("dbinom") || cmd == QLatin1String("tbinom")) {
            const QString n = parseGroupOrAtom(depth + 1).trimmed();
            const QString k = parseGroupOrAtom(depth + 1).trimmed();
            return n + QStringLiteral(" choose ") + k;
        }
        if (cmd == QLatin1String("sqrt")) {
            QString index;
            skipSpaces();
            if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('[')) {
                ++m_pos;
                const qsizetype idxStart = m_pos;
                while (m_pos < m_src.size() && m_src[m_pos] != QLatin1Char(']'))
                    ++m_pos;
                index = LatexSpeechCleaner(m_src.mid(idxStart, m_pos - idxStart)).parseAll(depth + 1).trimmed();
                if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char(']'))
                    ++m_pos;
            }
            const QString body = parseGroupOrAtom(depth + 1).trimmed();
            if (index == QLatin1String("3"))
                return QStringLiteral("cube root of ") + body;
            if (!index.isEmpty())
                return index + QStringLiteral("th root of ") + body;
            return QStringLiteral("square root of ") + body;
        }
        if (cmd == QLatin1String("text") || cmd == QLatin1String("mathrm") || cmd == QLatin1String("textrm") ||
            cmd == QLatin1String("operatorname") || cmd == QLatin1String("mathsf") || cmd == QLatin1String("mathtt") ||
            cmd == QLatin1String("mathbf") || cmd == QLatin1String("textbf") || cmd == QLatin1String("bm") ||
            cmd == QLatin1String("boldsymbol") || cmd == QLatin1String("mathit") || cmd == QLatin1String("mathbb") ||
            cmd == QLatin1String("mathcal") || cmd == QLatin1String("mathscr")) {
            return LatexSpeechCleaner(readRawGroup()).parseAll(depth + 1).trimmed();
        }
        if (cmd == QLatin1String("hat") || cmd == QLatin1String("widehat"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral(" hat");
        if (cmd == QLatin1String("bar") || cmd == QLatin1String("overline"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral(" bar");
        if (cmd == QLatin1String("tilde") || cmd == QLatin1String("widetilde"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral(" tilde");
        if (cmd == QLatin1String("vec"))
            return QStringLiteral("vector ") + parseGroupOrAtom(depth + 1);
        if (cmd == QLatin1String("dot") || cmd == QLatin1String("ddot"))
            return parseGroupOrAtom(depth + 1);
        if (cmd == QLatin1String("begin") || cmd == QLatin1String("end")) {
            (void)readRawGroup();
            return QStringLiteral(" ");
        }
        if (cmd == QLatin1String("left") || cmd == QLatin1String("right") || cmd == QLatin1String("big") ||
            cmd == QLatin1String("Big") || cmd == QLatin1String("bigg") || cmd == QLatin1String("Bigg") ||
            cmd == QLatin1String("limits") || cmd == QLatin1String("nolimits") ||
            cmd == QLatin1String("displaystyle") || cmd == QLatin1String("textstyle")) {
            skipSpaces();
            if ((cmd == QLatin1String("left") || cmd == QLatin1String("right")) && m_pos < m_src.size() &&
                m_src[m_pos] == QLatin1Char('.'))
                ++m_pos;
            return {};
        }
        if (const auto it = spokenLatexSymbols().constFind(cmd); it != spokenLatexSymbols().constEnd())
            return *it;
        return cmd;
    }

    QString m_src;
    qsizetype m_pos = 0;
};

bool looksLikeCurrencySpan(const QString &inner) {
    if (inner.isEmpty())
        return true;
    if (inner[0].isDigit() && inner.contains(QLatin1Char(' '))) {
        static const QRegularExpression mathOps(QStringLiteral("[\\\\^_=<>+*/{}\\[\\]()]"));
        if (!mathOps.match(inner).hasMatch())
            return true;
    }
    return false;
}

bool isKnownBareLatexCommand(const QString &cmd) {
    static const QSet<QString> structural{
        QStringLiteral("frac"),    QStringLiteral("dfrac"),   QStringLiteral("tfrac"),   QStringLiteral("sqrt"),
        QStringLiteral("binom"),   QStringLiteral("dbinom"),  QStringLiteral("tbinom"),  QStringLiteral("text"),
        QStringLiteral("mathrm"),  QStringLiteral("textrm"),  QStringLiteral("mathbf"),  QStringLiteral("textbf"),
        QStringLiteral("mathbb"),  QStringLiteral("mathcal"), QStringLiteral("mathscr"), QStringLiteral("operatorname"),
        QStringLiteral("hat"),     QStringLiteral("widehat"), QStringLiteral("bar"),     QStringLiteral("overline"),
        QStringLiteral("tilde"),   QStringLiteral("vec"),     QStringLiteral("left"),    QStringLiteral("right"),
        QStringLiteral("begin"),   QStringLiteral("end"),
    };
    return structural.contains(cmd) || spokenLatexSymbols().contains(cmd);
}

}  // namespace

namespace SpeechCleaner {

QString cleanLatexForSpeech(const QString &text) {
    QString out;
    out.reserve(text.size());
    const qsizetype n = text.size();
    qsizetype i = 0;
    bool inInlineCode = false;

    auto appendSpaced = [&out](const QString &spoken) {
        const QString trimmed = spoken.trimmed();
        if (trimmed.isEmpty())
            return;
        if (!out.isEmpty() && !out.back().isSpace() && out.back() != QLatin1Char('(') && out.back() != QLatin1Char('['))
            out += QLatin1Char(' ');
        out += trimmed;
    };

    while (i < n) {
        if (text[i] == QLatin1Char('`')) {
            inInlineCode = !inInlineCode;
            out += text[i++];
            continue;
        }
        if (inInlineCode) {
            out += text[i++];
            continue;
        }
        const bool escaped = (i > 0 && text[i - 1] == QLatin1Char('\\'));

        // Display math: $$...$$
        if (!escaped && i + 1 < n && text[i] == QLatin1Char('$') && text[i + 1] == QLatin1Char('$')) {
            const qsizetype close = text.indexOf(QStringLiteral("$$"), i + 2);
            if (close > i + 2) {
                appendSpaced(LatexSpeechCleaner(text.mid(i + 2, close - (i + 2))).parseAll());
                i = close + 2;
                continue;
            }
            // Stray unmatched $$
            i += 2;
            continue;
        }
        // Display math: \[...\]
        if (!escaped && i + 1 < n && text[i] == QLatin1Char('\\') && text[i + 1] == QLatin1Char('[')) {
            const qsizetype close = text.indexOf(QStringLiteral("\\]"), i + 2);
            if (close > i + 2) {
                appendSpaced(LatexSpeechCleaner(text.mid(i + 2, close - (i + 2))).parseAll());
                i = close + 2;
                continue;
            }
        }
        // Inline math: \(...\)
        if (!escaped && i + 1 < n && text[i] == QLatin1Char('\\') && text[i + 1] == QLatin1Char('(')) {
            const qsizetype close = text.indexOf(QStringLiteral("\\)"), i + 2);
            if (close > i + 2) {
                appendSpaced(LatexSpeechCleaner(text.mid(i + 2, close - (i + 2))).parseAll());
                i = close + 2;
                continue;
            }
        }
        // Inline math: $...$ (preserving currency spans like "$5 to $10")
        if (!escaped && text[i] == QLatin1Char('$') && i + 1 < n && text[i + 1] != QLatin1Char('$') &&
            !text[i + 1].isSpace()) {
            qsizetype j = i + 1;
            qsizetype close = -1;
            while (j < n) {
                if (text[j] == QLatin1Char('\n') && j + 1 < n && text[j + 1] == QLatin1Char('\n'))
                    break;
                if (text[j] == QLatin1Char('`'))
                    break;
                if (text[j] == QLatin1Char('$') && text[j - 1] != QLatin1Char('\\')) {
                    if (!text[j - 1].isSpace() &&
                        (j + 1 >= n || (!text[j + 1].isDigit() && text[j + 1] != QLatin1Char('$')))) {
                        close = j;
                        break;
                    }
                }
                ++j;
            }
            if (close > i + 1) {
                const QString inner = text.mid(i + 1, close - (i + 1));
                if (!looksLikeCurrencySpan(inner)) {
                    appendSpaced(LatexSpeechCleaner(inner).parseAll());
                    i = close + 1;
                    continue;
                }
            }
        }
        // Bare LaTeX command outside math delimiters (e.g. "\frac{a}{b}", "\alpha", "\nabla", "\sqrt{x}").
        if (text[i] == QLatin1Char('\\') && i + 1 < n && text[i + 1].isLetter()) {
            qsizetype end = i + 1;
            while (end < n && text[end].isLetter())
                ++end;
            const QString cmd = text.mid(i + 1, end - (i + 1));
            if (isKnownBareLatexCommand(cmd)) {
                LatexSpeechCleaner cleaner(text);
                qsizetype pos = i;
                const QString spoken = cleaner.parseCommandAt(pos);
                appendSpaced(spoken);
                i = pos;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

QString cleanSpeechText(const QString &markdownOrText) {
    if (markdownOrText.trimmed().isEmpty())
        return {};

    QString text = Speech::speakable(markdownOrText);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    // 1. Remove Markdown fenced code block markers (```lang / ~~~lang), keeping readable content.
    static const QRegularExpression codeFenceLine(QStringLiteral("(?m)^[ \\t]*(?:`{3,}|~{3,})[^\\n]*$"));
    text.replace(codeFenceLine, QStringLiteral("\n"));

    // 2. Clean LaTeX expressions ($...$, $$...$$, \(...\), \[...\], and bare commands) before stripping _.
    text = cleanLatexForSpeech(text);

    // 3. Strip inline code backticks (`code` / ``code`` -> code).
    static const QRegularExpression doubleTick(QStringLiteral("``([^`]+)``"));
    static const QRegularExpression singleTick(QStringLiteral("`([^`]+)`"));
    text.replace(doubleTick, QStringLiteral("\\1"));
    text.replace(singleTick, QStringLiteral("\\1"));

    // 4. Strip HTML tags (<sup>, <span>, etc.) and decode common entities.
    static const QRegularExpression htmlBreaks(QStringLiteral("<\\s*(?:br\\s*/?|/(?:p|div|li|tr|h[1-6]))\\s*>"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression htmlTag(QStringLiteral("</?[a-zA-Z][^>]*>"));
    text.replace(htmlBreaks, QStringLiteral("\n"));
    text.remove(htmlTag);
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&mdash;"), QStringLiteral("\u2014"));
    text.replace(QStringLiteral("&ndash;"), QStringLiteral("\u2013"));
    text.replace(QStringLiteral("&hellip;"), QStringLiteral("..."));

    // 5. Markdown images & links: ![alt](url) -> alt, [label](url) -> label.
    static const QRegularExpression mdImage(QStringLiteral("!\\[([^\\]]*)\\]\\((?:[^()\\n]|\\([^()\\n]*\\))*\\)"));
    static const QRegularExpression mdLink(QStringLiteral("\\[([^\\]]+)\\]\\((?:[^()\\n]|\\([^()\\n]*\\))*\\)"));
    static const QRegularExpression mdRefLink(QStringLiteral("\\[([^\\]]+)\\]\\[[^\\]]*\\]"));
    text.replace(mdImage, QStringLiteral("\\1"));
    text.replace(mdLink, QStringLiteral("\\1"));
    text.replace(mdRefLink, QStringLiteral("\\1"));

    // 6. Strip citation brackets: [1], [2, 3], [1-4], [p. 4], [pp. 12-15], [Ch. 3], [§2.1].
    static const QRegularExpression numCite(QStringLiteral("\\[\\s*\\d+(?:\\s*[-\u2013\u2014,]\\s*\\d+)*\\s*\\]"));
    static const QRegularExpression pageCite(
        QStringLiteral("\\[\\s*(?:p{1,2}\\.?|pages?|ch\\.?|chap\\.?|chapter|sec\\.?|section|\\x{00A7}|fig\\.?|eq\\.?)"
                       "\\s*[\\dIVXLCivxlc]+(?:[^\\]]{0,24})?\\]"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(numCite);
    text.remove(pageCite);

    // 7. Line-level Markdown markers (headings, blockquotes, list bullets, horizontal rules, table bars).
    static const QRegularExpression hrLine(QStringLiteral("^[ \\t]*(?:[-*_][ \\t]*){3,}$"));
    static const QRegularExpression tableSepLine(
        QStringLiteral("^[ \\t]*\\|?(?:[ \\t]*:?-{3,}:?[ \\t]*\\|)+[ \\t]*:?-{3,}:?[ \\t]*\\|?[ \\t]*$"));
    static const QRegularExpression blockquotePrefix(QStringLiteral("^[ \\t]*(?:>[ \\t]*)+"));
    static const QRegularExpression headingPrefix(QStringLiteral("^[ \\t]{0,3}#{1,6}[ \\t]+"));
    static const QRegularExpression headingSuffix(QStringLiteral("[ \\t]+#+[ \\t]*$"));
    static const QRegularExpression bulletPrefix(QStringLiteral("^[ \\t]*[-*+\u2022][ \\t]+"));

    // Inline emphasis & strikethrough patterns.
    static const QRegularExpression boldItalicStar(QStringLiteral("\\*{3}(?=\\S)(.+?)(?<=\\S)\\*{3}"));
    static const QRegularExpression boldItalicUnder(QStringLiteral("(?<![\\p{L}\\p{N}])_{3}(?=\\S)(.+?)(?<=\\S)_{3}(?![\\p{L}\\p{N}])"));
    static const QRegularExpression boldStar(QStringLiteral("\\*{2}(?=\\S)(.+?)(?<=\\S)\\*{2}"));
    static const QRegularExpression boldUnder(QStringLiteral("(?<![\\p{L}\\p{N}])_{2}(?=\\S)(.+?)(?<=\\S)_{2}(?![\\p{L}\\p{N}])"));
    static const QRegularExpression strikeRx(QStringLiteral("~~(?=\\S)(.+?)(?<=\\S)~~"));
    static const QRegularExpression italicStar(QStringLiteral("(?<!\\*)\\*(?=\\S)([^*\\n]+?)(?<=\\S)\\*(?!\\*)"));
    static const QRegularExpression italicUnder(
        QStringLiteral("(?<![\\p{L}\\p{N}_])_(?=\\S)([^_\\n]+?)(?<=\\S)_(?![\\p{L}\\p{N}_])"));
    static const QRegularExpression strayMarkers(QStringLiteral("\\*{2,}|_{2,}|~~|`+"));
    static const QRegularExpression mdEscape(QStringLiteral("\\\\([*_#\\[\\]()>~`\\\\])"));
    static const QRegularExpression multiSpace(QStringLiteral("[ \\t]+"));
    static const QRegularExpression spaceBeforePunct(QStringLiteral("[ \\t]+([,.;:!?])"));
    static const QRegularExpression openParenSpace(QStringLiteral("([(\\[])[ \\t]+"));
    static const QRegularExpression closeParenSpace(QStringLiteral("[ \\t]+([)\\]])"));

    const QStringList rawLines = text.split(QLatin1Char('\n'));
    QStringList cleanedLines;
    cleanedLines.reserve(rawLines.size());
    for (QString line : rawLines) {
        if (hrLine.match(line).hasMatch() || tableSepLine.match(line).hasMatch())
            continue;
        line.remove(blockquotePrefix);
        line.remove(headingPrefix);
        line.remove(headingSuffix);
        line.remove(bulletPrefix);
        if (line.contains(QLatin1Char('|'))) {
            QString trimmedPipe = line.trimmed();
            if (trimmedPipe.startsWith(QLatin1Char('|')))
                trimmedPipe.remove(0, 1);
            if (trimmedPipe.endsWith(QLatin1Char('|')))
                trimmedPipe.chop(1);
            line = trimmedPipe.replace(QLatin1Char('|'), QStringLiteral(", "));
        }
        line.replace(boldItalicStar, QStringLiteral("\\1"));
        line.replace(boldItalicUnder, QStringLiteral("\\1"));
        line.replace(boldStar, QStringLiteral("\\1"));
        line.replace(boldUnder, QStringLiteral("\\1"));
        line.replace(strikeRx, QStringLiteral("\\1"));
        line.replace(italicStar, QStringLiteral("\\1"));
        line.replace(italicUnder, QStringLiteral("\\1"));
        line.remove(strayMarkers);
        line.replace(mdEscape, QStringLiteral("\\1"));
        line.replace(multiSpace, QStringLiteral(" "));
        line.replace(spaceBeforePunct, QStringLiteral("\\1"));
        line.replace(openParenSpace, QStringLiteral("\\1"));
        line.replace(closeParenSpace, QStringLiteral("\\1"));
        line = line.trimmed();
        if (!line.isEmpty())
            cleanedLines.append(line);
    }

    return cleanedLines.join(QLatin1Char('\n')).trimmed();
}

QStringList splitOverlongSentence(const QString &sentence, qsizetype maxChars) {
    QStringList pieces;
    if (maxChars <= 0) {
        if (!sentence.trimmed().isEmpty())
            pieces.append(sentence.trimmed());
        return pieces;
    }
    if (sentence.size() <= maxChars) {
        if (!sentence.trimmed().isEmpty())
            pieces.append(sentence.trimmed());
        return pieces;
    }
    const qsizetype len = sentence.size();
    qsizetype pos = 0;
    while (pos < len) {
        qsizetype take = qMin(maxChars, len - pos);
        if (pos + take < len) {
            qsizetype cut = -1;
            for (qsizetype j = pos + take; j > pos + maxChars / 2; --j) {
                const QChar ch = sentence[j - 1];
                if (ch == QLatin1Char(';') || ch == QLatin1Char(',') || ch == QChar(0x2014) || ch == QChar(0xFF0C) ||
                    ch == QChar(0x3001)) {
                    cut = j;
                    break;
                }
            }
            if (cut < 0)
                for (qsizetype j = pos + take; j > pos + maxChars / 2; --j)
                    if (sentence[j - 1].isSpace()) {
                        cut = j;
                        break;
                    }
            if (cut > pos)
                take = cut - pos;
        }
        const QString part = sentence.mid(pos, take).trimmed();
        if (!part.isEmpty())
            pieces.append(part);
        pos += take;
        while (pos < len && sentence[pos].isSpace())
            ++pos;
    }
    return pieces;
}

}  // namespace SpeechCleaner
