#include "ai/mathformatter.h"

#include <QHash>
#include <QList>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextDocument>

namespace {

QString escapeHtmlText(const QString &s) {
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        switch (c.unicode()) {
        case '&': out += QStringLiteral("&amp;"); break;
        case '<': out += QStringLiteral("&lt;"); break;
        case '>': out += QStringLiteral("&gt;"); break;
        case '"': out += QStringLiteral("&quot;"); break;
        default: out += c; break;
        }
    }
    return out;
}

QString mapBlackboard(const QString &s) {
    static const QHash<QChar, QString> table = {
        {'R', QStringLiteral("\u211D")}, {'N', QStringLiteral("\u2115")}, {'Z', QStringLiteral("\u2124")},
        {'Q', QStringLiteral("\u211A")}, {'C', QStringLiteral("\u2102")}, {'E', QStringLiteral("\U0001D53C")},
        {'P', QStringLiteral("\u2119")}, {'F', QStringLiteral("\U0001D53D")}, {'H', QStringLiteral("\u210D")},
        {'I', QStringLiteral("\U0001D540")}, {'K', QStringLiteral("\U0001D542")}, {'S', QStringLiteral("\U0001D54A")},
        {'T', QStringLiteral("\U0001D54B")}, {'1', QStringLiteral("\U0001D7D9")},
    };
    QString out;
    for (QChar c : s)
        out += table.value(c, escapeHtmlText(QString(c)));
    return QStringLiteral("<span style=\"font-style:normal;\">") + out + QStringLiteral("</span>");
}

QString mapCalligraphic(const QString &s) {
    static const QHash<QChar, QString> table = {
        {'A', QStringLiteral("\U0001D49C")}, {'B', QStringLiteral("\u212C")},     {'C', QStringLiteral("\U0001D49E")},
        {'D', QStringLiteral("\U0001D49F")}, {'E', QStringLiteral("\u2130")},     {'F', QStringLiteral("\u2131")},
        {'G', QStringLiteral("\U0001D4A2")}, {'H', QStringLiteral("\u210B")},     {'I', QStringLiteral("\u2110")},
        {'J', QStringLiteral("\U0001D4A5")}, {'K', QStringLiteral("\U0001D4A6")}, {'L', QStringLiteral("\u2112")},
        {'M', QStringLiteral("\u2133")},     {'N', QStringLiteral("\U0001D4A9")}, {'O', QStringLiteral("\U0001D4AA")},
        {'P', QStringLiteral("\U0001D4AB")}, {'Q', QStringLiteral("\U0001D4AC")}, {'R', QStringLiteral("\u211B")},
        {'S', QStringLiteral("\U0001D4AE")}, {'T', QStringLiteral("\U0001D4AF")}, {'U', QStringLiteral("\U0001D4B0")},
        {'V', QStringLiteral("\U0001D4B1")}, {'W', QStringLiteral("\U0001D4B2")}, {'X', QStringLiteral("\U0001D4B3")},
        {'Y', QStringLiteral("\U0001D4B4")}, {'Z', QStringLiteral("\U0001D4B5")},
    };
    QString out;
    for (QChar c : s)
        out += table.value(c, escapeHtmlText(QString(c)));
    return QStringLiteral("<span style=\"font-style:normal;\">") + out + QStringLiteral("</span>");
}

const QHash<QString, QString> &latexSymbols() {
    static const QHash<QString, QString> syms = {
        // Greek lowercase
        {QStringLiteral("alpha"), QStringLiteral("\u03B1")},
        {QStringLiteral("beta"), QStringLiteral("\u03B2")},
        {QStringLiteral("gamma"), QStringLiteral("\u03B3")},
        {QStringLiteral("delta"), QStringLiteral("\u03B4")},
        {QStringLiteral("epsilon"), QStringLiteral("\u03F5")},
        {QStringLiteral("varepsilon"), QStringLiteral("\u03B5")},
        {QStringLiteral("zeta"), QStringLiteral("\u03B6")},
        {QStringLiteral("eta"), QStringLiteral("\u03B7")},
        {QStringLiteral("theta"), QStringLiteral("\u03B8")},
        {QStringLiteral("vartheta"), QStringLiteral("\u03D1")},
        {QStringLiteral("iota"), QStringLiteral("\u03B9")},
        {QStringLiteral("kappa"), QStringLiteral("\u03BA")},
        {QStringLiteral("varkappa"), QStringLiteral("\u03F0")},
        {QStringLiteral("lambda"), QStringLiteral("\u03BB")},
        {QStringLiteral("mu"), QStringLiteral("\u03BC")},
        {QStringLiteral("nu"), QStringLiteral("\u03BD")},
        {QStringLiteral("xi"), QStringLiteral("\u03BE")},
        {QStringLiteral("omicron"), QStringLiteral("\u03BF")},
        {QStringLiteral("pi"), QStringLiteral("\u03C0")},
        {QStringLiteral("varpi"), QStringLiteral("\u03D6")},
        {QStringLiteral("rho"), QStringLiteral("\u03C1")},
        {QStringLiteral("varrho"), QStringLiteral("\u03F1")},
        {QStringLiteral("sigma"), QStringLiteral("\u03C3")},
        {QStringLiteral("varsigma"), QStringLiteral("\u03C2")},
        {QStringLiteral("tau"), QStringLiteral("\u03C4")},
        {QStringLiteral("upsilon"), QStringLiteral("\u03C5")},
        {QStringLiteral("phi"), QStringLiteral("\u03D5")},
        {QStringLiteral("varphi"), QStringLiteral("\u03C6")},
        {QStringLiteral("chi"), QStringLiteral("\u03C7")},
        {QStringLiteral("psi"), QStringLiteral("\u03C8")},
        {QStringLiteral("omega"), QStringLiteral("\u03C9")},
        // Greek uppercase
        {QStringLiteral("Alpha"), QStringLiteral("\u0391")},
        {QStringLiteral("Beta"), QStringLiteral("\u0392")},
        {QStringLiteral("Gamma"), QStringLiteral("\u0393")},
        {QStringLiteral("Delta"), QStringLiteral("\u0394")},
        {QStringLiteral("Epsilon"), QStringLiteral("\u0395")},
        {QStringLiteral("Zeta"), QStringLiteral("\u0396")},
        {QStringLiteral("Eta"), QStringLiteral("\u0397")},
        {QStringLiteral("Theta"), QStringLiteral("\u0398")},
        {QStringLiteral("Iota"), QStringLiteral("\u0399")},
        {QStringLiteral("Kappa"), QStringLiteral("\u039A")},
        {QStringLiteral("Lambda"), QStringLiteral("\u039B")},
        {QStringLiteral("Mu"), QStringLiteral("\u039C")},
        {QStringLiteral("Nu"), QStringLiteral("\u039D")},
        {QStringLiteral("Xi"), QStringLiteral("\u039E")},
        {QStringLiteral("Omicron"), QStringLiteral("\u039F")},
        {QStringLiteral("Pi"), QStringLiteral("\u03A0")},
        {QStringLiteral("Rho"), QStringLiteral("\u03A1")},
        {QStringLiteral("Sigma"), QStringLiteral("\u03A3")},
        {QStringLiteral("Tau"), QStringLiteral("\u03A4")},
        {QStringLiteral("Upsilon"), QStringLiteral("\u03A5")},
        {QStringLiteral("Phi"), QStringLiteral("\u03A6")},
        {QStringLiteral("Chi"), QStringLiteral("\u03A7")},
        {QStringLiteral("Psi"), QStringLiteral("\u03A8")},
        {QStringLiteral("Omega"), QStringLiteral("\u03A9")},
        // Big operators & calculus
        {QStringLiteral("sum"), QStringLiteral("\u2211")},
        {QStringLiteral("prod"), QStringLiteral("\u220F")},
        {QStringLiteral("coprod"), QStringLiteral("\u2210")},
        {QStringLiteral("int"), QStringLiteral("\u222B")},
        {QStringLiteral("iint"), QStringLiteral("\u222C")},
        {QStringLiteral("iiint"), QStringLiteral("\u222D")},
        {QStringLiteral("oint"), QStringLiteral("\u222E")},
        {QStringLiteral("nabla"), QStringLiteral("\u2207")},
        {QStringLiteral("partial"), QStringLiteral("\u2202")},
        {QStringLiteral("infty"), QStringLiteral("\u221E")},
        // Relations & logic
        {QStringLiteral("approx"), QStringLiteral("\u2248")},
        {QStringLiteral("sim"), QStringLiteral("\u223C")},
        {QStringLiteral("simeq"), QStringLiteral("\u2243")},
        {QStringLiteral("cong"), QStringLiteral("\u2245")},
        {QStringLiteral("equiv"), QStringLiteral("\u2261")},
        {QStringLiteral("propto"), QStringLiteral("\u221D")},
        {QStringLiteral("le"), QStringLiteral("\u2264")},
        {QStringLiteral("leq"), QStringLiteral("\u2264")},
        {QStringLiteral("ge"), QStringLiteral("\u2265")},
        {QStringLiteral("geq"), QStringLiteral("\u2265")},
        {QStringLiteral("ll"), QStringLiteral("\u226A")},
        {QStringLiteral("gg"), QStringLiteral("\u226B")},
        {QStringLiteral("ne"), QStringLiteral("\u2260")},
        {QStringLiteral("neq"), QStringLiteral("\u2260")},
        {QStringLiteral("in"), QStringLiteral("\u2208")},
        {QStringLiteral("notin"), QStringLiteral("\u2209")},
        {QStringLiteral("ni"), QStringLiteral("\u220B")},
        {QStringLiteral("subset"), QStringLiteral("\u2282")},
        {QStringLiteral("subseteq"), QStringLiteral("\u2286")},
        {QStringLiteral("supset"), QStringLiteral("\u2283")},
        {QStringLiteral("supseteq"), QStringLiteral("\u2287")},
        {QStringLiteral("cup"), QStringLiteral("\u222A")},
        {QStringLiteral("cap"), QStringLiteral("\u2229")},
        {QStringLiteral("setminus"), QStringLiteral("\u2216")},
        {QStringLiteral("emptyset"), QStringLiteral("\u2205")},
        {QStringLiteral("varnothing"), QStringLiteral("\u2205")},
        {QStringLiteral("forall"), QStringLiteral("\u2200")},
        {QStringLiteral("exists"), QStringLiteral("\u2203")},
        {QStringLiteral("nexists"), QStringLiteral("\u2204")},
        {QStringLiteral("neg"), QStringLiteral("\u00AC")},
        {QStringLiteral("lnot"), QStringLiteral("\u00AC")},
        {QStringLiteral("land"), QStringLiteral("\u2227")},
        {QStringLiteral("wedge"), QStringLiteral("\u2227")},
        {QStringLiteral("lor"), QStringLiteral("\u2228")},
        {QStringLiteral("vee"), QStringLiteral("\u2228")},
        // Arrows
        {QStringLiteral("to"), QStringLiteral("\u2192")},
        {QStringLiteral("rightarrow"), QStringLiteral("\u2192")},
        {QStringLiteral("longrightarrow"), QStringLiteral("\u27F6")},
        {QStringLiteral("leftarrow"), QStringLiteral("\u2190")},
        {QStringLiteral("gets"), QStringLiteral("\u2190")},
        {QStringLiteral("leftrightarrow"), QStringLiteral("\u2194")},
        {QStringLiteral("Rightarrow"), QStringLiteral("\u21D2")},
        {QStringLiteral("implies"), QStringLiteral("\u21D2")},
        {QStringLiteral("Leftarrow"), QStringLiteral("\u21D0")},
        {QStringLiteral("Leftrightarrow"), QStringLiteral("\u21D4")},
        {QStringLiteral("iff"), QStringLiteral("\u21D4")},
        {QStringLiteral("mapsto"), QStringLiteral("\u21A6")},
        {QStringLiteral("uparrow"), QStringLiteral("\u2191")},
        {QStringLiteral("downarrow"), QStringLiteral("\u2193")},
        // Binary & misc symbols
        {QStringLiteral("times"), QStringLiteral("\u00D7")},
        {QStringLiteral("cdot"), QStringLiteral("\u00B7")},
        {QStringLiteral("cdots"), QStringLiteral("\u22EF")},
        {QStringLiteral("ldots"), QStringLiteral("\u2026")},
        {QStringLiteral("dots"), QStringLiteral("\u2026")},
        {QStringLiteral("vdots"), QStringLiteral("\u22EE")},
        {QStringLiteral("ddots"), QStringLiteral("\u22F1")},
        {QStringLiteral("pm"), QStringLiteral("\u00B1")},
        {QStringLiteral("mp"), QStringLiteral("\u2213")},
        {QStringLiteral("div"), QStringLiteral("\u00F7")},
        {QStringLiteral("oplus"), QStringLiteral("\u2295")},
        {QStringLiteral("otimes"), QStringLiteral("\u2297")},
        {QStringLiteral("odot"), QStringLiteral("\u2299")},
        {QStringLiteral("circ"), QStringLiteral("\u2218")},
        {QStringLiteral("bullet"), QStringLiteral("\u2022")},
        {QStringLiteral("star"), QStringLiteral("\u22C6")},
        {QStringLiteral("dagger"), QStringLiteral("\u2020")},
        {QStringLiteral("perp"), QStringLiteral("\u22A5")},
        {QStringLiteral("parallel"), QStringLiteral("\u2225")},
        {QStringLiteral("angle"), QStringLiteral("\u2220")},
        {QStringLiteral("triangle"), QStringLiteral("\u25B3")},
        {QStringLiteral("ell"), QStringLiteral("\u2113")},
        {QStringLiteral("hbar"), QStringLiteral("\u210F")},
        {QStringLiteral("Re"), QStringLiteral("\u211C")},
        {QStringLiteral("Im"), QStringLiteral("\u2111")},
        {QStringLiteral("aleph"), QStringLiteral("\u2135")},
        {QStringLiteral("prime"), QStringLiteral("\u2032")},
        {QStringLiteral("langle"), QStringLiteral("\u27E8")},
        {QStringLiteral("rangle"), QStringLiteral("\u27E9")},
        {QStringLiteral("lceil"), QStringLiteral("\u2308")},
        {QStringLiteral("rceil"), QStringLiteral("\u2309")},
        {QStringLiteral("lfloor"), QStringLiteral("\u230A")},
        {QStringLiteral("rfloor"), QStringLiteral("\u230B")},
        {QStringLiteral("mid"), QStringLiteral("\u2223")},
        {QStringLiteral("top"), QStringLiteral("\u22A4")},
        {QStringLiteral("bot"), QStringLiteral("\u22A5")},
        {QStringLiteral("vdash"), QStringLiteral("\u22A2")},
        {QStringLiteral("models"), QStringLiteral("\u22A8")},
        {QStringLiteral("degree"), QStringLiteral("\u00B0")},
        {QStringLiteral("quad"), QStringLiteral("\u2003")},
        {QStringLiteral("qquad"), QStringLiteral("\u2003\u2003")},
    };
    return syms;
}

const QSet<QString> &uprightFunctions() {
    static const QSet<QString> fns = {
        QStringLiteral("sin"),    QStringLiteral("cos"),     QStringLiteral("tan"),     QStringLiteral("cot"),
        QStringLiteral("sec"),    QStringLiteral("csc"),     QStringLiteral("arcsin"),  QStringLiteral("arccos"),
        QStringLiteral("arctan"), QStringLiteral("sinh"),    QStringLiteral("cosh"),    QStringLiteral("tanh"),
        QStringLiteral("log"),    QStringLiteral("ln"),      QStringLiteral("exp"),     QStringLiteral("lim"),
        QStringLiteral("limsup"), QStringLiteral("liminf"),  QStringLiteral("max"),     QStringLiteral("min"),
        QStringLiteral("sup"),    QStringLiteral("inf"),     QStringLiteral("arg"),     QStringLiteral("argmax"),
        QStringLiteral("argmin"), QStringLiteral("det"),     QStringLiteral("dim"),     QStringLiteral("ker"),
        QStringLiteral("deg"),    QStringLiteral("gcd"),     QStringLiteral("mod"),     QStringLiteral("Pr"),
        QStringLiteral("Var"),    QStringLiteral("Cov"),     QStringLiteral("softmax"), QStringLiteral("relu"),
        QStringLiteral("sigmoid"),
    };
    return fns;
}

class LatexParser {
public:
    explicit LatexParser(const QString &src, bool display) : m_src(src), m_display(display) {}

    QString parseAll(int depth = 0) {
        if (depth > 24) {
            // Advance past the current balanced group in O(N) so deep nesting
            // can never cause quadratic tail re-scanning.
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
            return escapeHtmlText(m_src.mid(start, m_pos - start));
        }
        QString out;
        while (m_pos < m_src.size()) {
            const QChar c = m_src[m_pos];
            if (c == QLatin1Char('}'))
                break;
            out += parseAtom(depth);
        }
        return out;
    }

private:
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
        ++m_pos;  // consume '{'
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
            return inner;
        }
        return parseBase(depth + 1);
    }

    QString parseAtom(int depth) {
        QString base = parseBase(depth);
        // Handle trailing ^ and _ in any order (e.g. x_i^2 or x^2_i).
        for (int pass = 0; pass < 2; ++pass) {
            const qsizetype saved = m_pos;
            skipSpaces();
            if (m_pos < m_src.size() && (m_src[m_pos] == QLatin1Char('^') || m_src[m_pos] == QLatin1Char('_'))) {
                const bool isSup = m_src[m_pos++] == QLatin1Char('^');
                const QString script = parseGroupOrAtom(depth + 1);
                if (!script.isEmpty())
                    base += isSup ? (QStringLiteral("<sup>") + script + QStringLiteral("</sup>"))
                                  : (QStringLiteral("<sub>") + script + QStringLiteral("</sub>"));
            } else {
                m_pos = saved;
                break;
            }
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
        if (c == QLatin1Char('&')) {
            ++m_pos;
            return QStringLiteral("&nbsp;");
        }
        if (c == QLatin1Char('~')) {
            ++m_pos;
            return QStringLiteral("&nbsp;");
        }
        if (c == QLatin1Char('-')) {
            ++m_pos;
            return QStringLiteral("<span style=\"font-style:normal;\">\u2212</span>");
        }
        if (c.isDigit() || c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('[') ||
            c == QLatin1Char(']') || c == QLatin1Char('+') || c == QLatin1Char('=') || c == QLatin1Char(',') ||
            c == QLatin1Char(';') || c == QLatin1Char(':') || c == QLatin1Char('!') || c == QLatin1Char('|') ||
            c == QLatin1Char('/')) {
            ++m_pos;
            return QStringLiteral("<span style=\"font-style:normal;\">") + escapeHtmlText(QString(c)) +
                   QStringLiteral("</span>");
        }
        if (c.isSpace()) {
            while (m_pos < m_src.size() && m_src[m_pos].isSpace())
                ++m_pos;
            return QStringLiteral(" ");
        }
        ++m_pos;
        return escapeHtmlText(QString(c));
    }

    QString parseCommand(int depth) {
        ++m_pos;  // consume '\'
        if (m_pos >= m_src.size())
            return {};
        const QChar next = m_src[m_pos];
        // Punctuation / spacing commands
        if (!next.isLetter()) {
            ++m_pos;
            switch (next.unicode()) {
            case '\\':
                return m_display ? QStringLiteral("<br/>") : QStringLiteral(" ");
            case ',':
                return QStringLiteral("\u2009");
            case ';':
            case ':':
            case ' ':
                return QStringLiteral("\u2005");
            case '!':
                return {};
            case '|':
                return QStringLiteral("<span style=\"font-style:normal;\">\u2016</span>");
            case '{':
            case '}':
            case '%':
            case '_':
            case '&':
            case '$':
            case '#':
                return QStringLiteral("<span style=\"font-style:normal;\">") + escapeHtmlText(QString(next)) +
                       QStringLiteral("</span>");
            default:
                return escapeHtmlText(QString(next));
            }
        }
        const qsizetype start = m_pos;
        while (m_pos < m_src.size() && m_src[m_pos].isLetter())
            ++m_pos;
        const QString cmd = m_src.mid(start, m_pos - start);

        if (cmd == QLatin1String("frac") || cmd == QLatin1String("dfrac") || cmd == QLatin1String("tfrac")) {
            const QString num = parseGroupOrAtom(depth + 1).trimmed();
            const QString den = parseGroupOrAtom(depth + 1).trimmed();
            const bool nested = num.contains(QLatin1String("<sup")) || num.contains(QLatin1String("<sub")) ||
                                den.contains(QLatin1String("<sup")) || den.contains(QLatin1String("<sub")) ||
                                num.contains(QLatin1String("\u2044")) || den.contains(QLatin1String("\u2044"));
            if (nested) {
                auto needsParens = [](const QString &expr) {
                    static const QRegularExpression tagRx(QStringLiteral("<[^>]*>"));
                    QString plain = expr;
                    plain.remove(tagRx);
                    return plain.contains(QLatin1Char('+')) || plain.contains(QLatin1Char('=')) ||
                           plain.contains(QStringLiteral("\u2212"));
                };
                const QString nWrap = needsParens(num)
                                          ? QStringLiteral("<span style=\"font-style:normal;\">(</span>") + num +
                                                QStringLiteral("<span style=\"font-style:normal;\">)</span>")
                                          : num;
                const QString dWrap = needsParens(den)
                                          ? QStringLiteral("<span style=\"font-style:normal;\">(</span>") + den +
                                                QStringLiteral("<span style=\"font-style:normal;\">)</span>")
                                          : den;
                return nWrap + QStringLiteral("<span style=\"font-style:normal;\"> \u2044 </span>") + dWrap;
            }
            return QStringLiteral("<sup>") + num +
                   QStringLiteral("</sup><span style=\"font-style:normal;\">\u2044</span><sub>") + den +
                   QStringLiteral("</sub>");
        }
        if (cmd == QLatin1String("binom") || cmd == QLatin1String("dbinom") || cmd == QLatin1String("tbinom")) {
            const QString n = parseGroupOrAtom(depth + 1).trimmed();
            const QString k = parseGroupOrAtom(depth + 1).trimmed();
            return QStringLiteral("<span style=\"font-style:normal;\">C(</span>") + n +
                   QStringLiteral("<span style=\"font-style:normal;\">, </span>") + k +
                   QStringLiteral("<span style=\"font-style:normal;\">)</span>");
        }
        if (cmd == QLatin1String("sqrt")) {
            QString indexHtml;
            skipSpaces();
            if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('[')) {
                ++m_pos;
                const qsizetype idxStart = m_pos;
                while (m_pos < m_src.size() && m_src[m_pos] != QLatin1Char(']'))
                    ++m_pos;
                const QString rawIdx = m_src.mid(idxStart, m_pos - idxStart);
                if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char(']'))
                    ++m_pos;
                indexHtml = QStringLiteral("<sup>") + LatexParser(rawIdx, false).parseAll(depth + 1) +
                            QStringLiteral("</sup>");
            }
            const QString rawArg = readRawGroup();
            const QString body = LatexParser(rawArg, false).parseAll(depth + 1).trimmed();
            const bool simple = rawArg.trimmed().size() <= 1;
            return indexHtml + QStringLiteral("<span style=\"font-style:normal;\">\u221A</span>") +
                   (simple ? body
                           : QStringLiteral("<span style=\"font-style:normal;\">(</span>") + body +
                                 QStringLiteral("<span style=\"font-style:normal;\">)</span>"));
        }
        if (cmd == QLatin1String("mathbb"))
            return mapBlackboard(readRawGroup());
        if (cmd == QLatin1String("mathcal") || cmd == QLatin1String("mathscr"))
            return mapCalligraphic(readRawGroup());
        if (cmd == QLatin1String("text") || cmd == QLatin1String("mathrm") || cmd == QLatin1String("textrm") ||
            cmd == QLatin1String("operatorname") || cmd == QLatin1String("mathsf") || cmd == QLatin1String("mathtt")) {
            const QString raw = readRawGroup();
            return QStringLiteral("<span style=\"font-style:normal;\">") + escapeHtmlText(raw) +
                   QStringLiteral("</span>");
        }
        if (cmd == QLatin1String("mathbf") || cmd == QLatin1String("textbf") || cmd == QLatin1String("bm") ||
            cmd == QLatin1String("boldsymbol")) {
            return QStringLiteral("<b style=\"font-style:normal;\">") + parseGroupOrAtom(depth + 1) +
                   QStringLiteral("</b>");
        }
        if (cmd == QLatin1String("hat") || cmd == QLatin1String("widehat"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0302");
        if (cmd == QLatin1String("bar") || cmd == QLatin1String("overline"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0304");
        if (cmd == QLatin1String("tilde") || cmd == QLatin1String("widetilde"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0303");
        if (cmd == QLatin1String("vec"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u20D7");
        if (cmd == QLatin1String("dot"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0307");
        if (cmd == QLatin1String("ddot"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0308");
        if (cmd == QLatin1String("begin") || cmd == QLatin1String("end")) {
            (void)readRawGroup();  // consume environment name like {aligned} / {equation*}
            return {};
        }
        if (cmd == QLatin1String("left") || cmd == QLatin1String("right") || cmd == QLatin1String("big") ||
            cmd == QLatin1String("Big") || cmd == QLatin1String("bigg") || cmd == QLatin1String("Bigg") ||
            cmd == QLatin1String("limits") || cmd == QLatin1String("nolimits") || cmd == QLatin1String("displaystyle") ||
            cmd == QLatin1String("textstyle")) {
            skipSpaces();
            if ((cmd == QLatin1String("left") || cmd == QLatin1String("right")) && m_pos < m_src.size() &&
                m_src[m_pos] == QLatin1Char('.'))
                ++m_pos;  // invisible delimiter \left. or \right.
            return {};
        }
        if (latexSymbols().contains(cmd)) {
            const QString sym = latexSymbols().value(cmd);
            const bool isLowerGreek = !sym.isEmpty() && sym[0].unicode() >= 0x03B1 && sym[0].unicode() <= 0x03F5;
            return isLowerGreek ? sym : (QStringLiteral("<span style=\"font-style:normal;\">") + sym + QStringLiteral("</span>"));
        }
        if (uprightFunctions().contains(cmd))
            return QStringLiteral("<span style=\"font-style:normal;\">") + cmd + QStringLiteral("</span>");
        // Unknown command: emit plain text name without backslash so equations remain readable.
        return escapeHtmlText(cmd);
    }

    QString m_src;
    qsizetype m_pos = 0;
    bool m_display = false;
};

bool looksLikeCurrencySpan(const QString &inner) {
    if (inner.isEmpty())
        return true;
    // Guard e.g. "$5 to $10" or "$2.50 per book" if a stray $ appears later on the line.
    if (inner[0].isDigit() && inner.contains(QLatin1Char(' '))) {
        static const QRegularExpression mathOps(QStringLiteral("[\\\\^_=<>+*/{}\\[\\]()]"));
        if (!mathOps.match(inner).hasMatch())
            return true;
    }
    return false;
}

QString generateMathNonce(const QString &avoid) {
    QString nonce;
    do {
        const quint64 a = QRandomGenerator::global()->generate64();
        const quint64 b = QRandomGenerator::global()->generate64();
        nonce = QStringLiteral("CBMATH%1%2")
                    .arg(a, 16, 16, QLatin1Char('0'))
                    .arg(b, 16, 16, QLatin1Char('0'));
    } while (avoid.contains(nonce));
    return nonce;
}

QString stripDangerousMarkup(const QString &text) {
    if (text.isEmpty())
        return text;
    static const QRegularExpression htmlImg(QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression htmlScriptStyleBlock(
        QStringLiteral("<(?:script|style)\\b[^>]*>[\\s\\S]*?</(?:script|style)\\s*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression htmlDangerousTag(
        QStringLiteral("</?(?:script|style|iframe|object|embed|link|meta|base|svg|audio|video|source|form|input|button|"
                       "textarea|select)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression inlineImg(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression refImg(QStringLiteral("!\\[([^\\]]*)\\]\\[[^\\]]*\\]"));

    QString out = text;
    // Iterate until fixed point so doubly/triply nested malformed tags like
    // "<i<i<img >mg >mg src=...>" can never reconstruct an active tag.
    for (int pass = 0; pass < 64; ++pass) {
        const QString before = out;
        out.remove(htmlImg);
        out.remove(htmlScriptStyleBlock);
        out.remove(htmlDangerousTag);
        out.replace(inlineImg, QStringLiteral("\\1"));
        out.replace(refImg, QStringLiteral("\\1"));
        if (out == before)
            break;
    }
    out.replace(QStringLiteral("!["), QStringLiteral("!\u200B["));
    return out;
}

// CommonMark §4.5 backtick (`) and tilde (~) code fence matcher: prevents
// fence-char, fence-length, or info-string desyncs (e.g. a 3-tick line or
// "``` info" inside a 4-tick or ~~~ fence).
bool matchCodeFence(const QString &text, qsizetype pos, QChar openFenceChar, qsizetype openFenceLen,
                    QChar &charOut, qsizetype &lenOut) {
    const qsizetype n = text.size();
    if (pos + 2 >= n)
        return false;
    const QChar ch = text[pos];
    if ((ch != QLatin1Char('`') && ch != QLatin1Char('~')) || text[pos + 1] != ch || text[pos + 2] != ch)
        return false;
    qsizetype lineStart = pos;
    while (lineStart > 0 && text[lineStart - 1] != QLatin1Char('\n'))
        --lineStart;
    if (pos - lineStart > 3)
        return false;
    for (qsizetype k = lineStart; k < pos; ++k)
        if (text[k] != QLatin1Char(' '))
            return false;

    qsizetype run = 0;
    while (pos + run < n && text[pos + run] == ch)
        ++run;
    qsizetype lineEnd = pos + run;
    while (lineEnd < n && text[lineEnd] != QLatin1Char('\n'))
        ++lineEnd;

    if (openFenceLen == 0) {
        // CommonMark §4.5: opening backtick fence info string cannot contain backticks.
        if (ch == QLatin1Char('`')) {
            for (qsizetype k = pos + run; k < lineEnd; ++k)
                if (text[k] == QLatin1Char('`'))
                    return false;
        }
        charOut = ch;
        lenOut = run;
        return true;
    }
    // Closing fence must match the opening fence character, have at least
    // `openFenceLen` repetitions, and only optional trailing spaces/tabs before newline/EOF.
    if (ch != openFenceChar || run < openFenceLen)
        return false;
    for (qsizetype k = pos + run; k < lineEnd; ++k)
        if (text[k] != QLatin1Char(' ') && text[k] != QLatin1Char('\t'))
            return false;
    charOut = ch;
    lenOut = run;
    return true;
}

// Returns the index right after the matching closing backtick run of exact length
// `ticks` before a paragraph break (\n\n) or EOF, or -1 if unclosed.
qsizetype findMatchingInlineCodeEnd(const QString &text, qsizetype openPos, qsizetype ticks) {
    const qsizetype n = text.size();
    qsizetype j = openPos + ticks;
    while (j < n) {
        if (text[j] == QLatin1Char('\n') && j + 1 < n && text[j + 1] == QLatin1Char('\n'))
            return -1;
        if (text[j] == QLatin1Char('`')) {
            qsizetype run = 0;
            while (j + run < n && text[j + run] == QLatin1Char('`'))
                ++run;
            if (run == ticks)
                return j + run;
            j += run;
            continue;
        }
        ++j;
    }
    return -1;
}

QString escapeRawHtmlOutsideCode(const QString &text) {
    if (!text.contains(QLatin1Char('<')) && !text.contains(QLatin1Char('>')))
        return text;
    QString out;
    out.reserve(text.size() + 32);
    const qsizetype n = text.size();
    qsizetype i = 0;
    QChar fenceChar;
    qsizetype fenceLen = 0;
    int lineIndent = 0;
    bool inBlockquotePrefix = true;

    while (i < n) {
        QChar matchedChar;
        qsizetype matchedLen = 0;
        if (matchCodeFence(text, i, fenceChar, fenceLen, matchedChar, matchedLen)) {
            if (fenceLen == 0) {
                fenceChar = matchedChar;
                fenceLen = matchedLen;
            } else {
                fenceChar = QChar();
                fenceLen = 0;
            }
            out += text.mid(i, matchedLen);
            i += matchedLen;
            inBlockquotePrefix = false;
            continue;
        }
        if (fenceLen > 0) {
            const QChar c = text[i++];
            out += c;
            if (c == QLatin1Char('\n')) {
                lineIndent = 0;
                inBlockquotePrefix = true;
            }
            continue;
        }
        if (text[i] == QLatin1Char('\\') && i + 1 < n &&
            (text[i + 1] == QLatin1Char('\\') || text[i + 1] == QLatin1Char('`'))) {
            out += text[i];
            out += text[i + 1];
            i += 2;
            inBlockquotePrefix = false;
            continue;
        }
        if (text[i] == QLatin1Char('`')) {
            qsizetype ticks = 0;
            while (i + ticks < n && text[i + ticks] == QLatin1Char('`'))
                ++ticks;
            const qsizetype closeEnd = findMatchingInlineCodeEnd(text, i, ticks);
            if (closeEnd > i) {
                out += text.mid(i, closeEnd - i);
                i = closeEnd;
                inBlockquotePrefix = false;
                continue;
            }
            out += text.mid(i, ticks);
            i += ticks;
            inBlockquotePrefix = false;
            continue;
        }
        const QChar c = text[i++];
        if (c == QLatin1Char('\n')) {
            out += c;
            lineIndent = 0;
            inBlockquotePrefix = true;
            continue;
        }
        if (c == QLatin1Char('<')) {
            out += QStringLiteral("&lt;");
            inBlockquotePrefix = false;
            continue;
        }
        if (c == QLatin1Char('>')) {
            if (inBlockquotePrefix && lineIndent <= 3) {
                out += c;
                lineIndent = 0;
            } else {
                out += QStringLiteral("&gt;");
                inBlockquotePrefix = false;
            }
            continue;
        }
        if (inBlockquotePrefix && c == QLatin1Char(' ')) {
            ++lineIndent;
            if (lineIndent > 3)
                inBlockquotePrefix = false;
        } else {
            inBlockquotePrefix = false;
        }
        out += c;
    }
    return out;
}

QString extractLatexWithNonces(const QString &text, const QString &nonce, QStringList &mathInnersOut,
                               QList<bool> &mathIsBlockOut, bool padBlocks) {
    if (!text.contains(QLatin1Char('$')) && !text.contains(QStringLiteral("\\(")) &&
        !text.contains(QStringLiteral("\\[")))
        return text;

    QString out;
    out.reserve(text.size() + 64);
    const qsizetype n = text.size();
    qsizetype i = 0;
    QChar fenceChar;
    qsizetype fenceLen = 0;

    auto emitMath = [&](const QString &expr, bool display) -> bool {
        const QString inner = LatexParser(expr.trimmed(), display).parseAll().trimmed();
        if (inner.isEmpty())
            return false;
        const int idx = int(mathInnersOut.size());
        mathInnersOut.append(inner);
        mathIsBlockOut.append(display);
        const QString token =
            QStringLiteral("%1%2%3Z").arg(nonce, display ? QStringLiteral("B") : QStringLiteral("I"), QString::number(idx));
        if (display && padBlocks)
            out += QStringLiteral("\n\n") + token + QStringLiteral("\n\n");
        else
            out += token;
        return true;
    };

    while (i < n) {
        // Preserve fenced code blocks (``` or ~~~) verbatim (e.g. copyable LaTeX output).
        QChar matchedChar;
        qsizetype matchedLen = 0;
        if (matchCodeFence(text, i, fenceChar, fenceLen, matchedChar, matchedLen)) {
            if (fenceLen == 0) {
                fenceChar = matchedChar;
                fenceLen = matchedLen;
            } else {
                fenceChar = QChar();
                fenceLen = 0;
            }
            out += text.mid(i, matchedLen);
            i += matchedLen;
            continue;
        }
        if (fenceLen > 0) {
            out += text[i++];
            continue;
        }
        if (text[i] == QLatin1Char('\\') && i + 1 < n &&
            (text[i + 1] == QLatin1Char('\\') || text[i + 1] == QLatin1Char('`') ||
             text[i + 1] == QLatin1Char('$'))) {
            out += text[i];
            out += text[i + 1];
            i += 2;
            continue;
        }
        // Preserve matched inline code spans (`...`) verbatim.
        if (text[i] == QLatin1Char('`')) {
            qsizetype ticks = 0;
            while (i + ticks < n && text[i + ticks] == QLatin1Char('`'))
                ++ticks;
            const qsizetype closeEnd = findMatchingInlineCodeEnd(text, i, ticks);
            if (closeEnd > i) {
                out += text.mid(i, closeEnd - i);
                i = closeEnd;
                continue;
            }
            out += text.mid(i, ticks);
            i += ticks;
            continue;
        }

        // Display math: $$...$$
        if (i + 1 < n && text[i] == QLatin1Char('$') && text[i + 1] == QLatin1Char('$')) {
            const qsizetype close = text.indexOf(QStringLiteral("$$"), i + 2);
            if (close > i + 2 && emitMath(text.mid(i + 2, close - (i + 2)), true)) {
                i = close + 2;
                continue;
            }
        }
        // Display math: \[...\]
        if (i + 1 < n && text[i] == QLatin1Char('\\') && text[i + 1] == QLatin1Char('[')) {
            const qsizetype close = text.indexOf(QStringLiteral("\\]"), i + 2);
            if (close > i + 2 && emitMath(text.mid(i + 2, close - (i + 2)), true)) {
                i = close + 2;
                continue;
            }
        }
        // Inline math: \(...\)
        if (i + 1 < n && text[i] == QLatin1Char('\\') && text[i + 1] == QLatin1Char('(')) {
            const qsizetype close = text.indexOf(QStringLiteral("\\)"), i + 2);
            if (close > i + 2 && emitMath(text.mid(i + 2, close - (i + 2)), false)) {
                i = close + 2;
                continue;
            }
        }
        // Inline math: $...$ (with strict Pandoc/KaTeX currency guards)
        if (text[i] == QLatin1Char('$') && i + 1 < n && text[i + 1] != QLatin1Char('$') &&
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
                    break;
                }
                ++j;
            }
            if (close > i + 1) {
                const QString expr = text.mid(i + 1, close - (i + 1));
                if (!looksLikeCurrencySpan(expr) && emitMath(expr, false)) {
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

QString sanitizeMathBgColor(const QString &mathBlockBg) {
    const QString trimmed = mathBlockBg.trimmed();
    static const QRegularExpression safeColorRx(QStringLiteral("^[#a-zA-Z0-9(),.%\\s-]+$"));
    if (trimmed.isEmpty() || trimmed.size() > 64 || !safeColorRx.match(trimmed).hasMatch())
        return QStringLiteral("rgba(128,128,128,0.12)");
    return escapeHtmlText(trimmed);
}

}  // namespace

namespace MathFormatter {

QString formatLatexInMarkdown(const QString &text) {
    if (text.isEmpty())
        return text;

    // 1. Replace ONLY genuine LaTeX produced by our own math formatter with per-call
    //    unforgeable random nonce placeholders BEFORE escaping raw HTML.
    const QString nonce = generateMathNonce(text);
    QStringList mathInners;
    QList<bool> mathIsBlock;
    QString out = extractLatexWithNonces(text, nonce, mathInners, mathIsBlock, false);

    // 2. Escape all remaining raw HTML angle brackets outside code fences/spans so
    //    untrusted prose can never inject raw tags into MarkdownText or RichText.
    out = escapeRawHtmlOutsideCode(out);

    // 3. Substitute ONLY our own generated math HTML back for the per-call nonce tokens.
    for (int i = 0; i < mathInners.size(); ++i) {
        const QString token =
            QStringLiteral("%1%2%3Z").arg(nonce, mathIsBlock[i] ? QStringLiteral("B") : QStringLiteral("I"), QString::number(i));
        const QString rendered =
            mathIsBlock[i]
                ? (QStringLiteral(
                       "<div class=\"math-block\" style=\"font-family:'Literata',Georgia,serif;font-style:italic;"
                       "background-color:rgba(128,128,128,0.12);padding:8px 12px;margin:6px 0;text-align:center;\">") +
                   mathInners[i] + QStringLiteral("</div>"))
                : (QStringLiteral(
                       "<span class=\"math-inline\" style=\"font-family:'Literata',Georgia,serif;font-style:italic;\">") +
                   mathInners[i] + QStringLiteral("</span>"));
        out.replace(token, rendered);
    }
    return out;
}

QString cleanMarkdown(const QString &text) {
    if (text.isEmpty())
        return text;
    // 1. Iteratively strip dangerous raw HTML tags and Markdown image embeds to fixed point.
    QString out = formatLatexInMarkdown(stripDangerousMarkup(text));
    out.replace(QStringLiteral("!["), QStringLiteral("!\u200B["));
    return out;
}

bool hasMath(const QString &text) {
    if (text.isEmpty())
        return false;
    if (!text.contains(QLatin1Char('$')) && !text.contains(QStringLiteral("\\(")) &&
        !text.contains(QStringLiteral("\\[")))
        return false;
    const QString pre = stripDangerousMarkup(text);
    const QString nonce = generateMathNonce(pre);
    QStringList mathInners;
    QList<bool> mathIsBlock;
    (void)extractLatexWithNonces(pre, nonce, mathInners, mathIsBlock, false);
    return !mathInners.isEmpty();
}

QString formatDisplayHtml(const QString &text, const QString &mathBlockBg) {
    if (text.isEmpty())
        return {};

    // 1. Strip dangerous tags and Markdown images iteratively to fixed point.
    const QString pre = stripDangerousMarkup(text);

    // 2. Generate an unforgeable per-call random nonce and extract ONLY genuine LaTeX
    //    rendered by Clothbound's own LatexParser — NEVER hoisting literal
    //    <div class="math-block"> or <span class="math-inline"> from untrusted input.
    const QString nonce = generateMathNonce(pre);
    QStringList mathInners;
    QList<bool> mathIsBlock;
    QString md = extractLatexWithNonces(pre, nonce, mathInners, mathIsBlock, true);

    // 3. Escape all remaining raw HTML in untrusted Markdown BEFORE QTextDocument::setMarkdown().
    md = escapeRawHtmlOutsideCode(md);

    QTextDocument doc;
    doc.setMarkdown(md);
    QString html = doc.toHtml();

    // Extract inner body HTML so Qt Quick TextEdit inherits the panel's exact font family, size, and ink color.
    const qsizetype bodyOpen = html.indexOf(QStringLiteral("<body"));
    if (bodyOpen >= 0) {
        const qsizetype bodyStart = html.indexOf(QLatin1Char('>'), bodyOpen);
        const qsizetype bodyEnd = html.lastIndexOf(QStringLiteral("</body>"));
        if (bodyStart >= 0 && bodyEnd > bodyStart)
            html = html.mid(bodyStart + 1, bodyEnd - bodyStart - 1).trimmed();
    }
    // Defense-in-depth: iteratively strip any <img>, active links <a>, and unsafe resource attributes.
    static const QRegularExpression imgRx(QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression linkHrefRx(QStringLiteral("<a\\b[^>]*>([\\s\\S]*?)</a>"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression strayLinkRx(QStringLiteral("</?a\\b[^>]*>"),
                                                QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unsafeAttrRx(
        QStringLiteral("\\s(?:href|src|background)\\s*=\\s*(?:\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    for (int pass = 0; pass < 32; ++pass) {
        const QString before = html;
        html.remove(imgRx);
        html.replace(linkHrefRx, QStringLiteral("\\1"));
        html.remove(strayLinkRx);
        html.remove(unsafeAttrRx);
        if (html == before)
            break;
    }

    // For single-paragraph snippets (e.g. Glossary definitions), remove default 12px paragraph margins.
    if (html.count(QStringLiteral("<p")) == 1 && !mathIsBlock.contains(true)) {
        static const QRegularExpression singlePMargin(QStringLiteral("margin-top:\\s*\\d+px;\\s*margin-bottom:\\s*\\d+px;"));
        html.replace(singlePMargin, QStringLiteral("margin-top:0px; margin-bottom:0px;"));
    }

    const QString safeBgColor = sanitizeMathBgColor(mathBlockBg);
    for (int i = 0; i < mathInners.size(); ++i) {
        const QString token =
            QStringLiteral("%1%2%3Z").arg(nonce, mathIsBlock[i] ? QStringLiteral("B") : QStringLiteral("I"), QString::number(i));
        if (mathIsBlock[i]) {
            // Qt's QTextDocument honors table cellpadding & td background-color for crisp tinted blocks.
            const QString snippet =
                QStringLiteral(
                    "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" "
                    "style=\"margin-top:6px;margin-bottom:6px;\">"
                    "<tr><td align=\"center\" style=\"background-color:%1;font-family:'Literata',Georgia,serif;"
                    "font-style:italic;\">%2</td></tr>"
                    "</table>")
                    .arg(safeBgColor, mathInners[i]);
            const QRegularExpression pWrap(QStringLiteral("<p[^>]*>\\s*") + QRegularExpression::escape(token) +
                                           QStringLiteral("\\s*</p>"));
            if (pWrap.match(html).hasMatch())
                html.replace(pWrap, snippet);
            else
                html.replace(token, snippet);
        } else {
            const QString snippet =
                QStringLiteral("<span style=\"font-family:'Literata',Georgia,serif;font-style:italic;\">%1</span>")
                    .arg(mathInners[i]);
            html.replace(token, snippet);
        }
    }
    return html;
}

}  // namespace MathFormatter
