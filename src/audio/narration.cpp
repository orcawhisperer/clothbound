#include "audio/narration.h"

#include "ai/ai.h"
#include "ai/aiservices.h"
#include "audio/audiocache.h"
#include "audio/cast.h"
#include "audio/director.h"
#include "library/library.h"
#include "audio/localspeech.h"
#include "reader/reader.h"
#include "audio/speechplayer.h"
#include "audio/voicepacks.h"

#include <QAudioDevice>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
QSettings settings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
// CLOTHBOUND_DEBUG (or legacy DOGEAR_DEBUG) traces the clips' lives on stderr.
void trace(const QString &message) {
    static const bool on = qEnvironmentVariableIsSet("CLOTHBOUND_DEBUG") || qEnvironmentVariableIsSet("DOGEAR_DEBUG");
    static const qint64 started = nowMs();
    if (on)
        fprintf(stderr, "narration %6.2fs %s\n", (nowMs() - started) / 1000.0, qPrintable(message));
}
// Clips after the first few wait this long for the director or cast before
// going ahead in the ordinary reading.
constexpr qint64 directionsPatienceMs = 6000;
// Narration runs at about this many characters a minute at normal speed.
constexpr double charsPerMinute = 900;
constexpr int sampleRate = 24000;
constexpr qint64 sleepFadeMs = 10000;
constexpr qint64 timeEmitIntervalMs = 250;

QString classicGeminiTrait(const QString &voiceId) {
    static const QHash<QString, QString> traits{
        {QStringLiteral("sulafat"), QStringLiteral("Warm")},
        {QStringLiteral("charon"), QStringLiteral("Informative")},
        {QStringLiteral("kore"), QStringLiteral("Firm")},
        {QStringLiteral("puck"), QStringLiteral("Upbeat")},
        {QStringLiteral("aoede"), QStringLiteral("Breezy")},
        {QStringLiteral("fenrir"), QStringLiteral("Excitable")},
        {QStringLiteral("zephyr"), QStringLiteral("Bright")},
        {QStringLiteral("orus"), QStringLiteral("Firm")},
        {QStringLiteral("leda"), QStringLiteral("Youthful")},
        {QStringLiteral("autonoe"), QStringLiteral("Bright")},
        {QStringLiteral("enceladus"), QStringLiteral("Breathy")},
        {QStringLiteral("iapetus"), QStringLiteral("Clear")},
        {QStringLiteral("umbriel"), QStringLiteral("Easy-going")},
        {QStringLiteral("algieba"), QStringLiteral("Smooth")},
        {QStringLiteral("despina"), QStringLiteral("Smooth")},
        {QStringLiteral("erinome"), QStringLiteral("Clear")},
        {QStringLiteral("algenib"), QStringLiteral("Gravelly")},
        {QStringLiteral("rasalgethi"), QStringLiteral("Informative")},
        {QStringLiteral("laomedeia"), QStringLiteral("Upbeat")},
        {QStringLiteral("achernar"), QStringLiteral("Soft")},
        {QStringLiteral("alnilam"), QStringLiteral("Firm")},
        {QStringLiteral("schedar"), QStringLiteral("Even")},
        {QStringLiteral("gacrux"), QStringLiteral("Mature")},
        {QStringLiteral("pulcherrima"), QStringLiteral("Forward")},
        {QStringLiteral("achird"), QStringLiteral("Friendly")},
        {QStringLiteral("zubenelgenubi"), QStringLiteral("Casual")},
        {QStringLiteral("vindemiatrix"), QStringLiteral("Gentle")},
        {QStringLiteral("sadachbia"), QStringLiteral("Lively")},
        {QStringLiteral("sadaltager"), QStringLiteral("Knowledgeable")},
        {QStringLiteral("callirrhoe"), QStringLiteral("Easy-going")},
    };
    return traits.value(voiceId.toLower());
}

qint64 usToSamples(qint64 us, int rate) {
    // Split into whole seconds + sub-second remainder after clamping so
    // extreme D-Bus Seek/SetPosition values can never overflow int64.
    constexpr qint64 maxUs = 100LL * 365 * 24 * 3600 * 1000000LL;
    const qint64 clamped = std::clamp(us, -maxUs, maxUs);
    const qint64 sec = clamped / 1000000LL;
    const qint64 rem = clamped % 1000000LL;
    return sec * rate + (rem * rate) / 1000000LL;
}

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
        if (depth > 24)
            return m_src.mid(m_pos);
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

QStringList splitOverlongResponseSentence(const QString &sentence, qsizetype maxChars) {
    QStringList pieces;
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
}

Narration::Narration(AiServices *ai, AudioCache *cache, Reader *reader, Library *library, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader), m_library(library),
      m_director(new Director(ai, cache, reader, this)), m_player(new SpeechPlayer(sampleRate, this)) {
    connect(m_player, &SpeechPlayer::position, this, &Narration::heard);
    connect(m_player, &SpeechPlayer::volumeChanged, this, &Narration::volumeChanged);
    connect(m_player, &SpeechPlayer::sinkError, this, [this](const QString &message) {
        if (m_responseActive) {
            failResponse(message);
            return;
        }
        if (!m_active)
            return;
        pause();
        setProblem(message);
    });
    connect(m_player, &SpeechPlayer::starvedChanged, this, [this](bool starved) {
        if (m_responseActive) {
            setResponseBuffering(starved);
            return;
        }
        if (m_wanted)
            setBuffering(starved);
    });
    connect(m_player, &SpeechPlayer::drained, this, [this] {
        if (m_responseActive) {
            if (m_responseQueued + 1 >= m_responseChunks.size()) {
                stopResponse();
                return;
            }
            if (m_responseChunks[m_responseQueued + 1].state == ResponseChunk::Failed) {
                stopResponse();
                return;
            }
            pumpResponse();
            if (m_responseActive && m_player->drainedOut())
                setResponseBuffering(true);
            return;
        }
        // Everything queued has been heard: chapter end (for sleep), the end
        // of the book, or the next chapter's words haven't arrived yet.
        if (!m_active)
            return;
        if (m_sleepAtChapterEnd && m_current >= 0 && m_current < m_clips.size() &&
            (m_queued + 1 >= m_clips.size() || m_clips[m_queued + 1].chapter != m_clips[m_current].chapter)) {
            m_sleepAtChapterEnd = false;
            const bool hasNextClip = m_queued + 1 < m_clips.size();
            if (hasNextClip) {
                m_current = m_queued + 1;
                m_currentSample = 0;
                m_sentence = -1;
                m_bookPlayerDirty = true;
                m_player->clear();
            }
            pause();
            if (hasNextClip) {
                if (!m_clips[m_current].sentences.isEmpty())
                    showSentenceForSample(m_current, 0, 0);
                else
                    emit positionChanged();
                emit timeChanged();
            }
            emit sleepChanged();
            return;
        }
        if (m_queued + 1 < m_clips.size() && m_clips[m_queued + 1].state == Clip::Failed) {
            stallIfFailed();
            return;
        }
        const bool more = m_queued + 1 < m_clips.size() || m_requested.size() > m_loadedChapters.size() ||
                          nextChapterAfter(m_lastChapter) >= 0;
        if (!more) {
            stop();
        } else if (m_wanted) {
            pump();
            if (m_active && m_wanted && m_player->drainedOut())
                setBuffering(true);
        }
    });
    m_directionsTimer.setSingleShot(true);
    connect(&m_directionsTimer, &QTimer::timeout, this, &Narration::pump);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_active && m_wanted)
            pump();
    });
    m_sleepTimer.setSingleShot(true);
    connect(&m_sleepTimer, &QTimer::timeout, this, &Narration::finishSleep);
    m_sleepTick.setInterval(1000);
    connect(&m_sleepTick, &QTimer::timeout, this, &Narration::onSleepTick);
    connect(m_reader, &Reader::speakable, this, &Narration::addBlocks);
    // The open book's narrator and style are known before listening starts,
    // so they can be shown and changed.
    connect(m_reader, &Reader::bookChanged, this, [this] {
        m_pendingStart = false;
        if (m_responseActive && (m_reader->bookId() <= 0 || m_reader->bookId() != m_bookId))
            stopResponse();
        if (m_active && m_reader->bookId() != m_bookId)
            stop();
        if (!m_active && m_reader->bookId() > 0) {
            m_bookId = m_reader->bookId();
            loadVoice();
        }
    });

    QSettings s = settings();
    m_rate = std::clamp(s.value(QStringLiteral("listen/rate"), 1.0).toDouble(), 0.75, 2.0);
    m_economy = s.value(QStringLiteral("listen/economy"), false).toBool();
    m_player->setRate(m_rate);
}

Narration::~Narration() { stop(); }

double Narration::volume() const { return m_player->volume(); }

void Narration::setVolume(double volume) {
    // Explicit external volume change supersedes any pending sleep-fade restore.
    const bool wasFading = m_sleepFading;
    m_sleepPausedRestore = false;
    m_sleepFading = false;
    m_preFadeVolume = -1.0;
    m_player->setVolume(volume);
    if (wasFading)
        emit sleepChanged();
}

int Narration::indexOfClip(int trackId) const {
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips[i].id == trackId)
            return i;
    return -1;
}

void Narration::loadVoice() {
    QSettings s = settings();
    const QString performance = m_library->bookSetting(m_bookId, QStringLiteral("performance"));
    const QString chosen = performance.isEmpty()
                               ? s.value(QStringLiteral("listen/performance"), QStringLiteral("natural")).toString()
                               : performance;
    if (chosen != m_performance) {
        m_performance = chosen;
        emit settingsChanged();
    }
    const QString bookVoice = m_library->bookSetting(m_bookId, QStringLiteral("voice"));
    if (!bookVoice.isEmpty()) {
        m_voice = bookVoice;
        m_voiceName = m_library->bookSetting(m_bookId, QStringLiteral("voiceName"));
    } else {
        m_voice = s.value(QStringLiteral("listen/voice"), QStringLiteral("sulafat")).toString();
        m_voiceName = s.value(QStringLiteral("listen/voiceName"), QStringLiteral("Sulafat")).toString();
    }
    if (m_cast)
        m_cast->setNarrator(m_voice);
    emit voiceChanged();
}

int Narration::chapter() const {
    return m_current >= 0 && m_current < m_clips.size() ? m_clips[m_current].chapter : m_startChapter;
}

QString Narration::voiceAccent() const {
    if (m_voice.isEmpty())
        return {};
    if (AiServices::isBuiltinVoice(m_voice)) {
        if (auto *local = qobject_cast<LocalSpeech *>(m_ai->providerFor(QStringLiteral("builtin:"))))
            if (local->packs())
                return local->packs()->voice(m_voice).value(QStringLiteral("accent")).toString();
        return {};
    }
    if (AiServices::isStudioVoice(m_voice)) {
        const QString model = m_ai ? m_ai->localSpeechModel() : QString();
        return model.isEmpty() ? tr("Studio") : model;
    }
    return classicGeminiTrait(m_voice);
}

void Narration::setFollowing(bool following) {
    if (following == m_following)
        return;
    m_following = following;
    emit stateChanged();
    if (following && m_sentenceStart >= 0)
        emit speaking(chapter(), m_sentenceStart, m_sentenceEnd);
}

void Narration::setRate(double rate) {
    rate = std::clamp(std::round(rate * 20) / 20, 0.75, 2.0);
    if (qFuzzyCompare(rate, m_rate))
        return;
    m_rate = rate;
    trace(QStringLiteral("rate %1").arg(rate));
    m_player->setRate(rate);
    settings().setValue(QStringLiteral("listen/rate"), m_rate);
    emit rateChanged();
    emit positionChanged();
    emit timeChanged();
}

void Narration::setPerformance(const QString &performance) {
    static const QStringList known{"plain", "natural", "dramatic"};
    if (!known.contains(performance) || performance == m_performance)
        return;
    m_performance = performance;
    settings().setValue(QStringLiteral("listen/performance"), performance);
    if (m_reader->bookId() > 0)
        m_library->setBookSetting(m_reader->bookId(), QStringLiteral("performance"), performance);
    emit settingsChanged();
    // The new reading takes over from the sentence being spoken (or where we started).
    if (m_active) {
        const int at = m_sentenceStart >= 0 ? chapter() : m_startChapter;
        const qint64 offset = m_sentenceStart >= 0 ? m_sentenceStart : m_startOffset;
        const bool wanted = m_wanted;
        start(at, offset);
        if (!wanted)
            pause();
    }
}

void Narration::setEconomy(bool economy) {
    if (economy == m_economy)
        return;
    m_economy = economy;
    settings().setValue(QStringLiteral("listen/economy"), economy);
    emit settingsChanged();
}

void Narration::setVoice(const QString &id, const QString &name) {
    if (id.isEmpty())
        return;
    m_voice = id;
    m_voiceName = name.isEmpty() ? id : name;
    clearFallbackOffer();
    setFallbackNotice({});
    QSettings s = settings();
    s.setValue(QStringLiteral("listen/voice"), m_voice);
    s.setValue(QStringLiteral("listen/voiceName"), m_voiceName);
    if (m_reader->bookId() > 0) {
        m_library->setBookSetting(m_reader->bookId(), QStringLiteral("voice"), m_voice);
        m_library->setBookSetting(m_reader->bookId(), QStringLiteral("voiceName"), m_voiceName);
    }
    if (m_cast)
        m_cast->setNarrator(m_voice);
    emit voiceChanged();
    // The new voice takes over from the sentence being spoken (or where we started).
    if (m_active) {
        const int at = m_sentenceStart >= 0 ? chapter() : m_startChapter;
        const qint64 offset = m_sentenceStart >= 0 ? m_sentenceStart : m_startOffset;
        const bool wanted = m_wanted;
        start(at, offset);
        if (!wanted)
            pause();
    }
}

void Narration::useFallbackVoice() {
    if (m_fallbackVoice.isEmpty() || !m_ai->canSpeakWith(m_fallbackVoice))
        return;
    const QString id = m_fallbackVoice;
    const QString name = m_fallbackVoiceName;
    clearFallbackOffer();
    setProblem({});
    if (m_active) {
        const int at = m_sentenceStart >= 0 ? chapter() : m_startChapter;
        const qint64 offset = m_sentenceStart >= 0 ? m_sentenceStart : m_startOffset;
        setVoice(id, name);
        if (!m_active)
            start(at, offset);
    } else {
        setVoice(id, name);
        if (m_pendingStart)
            startPending();
    }
}

void Narration::startPending() {
    if (!m_pendingStart)
        return;
    const int ch = m_pendingChapter;
    const qint64 off = m_pendingOffset;
    m_pendingStart = false;
    emit stateChanged();
    start(ch, off);
}

void Narration::setBuffering(bool buffering) {
    if (buffering == m_buffering)
        return;
    m_buffering = buffering;
    if (buffering)
        trace(QStringLiteral("buffering at #%1").arg(m_current));
    emit stateChanged();
}

void Narration::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit problemChanged();
}

QPair<QString, QString> Narration::bestLocalVoice() const {
    auto *local = qobject_cast<LocalSpeech *>(m_ai->providerFor(QStringLiteral("builtin:")));
    if (!local)
        return {};
    const QVariantMap best = local->bestInstalledVoice(m_reader ? m_reader->language() : QString());
    if (best.isEmpty())
        return {};
    const QString id = best.value(QStringLiteral("id")).toString();
    const QString name = best.value(QStringLiteral("name")).toString();
    if (id.isEmpty() || !m_ai->canSpeakWith(id))
        return {};
    return {id, name.isEmpty() ? id : name};
}

bool Narration::localPacksSupported() const {
    auto *local = qobject_cast<LocalSpeech *>(m_ai->providerFor(QStringLiteral("builtin:")));
    return local && local->packsSupported();
}

void Narration::updateFallbackOffer() {
    const auto [id, name] = bestLocalVoice();
    QString nextId;
    QString nextName;
    bool nextSetup = false;
    if (!id.isEmpty() && id != m_voice) {
        nextId = id;
        nextName = name;
    } else if (id.isEmpty() && !AiServices::isBuiltinVoice(m_voice) && localPacksSupported()) {
        nextSetup = true;
    }
    if (nextId != m_fallbackVoice || nextName != m_fallbackVoiceName || nextSetup != m_offerOfflineSetup) {
        m_fallbackVoice = nextId;
        m_fallbackVoiceName = nextName;
        m_offerOfflineSetup = nextSetup;
        emit fallbackChanged();
    }
}

void Narration::clearFallbackOffer() {
    if (m_fallbackVoice.isEmpty() && m_fallbackVoiceName.isEmpty() && !m_offerOfflineSetup)
        return;
    m_fallbackVoice.clear();
    m_fallbackVoiceName.clear();
    m_offerOfflineSetup = false;
    emit fallbackChanged();
}

void Narration::setFallbackNotice(const QString &notice) {
    if (notice == m_fallbackNotice)
        return;
    m_fallbackNotice = notice;
    emit fallbackChanged();
}

bool Narration::fallbackToLocalMidSession(int fromIndex, const QString &reason) {
    const auto [fbId, fbName] = bestLocalVoice();
    if (fbId.isEmpty() || fbId == m_voice || !m_ai->canSpeakWith(fbId))
        return false;
    trace(QStringLiteral("fallback mid-session -> %1 (%2)").arg(fbId, reason));
    m_retryTimer.stop();
    clearFallbackOffer();
    m_voice = fbId;
    m_voiceName = fbName;
    if (m_cast)
        m_cast->setNarrator(m_voice);
    emit voiceChanged();
    setProblem({});
    setFallbackNotice(reason.isEmpty() ? tr("Switched to %1 offline").arg(fbName)
                                       : tr("Switched to %1 offline (%2)").arg(fbName, reason));
    // Abort any other in-flight cloud requests for unqueued clips from fromIndex
    // onward and re-synthesize them locally.
    for (int i = qMax(0, fromIndex); i < m_clips.size(); ++i) {
        Clip &c = m_clips[i];
        if (c.queued)
            continue;
        if (c.reply) {
            QPointer<Ai::Reply> obsolete = c.reply;
            c.reply = nullptr;
            obsolete->abort();
        }
        if (c.state != Clip::Complete || c.pcm.isEmpty()) {
            c.state = Clip::Waiting;
            c.pcm = QByteArray();
            c.tries = 0;
            c.key.clear();
            c.voice = m_voice;
        }
    }
    pump();
    return true;
}

int Narration::nextChapterAfter(int chapter) const {
    if (chapter < 0)
        return -1;
    const QVariantList spine = m_reader->manifest().value(QStringLiteral("spine")).toList();
    const bool fromLinear = spine.value(chapter).toMap().value(QStringLiteral("linear")).toBool();
    for (int i = chapter + 1; i < spine.size(); ++i)
        if (!fromLinear || spine[i].toMap().value(QStringLiteral("linear")).toBool())
            return i;
    return -1;
}

void Narration::start(int chapter, qint64 offset) {
    stop();
    setProblem({});
    clearFallbackOffer();
    setFallbackNotice({});
    m_pendingStart = false;
    m_bookId = m_reader->bookId();
    if (m_bookId <= 0)
        return;
    loadVoice();
    if (!m_ai->canSpeakWith(m_voice)) {
        if (AiServices::isBuiltinVoice(m_voice)) {
            m_pendingStart = true;
            m_pendingChapter = chapter;
            m_pendingOffset = offset;
            setProblem(tr("This built-in voice isn't downloaded yet. Choose it again in the Narrator panel (V)."));
            emit stateChanged();
            emit voiceMissing();
            return;
        }
        if (AiServices::isStudioVoice(m_voice)) {
            setProblem(tr("This Studio voice needs its model server running on this computer, chosen in Intelligence (✦)."));
            emit voiceMissing();
            return;
        }
        // Cloud voice with no Gemini key: seamlessly fall back to the best
        // installed local voice for this book's language, or open VoicePanel
        // cleanly on the Built-in tab for one-click offline setup.
        const auto [fbId, fbName] = bestLocalVoice();
        if (!fbId.isEmpty()) {
            m_voice = fbId;
            m_voiceName = fbName;
            if (m_cast)
                m_cast->setNarrator(m_voice);
            emit voiceChanged();
            setFallbackNotice(tr("Using %1 offline").arg(fbName));
        } else if (localPacksSupported()) {
            m_pendingStart = true;
            m_pendingChapter = chapter;
            m_pendingOffset = offset;
            setProblem(tr("Download an offline voice to listen right away, or add a Gemini key."));
            emit stateChanged();
            emit voiceMissing();
            return;
        } else {
            setProblem(m_ai->describe(int(Ai::Error::NoKey)));
            emit needsKey();
            return;
        }
    } else if (!AiServices::isOnThisComputer(m_voice) && !m_ai->fitsBudget(0.001)) {
        // Cloud voice already at monthly budget cap: seamlessly switch to local if installed.
        const auto [fbId, fbName] = bestLocalVoice();
        if (!fbId.isEmpty()) {
            m_voice = fbId;
            m_voiceName = fbName;
            if (m_cast)
                m_cast->setNarrator(m_voice);
            emit voiceChanged();
            setFallbackNotice(tr("Using %1 offline (budget reached)").arg(fbName));
        }
    }
    m_active = true;
    m_wanted = true;
    m_following = true;
    if (m_library)
        m_library->markListened(m_bookId);
    m_startChapter = chapter;
    m_startOffset = offset;
    m_requested.insert(chapter);
    setBuffering(true);
    m_player->play();
    emit stateChanged();
    emit blocksWanted(chapter);
}

void Narration::addBlocks(int chapter, const QString &json) {
    if (!m_active || !m_requested.contains(chapter) || m_loadedChapters.contains(chapter))
        return;
    m_loadedChapters.insert(chapter);
    m_lastChapter = qMax(m_lastChapter, chapter);
    const QList<SpeechBlock> blocks = Speech::parseBlocks(json);
    m_blocks.insert(chapter, blocks);
    const bool isPdf = m_reader && m_reader->format() == QLatin1String("pdf");
    // Direction kept from an earlier reading shapes the clips from the
    // start, so they match the audio made then.
    bool kept = false;
    if (directed() && m_ai->textFor(QStringLiteral("director"))) {
        if (m_moods.contains(chapter)) {
            kept = true;
        } else if (isPdf) {
            if (const auto byPage = m_director->keptPdfBatch(chapter, m_performance)) {
                for (auto it = byPage->cbegin(); it != byPage->cend(); ++it)
                    m_moods.insert(it.key(), it.value());
                kept = true;
            }
        } else {
            kept = keptDirections(chapter);
        }
    }
    // The cast too, kept from an earlier reading, or asked for below.
    bool castKept = true;
    if (m_cast && m_cast->enabled() && m_cast->available() && !m_cast->knows(chapter)) {
        if (isPdf) {
            if (const auto byPage = m_cast->keptPdfBatch(chapter)) {
                for (auto it = byPage->cbegin(); it != byPage->cend(); ++it)
                    m_cast->learn(it.key(), it.value());
            } else {
                castKept = false;
            }
        } else if (const auto lines = m_cast->kept(chapter, blocks)) {
            m_cast->learn(chapter, *lines);
        } else {
            castKept = false;
        }
    }
    const qint64 from = chapter == m_startChapter ? m_startOffset : 0;
    const bool firstOfSession = m_clips.isEmpty();
    const QList<SpeechClip> made =
        Speech::clipsFrom(chapter, blocks, from, breaksFor(chapter), blockVoicesFor(chapter, blocks), m_voice);
    const int before = int(m_clips.size());
    for (const SpeechClip &spoken : made)
        m_clips.append(clipFrom(spoken));
    // Otherwise the first few clips go ahead without direction, so the
    // voice starts at once and never waits on the director or cast.
    if (firstOfSession && (!kept || !castKept))
        for (int i = before; i < qMin<qsizetype>(before + 3, m_clips.size()); ++i)
            m_clips[i].immediate = true;
    if (m_current < 0 && !m_clips.isEmpty())
        m_current = 0;
    // A voice made more slowly than it's heard needs a head start:
    // enough that, made while it plays, it never runs out.
    if (firstOfSession && !made.isEmpty()) {
        const double speed = speaker() ? speaker()->speedOf(m_voice) : 0;
        if (speed > 0 && speed < 1.2 * m_rate) {
            qint64 characters = 0;
            for (const SpeechClip &clip : made)
                characters += clip.text.size();
            const double seconds = characters * m_samplesPerChar / sampleRate;
            const double head = seconds * qMax(0.0, 1.0 - speed / m_rate) * 1.1 + 2.0;
            m_holdSamples = qint64(head * sampleRate);
            trace(QStringLiteral("head start: %1 s of audio at %2x").arg(head, 0, 'f', 1).arg(speed, 0, 'f', 2));
            m_player->pause();
            emit stateChanged();
        }
    }
    if (!castKept && !made.isEmpty())
        requestCast(chapter);
    if (directed() && m_ai->textFor(QStringLiteral("director")) && !made.isEmpty() && !kept)
        requestDirections(chapter);
    if (!made.isEmpty()) {
        emit positionChanged();
        emit timeChanged();
    }
    pump();
}

bool Narration::allRemainingComplete() const {
    if (m_clips.isEmpty())
        return false;
    const int start = qMax(0, m_current);
    const int currentChapter = (m_current >= 0 && m_current < m_clips.size()) ? m_clips[m_current].chapter : -1;
    for (int i = start; i < m_clips.size(); ++i) {
        if (m_sleepAtChapterEnd && currentChapter >= 0 && m_clips[i].chapter != currentChapter)
            return true;
        if (m_clips[i].state == Clip::Waiting || m_clips[i].state == Clip::Working)
            return false;
    }
    if (m_sleepAtChapterEnd && currentChapter >= 0 && m_loadedChapters.contains(currentChapter))
        return true;
    return m_requested.size() <= m_loadedChapters.size() && nextChapterAfter(m_lastChapter) < 0;
}

int Narration::preparedPercent() const {
    if (m_holdSamples <= 0 || allRemainingComplete())
        return 100;
    qint64 have = 0;
    for (int i = qMax(0, m_current); i < m_clips.size(); ++i)
        have += m_clips[i].pcm.size() / 2;
    return int(qMin<qint64>(100, have * 100 / m_holdSamples));
}

// Plays once the head start is made, or when all remaining clips have finished.
void Narration::checkHeadStart() {
    if (m_holdSamples <= 0)
        return;
    emit positionChanged();
    if (preparedPercent() < 100 && !allRemainingComplete())
        return;
    trace(QStringLiteral("head start made"));
    m_holdSamples = 0;
    if (m_active && m_wanted && !m_responseActive && !m_bookPlayerDirty)
        m_player->play();
    emit stateChanged();
}

qint64 Narration::directionWaitLeft(const Clip &clip) const {
    if (clip.immediate)
        return 0;
    qint64 left = 0;
    if (m_directionsAsked.contains(clip.chapter) && directed() && !m_moods.contains(clip.chapter))
        left = qMax(left, directionsPatienceMs - (nowMs() - m_directionsAsked.value(clip.chapter)));
    if (m_castAsked.contains(clip.chapter) && m_cast && m_cast->enabled() && !m_cast->knows(clip.chapter) &&
        m_cast->available())
        left = qMax(left, directionsPatienceMs - (nowMs() - m_castAsked.value(clip.chapter)));
    return left;
}

bool Narration::waitingForDirections(const Clip &clip) const { return directionWaitLeft(clip) > 0; }

// The voice speaks through its own provider: built-in voices here, the rest
// in the cloud.
Ai::SpeechSynthesizer *Narration::speaker() const { return m_ai->speechFor(m_voice); }

// Moods need a voice that can take direction; built-in voices read plainly.
bool Narration::directed() const {
    const Ai::SpeechSynthesizer *voice = speaker();
    return m_performance != QStringLiteral("plain") && voice && voice->supportsStyle();
}

QString Narration::styleFor(const Clip &clip) const {
    if (!directed())
        return {};
    return Speech::styleFor(Speech::moodAt(m_moods.value(clip.chapter), clip.block),
                            m_performance == QStringLiteral("dramatic"));
}

void Narration::pump() {
    // Do not prefetch, spend, or touch the shared player while paused, inactive,
    // or while Companion response speech owns m_player.
    if (!m_active || !m_wanted || m_responseActive || m_bookPlayerDirty)
        return;
    // Ask for the next chapter's words before these run out, skipping past
    // chapters with nothing to say (a cover, an image page).
    const int remaining = int(m_clips.size()) - qMax(0, m_current);
    if (remaining < 5 && m_lastChapter >= 0) {
        const int next = nextChapterAfter(m_lastChapter);
        if (next >= 0 && !m_requested.contains(next)) {
            m_requested.insert(next);
            emit blocksWanted(next);
        }
    }
    // If every remaining chapter was empty (cover/image pages only), stop cleanly.
    if (m_clips.isEmpty() && m_requested.size() == m_loadedChapters.size() && nextChapterAfter(m_lastChapter) < 0) {
        stop();
        setProblem(tr("Nothing left to read aloud in this book."));
        return;
    }

    int working = 0;
    for (const Clip &clip : std::as_const(m_clips))
        working += clip.state == Clip::Working && clip.reply ? 1 : 0;
    const int from = qMax(0, m_current);
    qint64 wait = -1;
    const bool local = AiServices::isOnThisComputer(m_voice);
    const int ahead = local ? (m_holdSamples > 0 ? 64 : 8) : 4;
    for (int i = from; i < m_clips.size() && i < from + ahead; ++i) {
        Clip &clip = m_clips[i];
        if (clip.state != Clip::Waiting)
            continue;
        if (clip.pauseMs > 0) {
            clip.pcm = QByteArray(int(qint64(clip.pauseMs) * sampleRate / 1000) * 2, '\0');
            clip.samples = clip.pcm.size() / 2;
            clip.state = Clip::Complete;
            continue;
        }
        if (const qint64 left = directionWaitLeft(clip); left > 0) {
            wait = wait < 0 ? left : qMin(wait, left);
            continue;
        }
        clip.style = styleFor(clip);
        // A character's voice that can't speak now (not downloaded, no key)
        // gives way to the narrator's.
        if (!clip.voice.isEmpty() && clip.voice != m_voice && !m_ai->canSpeakWith(clip.voice))
            clip.voice = m_voice;
        QString voice = voiceOf(clip);
        Ai::SpeechSynthesizer *synth = m_ai->speechFor(voice);
        Ai::Provider *provider = m_ai->providerFor(voice);
        if ((!synth || !provider) && voice != m_voice) {
            clip.voice = m_voice;
            voice = m_voice;
            synth = m_ai->speechFor(voice);
            provider = m_ai->providerFor(voice);
        }
        if (!synth || !provider) {
            pause();
            setProblem(AiServices::isBuiltinVoice(m_voice)
                           ? tr("This built-in voice isn't downloaded yet.")
                           : AiServices::isStudioVoice(m_voice)
                                 ? tr("This Studio voice needs its model server running.")
                                 : m_ai->describe(int(Ai::Error::NoKey)));
            updateFallbackOffer();
            return;
        }
        const QString model = synth->speechModelName(m_economy);
        clip.key = AudioCache::keyFor(provider->id(), model, voice, clip.style, clip.text);
        const QString file = m_cache->lookup(clip.key);
        if (!file.isEmpty()) {
            decode(i, file);
            continue;
        }
        if (m_retryTimer.isActive())
            continue;
        // Two requests at a time: enough to stay ahead of the voice, gentle
        // on the key's rate limits. Built-in voices take their turns.
        if (working >= (local ? ahead : 2))
            continue;
        fetch(i);
        ++working;
    }
    if (wait > 0)
        m_directionsTimer.start(int(wait) + 50);
    queue();
    if (m_holdSamples > 0)
        checkHeadStart();
}

// Clips join the player strictly in order, as soon as their audio starts to
// arrive, so the voice plays on without a gap.
void Narration::queue() {
    if (!m_active || !m_wanted || m_responseActive || m_bookPlayerDirty)
        return;
    while (m_queued + 1 < m_clips.size()) {
        const int nextIndex = m_queued + 1;
        Clip &clip = m_clips[nextIndex];
        if (clip.state == Clip::Waiting || clip.state == Clip::Failed ||
            (clip.state == Clip::Working && !clip.reply))
            break;
        // Stop queuing at the chapter boundary when sleeping at chapter end.
        if (m_sleepAtChapterEnd && m_current >= 0 && m_current < m_clips.size() &&
            clip.chapter != m_clips[m_current].chapter)
            break;
        ++m_queued;
        clip.queued = true;
        const qint64 startSample = (nextIndex == m_current && m_currentSample > 0) ? m_currentSample : 0;
        m_player->addTrack(clip.id, startSample);
        if (!clip.pcm.isEmpty())
            m_player->appendAudio(clip.id, clip.pcm);
        if (clip.state == Clip::Complete)
            m_player->finishTrack(clip.id);
    }
    stallIfFailed();
}

// The voice has caught up with a clip that couldn't be made: it stops
// there, with the reason shown, until asked to go on (which tries again).
void Narration::stallIfFailed() {
    if (!m_wanted || m_queued + 1 >= m_clips.size() || m_clips[m_queued + 1].state != Clip::Failed ||
        !m_player->drainedOut())
        return;
    m_wanted = false;
    m_player->pause();
    setBuffering(false);
    emit stateChanged();
}

void Narration::fetch(int index) {
    Clip &clip = m_clips[index];
    Ai::SpeechRequest request;
    request.text = Speech::speakable(clip.text);
    request.style = clip.style;
    request.voice = voiceOf(clip);
    request.economy = m_economy;
    request.stream = true;
    for (const SpeechSentence &sentence : std::as_const(clip.sentences))
        request.sentences.append(Speech::speakable(sentence.text));

    Ai::SpeechSynthesizer *synth = m_ai->speechFor(request.voice);
    if (!synth && request.voice != m_voice) {
        clip.voice = m_voice;
        request.voice = m_voice;
        synth = m_ai->speechFor(request.voice);
    }
    if (!synth) {
        clip.state = Clip::Failed;
        setProblem(AiServices::isBuiltinVoice(m_voice)
                       ? tr("This built-in voice isn't downloaded yet.")
                       : AiServices::isStudioVoice(m_voice)
                             ? tr("This Studio voice needs its model server running.")
                             : m_ai->describe(int(Ai::Error::NoKey)));
        updateFallbackOffer();
        queue();
        return;
    }

    const bool local = AiServices::isOnThisComputer(request.voice);
    const double cost = local ? 0.0 : m_ai->listeningCost(clip.text.size() / charsPerMinute, m_economy, false);
    if (!local && !m_ai->fitsBudget(cost)) {
        if (fallbackToLocalMidSession(index, tr("monthly budget reached")))
            return;
        clip.state = Clip::Failed;
        setProblem(m_ai->describe(int(Ai::Error::Budget)));
        updateFallbackOffer();
        queue();
        return;
    }

    Ai::Reply *reply = synth->synthesize(request);
    m_ai->track(reply);
    clip.reply = reply;
    clip.state = Clip::Working;
    const quint64 session = m_session;
    const int clipId = clip.id;
    trace(QStringLiteral("fetch #%1 (id %2, %3 chars%4)")
              .arg(index)
              .arg(clipId)
              .arg(clip.text.size())
              .arg(clip.style.isEmpty() ? QString() : QStringLiteral(", ") + clip.style));

    connect(reply, &Ai::Reply::audioReceived, this, [this, session, clipId, reply](const QByteArray &pcm) {
        if (!m_active || session != m_session)
            return;
        const int i = indexOfClip(clipId);
        if (i < 0 || m_clips[i].reply != reply)
            return;
        Clip &clip = m_clips[i];
        if (clip.pcm.isEmpty()) {
            trace(QStringLiteral("first audio #%1").arg(i));
            setProblem({});
            clearFallbackOffer();
        }
        clip.pcm += pcm;
        if (const Ai::Reply *from = clip.reply)
            clip.boundaries = from->boundaries();
        const bool canFeedPlayer = m_wanted && !m_responseActive && !m_bookPlayerDirty;
        if (clip.queued) {
            if (canFeedPlayer)
                m_player->appendAudio(clip.id, pcm);
        } else if (canFeedPlayer) {
            queue();
        }
        if (canFeedPlayer)
            checkHeadStart();
    });
    connect(reply, &Ai::Reply::finished, this, [this, session, clipId, reply] {
        reply->deleteLater();
        if (!m_active || session != m_session)
            return;
        const int i = indexOfClip(clipId);
        if (i < 0 || m_clips[i].reply != reply)
            return;
        Clip &clip = m_clips[i];
        clip.reply = nullptr;
        if (reply->error()) {
            const Ai::Error err = reply->error();
            trace(QStringLiteral("failed #%1: %2").arg(i).arg(err.message));
            const bool isCloud = !AiServices::isOnThisComputer(voiceOf(clip));
            if ((err.kind == Ai::Error::Limited || err.kind == Ai::Error::Server) && !clip.queued && clip.tries < 3 &&
                m_wanted) {
                ++clip.tries;
                clip.state = Clip::Waiting;
                clip.pcm = QByteArray();
                setProblem(m_ai->describe(int(err.kind), err.message));
                if (isCloud && err.kind == Ai::Error::Limited)
                    updateFallbackOffer();
                const int delayMs = err.retryAfterSeconds > 0 ? qBound(1000, err.retryAfterSeconds * 1000, 60000)
                                                              : 3000 * clip.tries;
                if (!m_retryTimer.isActive())
                    m_retryTimer.start(delayMs);
                return;
            }
            if (isCloud && !clip.queued &&
                (err.kind == Ai::Error::Offline || err.kind == Ai::Error::Limited || err.kind == Ai::Error::Budget)) {
                const QString reason = err.kind == Ai::Error::Offline ? tr("offline")
                                     : err.kind == Ai::Error::Limited ? tr("Gemini limit reached")
                                                                      : tr("budget reached");
                clip.state = Clip::Waiting;
                clip.pcm = QByteArray();
                if (fallbackToLocalMidSession(i, reason))
                    return;
            }
            clip.state = Clip::Failed;
            clip.pcm = QByteArray();
            if (err.kind != Ai::Error::Cancelled) {
                setProblem(m_ai->describe(int(err.kind), err.message));
                if (isCloud &&
                    (err.kind == Ai::Error::Offline || err.kind == Ai::Error::Limited || err.kind == Ai::Error::Budget ||
                     err.kind == Ai::Error::NoKey || err.kind == Ai::Error::BadKey))
                    updateFallbackOffer();
            }
            const bool canFeedPlayer = m_wanted && !m_responseActive && !m_bookPlayerDirty;
            if (clip.queued && canFeedPlayer) {
                // Part of it was already playing: what arrived is heard, and
                // the voice stops at its end.
                m_player->finishTrack(clip.id);
            }
            if (canFeedPlayer) {
                queue();
                checkHeadStart();
            }
            return;
        }
        clip.state = Clip::Complete;
        clip.tries = 0;
        clip.samples = clip.pcm.size() / 2;
        clip.boundaries = reply->boundaries();
        setProblem({});
        clearFallbackOffer();
        if (!clip.text.isEmpty() && clip.samples > 0)
            m_samplesPerChar = 0.8 * m_samplesPerChar + 0.2 * double(clip.samples) / clip.text.size();
        trace(QStringLiteral("complete #%1 (%2 s)").arg(i).arg(clip.samples / double(sampleRate), 0, 'f', 1));
        const bool canFeedPlayer = m_wanted && !m_responseActive && !m_bookPlayerDirty;
        if (clip.queued && canFeedPlayer)
            m_player->finishTrack(clip.id);
        if (!clip.pcm.isEmpty() && reply->sampleRate() > 0)
            m_cache->store(clip.key, clip.pcm, reply->sampleRate(), clip.boundaries);
        if (m_current >= 0 && m_current < m_clips.size() && clip.chapter == m_clips[m_current].chapter) {
            emit positionChanged();
            emit timeChanged();
        }
        if (canFeedPlayer)
            checkHeadStart();
        pump();
    });
}

void Narration::decode(int index, const QString &file) {
    Clip &clip = m_clips[index];
    clip.state = Clip::Working;
    trace(QStringLiteral("cached #%1").arg(index));
    const QString key = clip.key;
    const int clipId = clip.id;
    const quint64 session = m_session;
    m_cache->decode(file, sampleRate, this, [this, session, clipId, key](const QByteArray &pcm, bool ok) {
        if (!m_active || session != m_session)
            return;
        const int i = indexOfClip(clipId);
        if (i < 0 || m_clips[i].state != Clip::Working || m_clips[i].reply)
            return;
        Clip &clip = m_clips[i];
        if (!ok) {
            // Evict the damaged cache entry before re-queuing so we never loop on it.
            m_cache->remove(key);
            clip.state = Clip::Waiting;
            clip.queued = false;
            clip.key.clear();
            pump();
            return;
        }
        clip.pcm = pcm;
        clip.samples = clip.pcm.size() / 2;
        clip.boundaries = m_cache->boundaries(key);
        clip.state = Clip::Complete;
        const bool canFeedPlayer = m_wanted && !m_responseActive && !m_bookPlayerDirty;
        if (clip.queued && canFeedPlayer) {
            m_player->appendAudio(clip.id, clip.pcm);
            m_player->finishTrack(clip.id);
        }
        if (canFeedPlayer)
            checkHeadStart();
        if (m_current >= 0 && m_current < m_clips.size() && clip.chapter == m_clips[m_current].chapter) {
            emit positionChanged();
            emit timeChanged();
        }
        pump();
    });
}

qint64 Narration::expectedSamples(const Clip &clip) const {
    if (clip.state == Clip::Complete && clip.samples > 0)
        return clip.samples;
    if (clip.pauseMs > 0)
        return qint64(clip.pauseMs) * sampleRate / 1000;
    // Still arriving: judge by its length in letters, at the pace heard so far.
    return qMax(qint64(clip.pcm.size() / 2), qint64(clip.text.size() * m_samplesPerChar));
}

void Narration::heard(int trackId, qint64 sample) {
    if (m_responseActive) {
        const int idx = indexOfResponseChunk(trackId);
        if (idx >= 0) {
            if (m_responseBuffering)
                setResponseBuffering(false);
            if (idx != m_responseCurrent) {
                m_responseCurrent = idx;
                for (int i = 0; i < idx; ++i)
                    if (m_responseChunks[i].state == ResponseChunk::Complete)
                        m_responseChunks[i].pcm = QByteArray();
                pumpResponse();
            }
        }
        return;
    }
    if (!m_active)
        return;
    const int track = indexOfClip(trackId);
    if (track < 0)
        return;
    // The voice is moving.
    if (m_buffering)
        setBuffering(false);
    if (track != m_current) {
        const int previous = m_current;
        const bool newChapter = previous >= 0 && previous < m_clips.size() &&
                                m_clips[previous].chapter != m_clips[track].chapter;
        if (newChapter && m_sleepAtChapterEnd) {
            m_sleepAtChapterEnd = false;
            m_current = track;
            m_currentSample = 0;
            m_sentence = -1;
            m_bookPlayerDirty = true;
            m_player->clear();
            pause();
            if (!m_clips[track].sentences.isEmpty())
                showSentenceForSample(track, 0, 0);
            else
                emit positionChanged();
            emit timeChanged();
            emit sleepChanged();
            return;
        }
        m_current = track;
        m_currentSample = 0;
        m_sentence = -1;
        trace(QStringLiteral("hearing #%1 (id %2)").arg(track).arg(trackId));
        // Keep two clips behind in memory so -15s rewind is instantaneous;
        // older clips are in the cache.
        for (int i = 0; i < track - 2; ++i)
            if (m_clips[i].state == Clip::Complete)
                m_clips[i].pcm = QByteArray();
        emit positionChanged();
        emit timeChanged();
        pump();
    }
    m_currentSample = qMax<qint64>(0, sample);
    showSentenceForSample(track, m_currentSample);
    const qint64 now = nowMs();
    const qint64 sec = positionUs() / 1000000LL;
    if (sec != m_lastReportedSec || now - m_lastTimeEmitMs >= timeEmitIntervalMs) {
        m_lastReportedSec = sec;
        m_lastTimeEmitMs = now;
        emit timeChanged();
    }
}

void Narration::showSentenceForSample(int index, qint64 sample, int forceSentence) {
    if (index < 0 || index >= m_clips.size())
        return;
    const Clip &clip = m_clips[index];
    if (clip.sentences.isEmpty())
        return;
    if (forceSentence >= 0 && forceSentence < clip.sentences.size()) {
        showSentenceAt(forceSentence);
        return;
    }
    // Where the voice said each sentence ends, when it did; otherwise by
    // the sentences' share of the clip.
    if (!clip.boundaries.isEmpty() && clip.boundaries.size() <= clip.sentences.size()) {
        int at = 0;
        while (at < clip.boundaries.size() && sample >= clip.boundaries[at])
            ++at;
        showSentenceAt(qMin(at, int(clip.sentences.size()) - 1));
        return;
    }
    const qint64 total = expectedSamples(clip);
    showSentence(total > 0 ? double(sample) / double(total) : 0);
}

qint64 Narration::sampleOfSentence(const Clip &clip, int sentence) const {
    if (sentence <= 0 || clip.sentences.isEmpty())
        return 0;
    if (!clip.boundaries.isEmpty() && sentence - 1 < clip.boundaries.size())
        return qMax<qint64>(0, clip.boundaries[sentence - 1]);
    if (sentence < clip.spans.size()) {
        const qint64 total = expectedSamples(clip);
        if (total > 0)
            return qBound<qint64>(0, qint64(std::ceil(clip.spans[sentence].first * double(total))), total - 1);
    }
    return 0;
}

void Narration::showSentenceAt(int at) {
    if (m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    if (at < 0 || at >= clip.sentences.size() || at == m_sentence)
        return;
    m_sentence = at;
    m_sentenceStart = clip.sentences[at].start;
    m_sentenceEnd = clip.sentences[at].end;
    emit positionChanged();
    emit speaking(clip.chapter, m_sentenceStart, m_sentenceEnd);
}

void Narration::showSentence(double fraction) {
    if (m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    if (clip.sentences.isEmpty())
        return;
    const auto &spans = clip.spans;
    int at = int(spans.size()) - 1;
    for (int i = 0; i < spans.size(); ++i)
        if (fraction < spans[i].second) {
            at = i;
            break;
        }
    showSentenceAt(at);
}

void Narration::jumpTo(int index, qint64 sampleOffset, bool preservePause, int forceSentence) {
    if (index < 0 || index >= m_clips.size())
        return;
    if (m_responseActive)
        stopResponse();
    m_bookPlayerDirty = false;
    const qint64 clampedSample = qMax<qint64>(0, sampleOffset);
    trace(QStringLiteral("jump to #%1 @ %2").arg(index).arg(clampedSample));
    m_holdSamples = 0;
    m_retryTimer.stop();
    setProblem({});
    m_player->clear();
    const int ahead = AiServices::isOnThisComputer(m_voice) ? 8 : 4;
    for (int i = 0; i < m_clips.size(); ++i) {
        Clip &clip = m_clips[i];
        if (i < index || i >= index + ahead) {
            if (clip.reply) {
                QPointer<Ai::Reply> obsolete = clip.reply;
                clip.reply = nullptr;
                obsolete->abort();
                if (clip.state == Clip::Working) {
                    clip.state = Clip::Waiting;
                    clip.pcm = QByteArray();
                }
            }
        }
        if (i >= index) {
            clip.queued = false;
            clip.tries = 0;
            if (clip.state == Clip::Complete && clip.pcm.isEmpty())
                clip.state = Clip::Waiting;
            if (clip.state == Clip::Failed)
                clip.state = Clip::Waiting;
        }
    }
    const bool playAfter = preservePause ? m_wanted : true;
    m_current = index;
    m_currentSample = clampedSample;
    m_queued = index - 1;
    m_sentence = -1;
    m_wanted = playAfter;
    if (playAfter) {
        if (m_sleepPausedRestore || m_sleepFading)
            restoreSleepVolume();
        setBuffering(m_clips[index].state != Clip::Complete);
        m_player->play();
    } else {
        setBuffering(false);
    }
    emit stateChanged();
    if (playAfter)
        pump();
    if (!m_clips[index].sentences.isEmpty())
        showSentenceForSample(index, clampedSample, forceSentence);
    else
        emit positionChanged();
    const qint64 posUs = positionUs();
    m_lastTimeEmitMs = nowMs();
    m_lastReportedSec = posUs / 1000000LL;
    emit timeChanged();
    emit seeked(posUs);
}

void Narration::nextParagraph() {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &now = m_clips[m_current];
    for (int i = m_current + 1; i < m_clips.size(); ++i)
        if (m_clips[i].pauseMs == 0 && (m_clips[i].block != now.block || m_clips[i].chapter != now.chapter)) {
            jumpTo(i);
            return;
        }
}

void Narration::previousParagraph() {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    // A little way into a clip, back to its start; at its start, the one before.
    if (m_sentence > 0 && m_clips[m_current].pauseMs == 0) {
        jumpTo(m_current);
        return;
    }
    int target = m_current - 1;
    while (target > 0 && m_clips[target].pauseMs > 0)
        --target;
    jumpTo(qMax(0, target));
}

void Narration::nextSentence() {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    const int curS = qMax(0, m_sentence);
    if (clip.pauseMs == 0 && curS + 1 < clip.sentences.size()) {
        const qint64 sample = sampleOfSentence(clip, curS + 1);
        jumpTo(m_current, sample, true, curS + 1);
        return;
    }
    for (int i = m_current + 1; i < m_clips.size(); ++i) {
        if (m_clips[i].pauseMs == 0 && !m_clips[i].sentences.isEmpty()) {
            jumpTo(i, 0, true, 0);
            return;
        }
    }
    const int nextCh = nextChapterAfter(m_lastChapter);
    if (nextCh >= 0 && !m_requested.contains(nextCh)) {
        m_requested.insert(nextCh);
        emit blocksWanted(nextCh);
    }
}

void Narration::prevSentence() {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    const int curS = qMax(0, m_sentence);
    if (clip.pauseMs == 0 && curS > 0) {
        const qint64 sample = sampleOfSentence(clip, curS - 1);
        jumpTo(m_current, sample, true, curS - 1);
        return;
    }
    for (int i = m_current - 1; i >= 0; --i) {
        if (m_clips[i].pauseMs == 0 && !m_clips[i].sentences.isEmpty()) {
            const int lastS = int(m_clips[i].sentences.size()) - 1;
            const qint64 sample = sampleOfSentence(m_clips[i], lastS);
            jumpTo(i, sample, true, lastS);
            return;
        }
    }
    jumpTo(m_current, 0, true, 0);
}

void Narration::skipSeconds(int delta) {
    if (delta == 0)
        return;
    seekBySamples(qint64(delta) * sampleRate);
}

void Narration::seekByUs(qint64 deltaUs) {
    if (deltaUs == 0)
        return;
    seekBySamples(usToSamples(deltaUs, sampleRate));
}

void Narration::seekBySamples(qint64 deltaSamples) {
    if (!m_active || m_clips.isEmpty() || m_current < 0 || m_current >= m_clips.size())
        return;
    int idx = m_current;
    qint64 pos = m_currentSample + deltaSamples;

    if (deltaSamples > 0) {
        while (idx < m_clips.size()) {
            const qint64 len = qMax<qint64>(1, expectedSamples(m_clips[idx]));
            if (pos < len)
                break;
            if (idx + 1 >= m_clips.size()) {
                const int nextCh = nextChapterAfter(m_lastChapter);
                if (nextCh >= 0 && !m_requested.contains(nextCh)) {
                    m_requested.insert(nextCh);
                    emit blocksWanted(nextCh);
                }
                pos = qMax<qint64>(0, len - sampleRate);
                break;
            }
            pos -= len;
            ++idx;
        }
        while (idx + 1 < m_clips.size() && m_clips[idx].pauseMs > 0) {
            ++idx;
            pos = 0;
        }
    } else {
        while (pos < 0 && idx > 0) {
            --idx;
            const qint64 len = qMax<qint64>(1, expectedSamples(m_clips[idx]));
            pos += len;
        }
        if (pos < 0)
            pos = 0;
        if (m_clips[idx].pauseMs > 0) {
            if (idx > 0) {
                --idx;
                pos = qMax<qint64>(0, expectedSamples(m_clips[idx]) - sampleRate);
            } else if (idx + 1 < m_clips.size()) {
                ++idx;
                pos = 0;
            }
        }
    }
    jumpTo(idx, pos, true);
}

void Narration::seekToUs(qint64 targetUs) {
    if (!m_active || m_clips.isEmpty() || m_current < 0 || m_current >= m_clips.size())
        return;
    const int at = m_clips[m_current].chapter;
    qint64 targetSamples = qMax<qint64>(0, usToSamples(targetUs, sampleRate));
    int firstInChapter = -1;
    int lastInChapter = -1;
    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].chapter == at) {
            if (firstInChapter < 0)
                firstInChapter = i;
            lastInChapter = i;
        }
    }
    if (firstInChapter < 0)
        return;
    int idx = firstInChapter;
    qint64 pos = targetSamples;
    while (idx < lastInChapter) {
        const qint64 len = qMax<qint64>(1, expectedSamples(m_clips[idx]));
        if (pos < len)
            break;
        pos -= len;
        ++idx;
    }
    const qint64 lastLen = qMax<qint64>(1, expectedSamples(m_clips[idx]));
    pos = qBound<qint64>(0, pos, qMax<qint64>(0, lastLen - 1));
    if (m_clips[idx].pauseMs > 0 && idx + 1 <= lastInChapter) {
        ++idx;
        pos = 0;
    }
    jumpTo(idx, pos, true);
}

void Narration::toggle() {
    if (m_responseActive)
        stopResponse();
    playing() ? pause() : resume();
}

void Narration::pause() {
    if (m_responseActive)
        stopResponse();
    if (!m_active)
        return;
    trace(QStringLiteral("pause"));
    const bool wasFading = m_sleepFading;
    if (m_sleepFading && !m_sleepPausedRestore)
        restoreSleepVolume();
    m_wanted = false;
    m_directionsTimer.stop();
    m_retryTimer.stop();
    m_player->pause();
    setBuffering(false);
    emit stateChanged();
    if (wasFading)
        emit sleepChanged();
}

void Narration::resume() {
    if (m_responseActive)
        stopResponse();
    if (!m_active)
        return;
    trace(QStringLiteral("resume"));
    setProblem({});
    const bool wasFading = m_sleepFading;
    if (m_sleepPausedRestore || m_sleepFading)
        restoreSleepVolume();
    if (m_bookPlayerDirty) {
        m_bookPlayerDirty = false;
        m_player->clear();
        if (m_current >= 0 && m_current < m_clips.size()) {
            m_queued = m_current - 1;
            for (int i = m_current; i < m_clips.size(); ++i) {
                m_clips[i].queued = false;
                if (m_clips[i].state == Clip::Complete && m_clips[i].pcm.isEmpty())
                    m_clips[i].state = Clip::Waiting;
            }
        }
    }
    // A clip that couldn't be made is tried again.
    for (int i = qMax(0, m_current); i < m_clips.size(); ++i)
        if (m_clips[i].state == Clip::Failed) {
            m_clips[i].state = Clip::Waiting;
            m_clips[i].tries = 0;
        }
    m_wanted = true;
    if (m_holdSamples > 0)
        checkHeadStart();
    if (m_holdSamples <= 0)
        m_player->play();
    emit stateChanged();
    if (wasFading)
        emit sleepChanged();
    pump();
}

void Narration::stop() {
    stopResponse();
    m_bookPlayerDirty = false;
    ++m_session;
    // The clips are let go first, so a request's ending finds nothing to update.
    const QList<Clip> clips = std::exchange(m_clips, {});
    for (const Clip &clip : clips)
        if (clip.reply)
            clip.reply->abort();
    m_director->cancel();
    if (m_cast)
        m_cast->cancel(this);
    m_directionsTimer.stop();
    m_retryTimer.stop();
    const bool hadSleep = m_sleepAt.isValid() || m_sleepAtChapterEnd || m_sleepFading;
    m_sleepTimer.stop();
    m_sleepTick.stop();
    m_sleepAt.clear();
    m_lastSleepSec = 0;
    m_sleepAtChapterEnd = false;
    if (m_sleepFading || m_sleepPausedRestore)
        restoreSleepVolume();
    m_player->pause();
    m_player->clear();
    m_current = -1;
    m_queued = -1;
    m_currentSample = 0;
    m_lastTimeEmitMs = 0;
    m_lastReportedSec = -1;
    m_requested.clear();
    m_loadedChapters.clear();
    m_directionsAsked.clear();
    m_castAsked.clear();
    m_directorBatchesAsked.clear();
    m_castBatchesAsked.clear();
    m_blocks.clear();
    m_moods.clear();
    m_lastChapter = -1;
    m_holdSamples = 0;
    m_sentence = -1;
    m_sentenceStart = m_sentenceEnd = -1;
    clearFallbackOffer();
    setFallbackNotice({});
    const bool was = m_active;
    m_active = false;
    m_wanted = false;
    m_buffering = false;
    if (hadSleep)
        emit sleepChanged();
    if (was) {
        emit stateChanged();
        emit positionChanged();
        emit timeChanged();
    }
}

int Narration::minutesLeftInChapter() const {
    if (m_current < 0 || m_current >= m_clips.size())
        return 0;
    const int at = m_clips[m_current].chapter;
    qint64 chars = 0;
    for (int i = m_current; i < m_clips.size() && m_clips[i].chapter == at; ++i)
        chars += m_clips[i].text.size();
    return chars > 0 ? qMax(1, int(std::ceil(chars / (charsPerMinute * m_rate)))) : 0;
}

qint64 Narration::positionUs() const {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return 0;
    const int at = m_clips[m_current].chapter;
    qint64 samples = 0;
    for (int i = 0; i < m_current; ++i)
        if (m_clips[i].chapter == at)
            samples += expectedSamples(m_clips[i]);
    samples += qMin(m_currentSample, expectedSamples(m_clips[m_current]));
    return samples * 1000000LL / sampleRate;
}

qint64 Narration::durationUs() const {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return 0;
    const int at = m_clips[m_current].chapter;
    qint64 samples = 0;
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips[i].chapter == at)
            samples += expectedSamples(m_clips[i]);
    return samples * 1000000LL / sampleRate;
}

void Narration::restoreSleepVolume() {
    const double saved = m_preFadeVolume;
    m_preFadeVolume = -1.0;
    m_sleepFading = false;
    m_sleepPausedRestore = false;
    if (saved >= 0.0)
        m_player->setVolume(saved);
}

void Narration::onSleepTick() {
    if (!m_sleepAt.isValid()) {
        m_sleepTick.stop();
        return;
    }
    const qint64 leftMs = m_sleepAt.remainingMs();
    if (leftMs <= 0) {
        finishSleep();
        return;
    }
    if (leftMs <= sleepFadeMs && m_active && m_wanted) {
        if (!m_sleepFading) {
            m_sleepFading = true;
            if (m_preFadeVolume < 0.0)
                m_preFadeVolume = m_player->volume();
            m_sleepTick.setInterval(100);
        }
        const double t = qBound(0.0, double(leftMs) / double(sleepFadeMs), 1.0);
        m_player->setVolume(m_preFadeVolume * t * t);
    } else if (m_sleepFading && !m_wanted) {
        restoreSleepVolume();
        m_sleepTick.setInterval(1000);
        emit sleepChanged();
    } else if (!m_wanted && m_sleepTick.interval() < 1000) {
        m_sleepTick.setInterval(1000);
    }
    const int secs = sleepSecondsLeft();
    if (secs != m_lastSleepSec || m_sleepFading) {
        m_lastSleepSec = secs;
        emit sleepChanged();
    }
}

void Narration::finishSleep() {
    if (m_responseActive)
        stopResponse();
    m_sleepTimer.stop();
    m_sleepTick.stop();
    m_sleepAt.clear();
    m_lastSleepSec = 0;
    if (!m_active) {
        if (m_sleepFading || m_sleepPausedRestore)
            restoreSleepVolume();
        emit sleepChanged();
        return;
    }
    if (m_preFadeVolume < 0.0)
        m_preFadeVolume = m_player->volume();
    m_player->setVolume(0.0);
    m_sleepFading = false;
    m_sleepPausedRestore = true;
    pause();
    emit sleepChanged();
}

void Narration::sleepIn(int minutes) {
    const bool wasChapterEnd = m_sleepAtChapterEnd;
    m_sleepAtChapterEnd = false;
    if (m_sleepFading || m_sleepPausedRestore)
        restoreSleepVolume();
    if (minutes <= 0) {
        m_sleepTimer.stop();
        m_sleepTick.stop();
        m_sleepAt.clear();
        m_lastSleepSec = 0;
    } else {
        const qint64 durationMs = qint64(minutes) * 60LL * 1000LL;
        m_sleepAt.startMs(durationMs);
        m_lastSleepSec = int(qMin<qint64>(qint64(minutes) * 60LL, std::numeric_limits<int>::max()));
        m_sleepTimer.start(int(qMin<qint64>(durationMs, std::numeric_limits<int>::max())));
        m_sleepTick.setInterval(1000);
        m_sleepTick.start();
    }
    emit sleepChanged();
    if (wasChapterEnd && m_active && m_wanted)
        pump();
}

void Narration::sleepAtEndOfChapter() {
    if (m_sleepFading || m_sleepPausedRestore)
        restoreSleepVolume();
    m_sleepTimer.stop();
    m_sleepTick.stop();
    m_sleepAt.clear();
    m_lastSleepSec = 0;
    m_sleepAtChapterEnd = true;
    // Trim any next-chapter clips already pushed into m_player so playback stops
    // cleanly at the current chapter boundary instead of spilling into Chapter N+1.
    if (m_active && m_current >= 0 && m_current < m_clips.size()) {
        const int currentChapter = m_clips[m_current].chapter;
        bool nextChapterQueued = false;
        for (int i = m_current; i <= m_queued && i < m_clips.size(); ++i) {
            if (m_clips[i].chapter != currentChapter) {
                nextChapterQueued = true;
                break;
            }
        }
        if (nextChapterQueued) {
            if (!m_responseActive && !m_bookPlayerDirty) {
                const bool playAfter = m_wanted && m_holdSamples <= 0;
                m_player->clear();
                m_queued = m_current - 1;
                for (int i = m_current; i < m_clips.size(); ++i) {
                    m_clips[i].queued = false;
                    if (m_clips[i].state == Clip::Complete && m_clips[i].pcm.isEmpty())
                        m_clips[i].state = Clip::Waiting;
                }
                if (playAfter)
                    m_player->play();
                queue();
            } else {
                for (int i = m_current; i < m_clips.size(); ++i)
                    if (m_clips[i].chapter != currentChapter)
                        m_clips[i].queued = false;
            }
        }
        if (m_holdSamples > 0)
            checkHeadStart();
    }
    emit sleepChanged();
}

int Narration::sleepMinutes() const {
    if (!m_sleepAt.isValid())
        return 0;
    return qMax(1, int(std::ceil(m_sleepAt.remainingMs() / 60000.0)));
}

int Narration::sleepSecondsLeft() const {
    if (!m_sleepAt.isValid())
        return 0;
    return int(qMax<qint64>(0, (m_sleepAt.remainingMs() + 999) / 1000));
}

bool Narration::keptDirections(int chapter) {
    const auto kept = m_director->kept(chapter, m_blocks.value(chapter), m_performance);
    if (!kept)
        return false;
    trace(QStringLiteral("director (kept): %1 changes for chapter %2").arg(kept->size()).arg(chapter));
    m_moods.insert(chapter, *kept);
    return true;
}

QSet<int> Narration::breaksFor(int chapter) const {
    QSet<int> breaks = Director::breaks(m_moods.value(chapter));
    if (m_cast)
        breaks.unite(m_cast->breaks(chapter, m_blocks.value(chapter)));
    return breaks;
}

QMap<int, QString> Narration::blockVoicesFor(int chapter, const QList<SpeechBlock> &blocks) const {
    return m_cast ? m_cast->blockVoices(chapter, blocks) : QMap<int, QString>{};
}

Narration::Clip Narration::clipFrom(const SpeechClip &spoken) {
    Clip clip;
    static_cast<SpeechClip &>(clip) = spoken;
    clip.id = m_nextClipId++;
    clip.spans = Speech::timeline(clip);
    clip.voice = !spoken.voice.isEmpty() ? spoken.voice : (m_cast ? m_cast->voiceAt(spoken.chapter, spoken.block) : m_voice);
    return clip;
}

QString Narration::voiceOf(const Clip &clip) const { return clip.voice.isEmpty() ? m_voice : clip.voice; }

void Narration::setCast(Cast *cast) {
    m_cast = cast;
    if (!cast)
        return;
    cast->setNarrator(m_voice);
    // A voice changed by hand, or the cast turned on or off: what's not yet
    // heard is read again by whoever reads it now.
    connect(cast, &Cast::recast, this, [this] {
        if (!m_active || !m_cast)
            return;
        const bool isPdf = m_reader && m_reader->format() == QLatin1String("pdf");
        QSet<int> chapters;
        for (const Clip &clip : std::as_const(m_clips))
            chapters.insert(clip.chapter);
        for (int chapter : chapters) {
            if (m_cast->enabled() && m_cast->available() && !m_cast->knows(chapter)) {
                if (isPdf) {
                    if (const auto byPage = m_cast->keptPdfBatch(chapter)) {
                        for (auto it = byPage->cbegin(); it != byPage->cend(); ++it)
                            m_cast->learn(it.key(), it.value());
                    } else {
                        requestCast(chapter);
                    }
                } else if (const auto lines = m_cast->kept(chapter, m_blocks.value(chapter))) {
                    m_cast->learn(chapter, *lines);
                } else {
                    requestCast(chapter);
                }
            }
            recut(chapter);
        }
    });
}

void Narration::requestCast(int chapter) {
    if (!m_cast)
        return;
    const quint64 session = m_session;
    const int bookId = m_bookId;
    if (m_reader && m_reader->format() == QLatin1String("pdf")) {
        const auto [first, last] = Director::pdfBatchRange(m_reader, chapter);
        if (m_castBatchesAsked.contains(first))
            return;
        m_castBatchesAsked.insert(first);
        const qint64 askedAt = nowMs();
        for (int p = first; p <= last; ++p)
            m_castAsked.insert(p, askedAt);
        m_cast->askPdfBatch(chapter, this, [this, session, bookId, first, last](const QHash<int, QMap<int, Cast::Line>> &byPage) {
            if (!m_active || session != m_session || m_reader->bookId() != bookId || !m_cast)
                return;
            m_castBatchesAsked.remove(first);
            for (int p = first; p <= last; ++p) {
                m_castAsked.remove(p);
                m_cast->learn(p, byPage.value(p));
                if (m_blocks.contains(p))
                    recut(p);
            }
            trace(QStringLiteral("cast pdf batch %1-%2").arg(first).arg(last));
        });
        return;
    }
    m_castAsked.insert(chapter, nowMs());
    m_cast->ask(chapter, m_blocks.value(chapter), this, [this, session, bookId, chapter](const QMap<int, Cast::Line> &lines) {
        if (!m_active || session != m_session || m_reader->bookId() != bookId || !m_blocks.contains(chapter) || !m_cast)
            return;
        m_castAsked.remove(chapter);
        m_cast->learn(chapter, lines);
        trace(QStringLiteral("cast: chapter %1: %2 lines").arg(chapter).arg(lines.size()));
        recut(chapter);
    });
}

void Narration::requestDirections(int chapter) {
    const quint64 session = m_session;
    const int bookId = m_bookId;
    if (m_reader && m_reader->format() == QLatin1String("pdf")) {
        const auto [first, last] = Director::pdfBatchRange(m_reader, chapter);
        if (m_directorBatchesAsked.contains(first))
            return;
        m_directorBatchesAsked.insert(first);
        const qint64 askedAt = nowMs();
        for (int p = first; p <= last; ++p)
            m_directionsAsked.insert(p, askedAt);
        m_director->askPdfBatch(chapter, m_performance, this,
                                [this, session, bookId, first, last](const QHash<int, QMap<int, QString>> &byPage) {
                                    if (!m_active || session != m_session || m_reader->bookId() != bookId)
                                        return;
                                    m_directorBatchesAsked.remove(first);
                                    bool anyRecut = false;
                                    for (int p = first; p <= last; ++p) {
                                        m_directionsAsked.remove(p);
                                        const QMap<int, QString> moods = byPage.value(p);
                                        m_moods.insert(p, moods);
                                        if (m_blocks.contains(p) && !moods.isEmpty()) {
                                            recut(p);
                                            anyRecut = true;
                                        }
                                    }
                                    trace(QStringLiteral("director pdf batch %1-%2").arg(first).arg(last));
                                    if (!anyRecut)
                                        pump();
                                });
        return;
    }
    m_directionsAsked.insert(chapter, nowMs());
    m_director->ask(chapter, m_blocks.value(chapter), m_performance, this,
                    [this, session, bookId, chapter](const QMap<int, QString> &moods) {
                        if (!m_active || session != m_session || m_reader->bookId() != bookId || !m_blocks.contains(chapter))
                            return;
                        QStringList said;
                        for (auto it = moods.cbegin(); it != moods.cend(); ++it)
                            said.append(QStringLiteral("%1 %2").arg(it.key()).arg(it.value().isEmpty() ? "neutral" : it.value()));
                        trace(QStringLiteral("director: chapter %1: %2").arg(chapter).arg(said.join(", ")));
                        applyMoods(chapter, moods);
                    });
}

// The chapter's clips not yet begun are made again, so each change of mood
// begins a clip of its own and every clip is read in one mood.
void Narration::applyMoods(int chapter, const QMap<int, QString> &moods) {
    m_moods.insert(chapter, moods);
    if (!moods.isEmpty())
        recut(chapter);
    else
        pump();
}

void Narration::recut(int chapter) {
    int end = -1;
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips[i].chapter == chapter)
            end = i + 1;
    int from = end;
    while (from > 0 && m_clips[from - 1].chapter == chapter && m_clips[from - 1].state == Clip::Waiting &&
           !m_clips[from - 1].immediate && !m_clips[from - 1].queued && (from - 1) > m_queued && (from - 1) > m_current)
        --from;
    if (end > 0 && from < end) {
        const int currentId = (m_current >= 0 && m_current < m_clips.size()) ? m_clips[m_current].id : -1;
        const int queuedId = (m_queued >= 0 && m_queued < m_clips.size()) ? m_clips[m_queued].id : -1;
        const QList<SpeechBlock> blocks = m_blocks.value(chapter);
        const Clip &first = m_clips[from];
        const qint64 offset = first.pauseMs > 0 ? blocks.value(first.block).start : first.start();
        const QList<SpeechClip> remade = Speech::clipsFrom(chapter, blocks, offset, breaksFor(chapter),
                                                           blockVoicesFor(chapter, blocks), m_voice);
        m_clips.remove(from, end - from);
        int at = from;
        for (const SpeechClip &spoken : remade)
            m_clips.insert(at++, clipFrom(spoken));
        if (currentId >= 0)
            m_current = indexOfClip(currentId);
        if (queuedId >= 0)
            m_queued = indexOfClip(queuedId);
    }
    pump();
}

// --- Companion response speech playback --------------------------------------

QString Narration::cleanSpeechText(const QString &markdownOrText) {
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

QString Narration::resolveResponseVoice() const {
    if (!m_ai)
        return {};
    auto tryLocal = [this]() -> QString {
        const auto [fbId, fbName] = bestLocalVoice();
        Q_UNUSED(fbName);
        if (!fbId.isEmpty() && m_ai->canSpeakWith(fbId))
            return fbId;
        if (auto *local = qobject_cast<LocalSpeech *>(m_ai->providerFor(QStringLiteral("builtin:")))) {
            const QVariantMap any = local->bestInstalledVoice({});
            const QString anyId = any.value(QStringLiteral("id")).toString();
            if (!anyId.isEmpty() && m_ai->canSpeakWith(anyId))
                return anyId;
        }
        return {};
    };

    QString candidate = m_voice;
    if (candidate.isEmpty()) {
        if (m_library && m_reader && m_reader->bookId() > 0)
            candidate = m_library->bookSetting(m_reader->bookId(), QStringLiteral("voice"));
        if (candidate.isEmpty())
            candidate = settings().value(QStringLiteral("listen/voice")).toString();
    }

    if (!candidate.isEmpty() && m_ai->canSpeakWith(candidate)) {
        if (!AiServices::isOnThisComputer(candidate) && !m_ai->fitsBudget(0.001)) {
            if (const QString local = tryLocal(); !local.isEmpty())
                return local;
        }
        return candidate;
    }

    if (const QString local = tryLocal(); !local.isEmpty())
        return local;
    if (m_ai->canSpeakWith(QStringLiteral("sulafat")))
        return QStringLiteral("sulafat");
    return {};
}

int Narration::indexOfResponseChunk(int trackId) const {
    for (int i = 0; i < m_responseChunks.size(); ++i)
        if (m_responseChunks[i].trackId == trackId)
            return i;
    return -1;
}

void Narration::setResponseBuffering(bool buffering) {
    if (buffering == m_responseBuffering)
        return;
    m_responseBuffering = buffering;
    emit responseSpeechChanged();
}

void Narration::failResponse(const QString &message) {
    stopResponse();
    if (!message.isEmpty()) {
        setProblem(message);
        emit responseSpeechProblem(message);
    }
}

void Narration::speakResponse(const QString &markdownOrText, const QString &itemId) {
    const QString id = itemId.trimmed();
    // Clicking the active/buffering item again toggles playback off cleanly.
    if ((m_responseActive || m_responseBuffering) && m_responseId == id) {
        stopResponse();
        return;
    }

    const QString cleaned = cleanSpeechText(markdownOrText);
    if (cleaned.isEmpty()) {
        stopResponse();
        return;
    }

    const QString voice = resolveResponseVoice();
    if (voice.isEmpty()) {
        failResponse(tr("Choose or download a voice first (P) to listen to Companion responses."));
        return;
    }
    if (!AiServices::isOnThisComputer(voice) && m_ai && !m_ai->fitsBudget(0.001)) {
        failResponse(m_ai->describe(int(Ai::Error::Budget)));
        return;
    }
    const bool headless = m_player->isHeadless();
    if (!headless && QMediaDevices::defaultAudioOutput().isNull()) {
        failResponse(SpeechPlayer::tr("No audio output device is available."));
        return;
    }

    // Never play two voices simultaneously: pause active book narration cleanly first.
    if (playing())
        pause();
    stopResponse();
    setProblem({});
    if (m_active)
        m_bookPlayerDirty = true;
    if (m_reader && m_reader->bookId() > 0)
        m_bookId = m_reader->bookId();

    // Split into clean sequential sentences/chunks so the first sentence begins immediately.
    QStringList sentences;
    const QStringList paragraphs = cleaned.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &paragraph : paragraphs) {
        SpeechBlock block;
        block.text = paragraph;
        block.end = paragraph.size();
        block.kind = QStringLiteral("text");
        const QList<SpeechSentence> split = Speech::sentences(block);
        for (const SpeechSentence &s : split)
            sentences.append(splitOverlongResponseSentence(s.text, 450));
    }
    if (sentences.isEmpty())
        return;

    QList<ResponseChunk> chunks;
    auto flushChunk = [&](QStringList &group) {
        if (group.isEmpty())
            return;
        ResponseChunk chunk;
        chunk.trackId = m_nextClipId++;
        chunk.sentences = group;
        chunk.text = group.join(QLatin1Char(' '));
        chunk.voice = voice;
        chunks.append(std::move(chunk));
        group.clear();
    };

    QStringList current;
    qsizetype currentChars = 0;
    for (const QString &sentence : std::as_const(sentences)) {
        // Keep the very first chunk snappy (1 sentence, or merging a tiny <40-char opener up to 220 chars).
        const bool buildingFirst = chunks.isEmpty();
        const qsizetype limit = buildingFirst ? 220 : 450;
        const bool firstDone = buildingFirst && !current.isEmpty() && currentChars >= 40;
        if (!current.isEmpty() && (firstDone || currentChars + 1 + sentence.size() > limit)) {
            flushChunk(current);
            currentChars = 0;
        }
        current.append(sentence);
        currentChars += sentence.size() + (current.size() > 1 ? 1 : 0);
    }
    flushChunk(current);
    if (chunks.isEmpty())
        return;

    ++m_responseSession;
    m_responseChunks = std::move(chunks);
    m_responseCurrent = 0;
    m_responseQueued = -1;
    m_responseVoice = voice;
    m_responseId = id;
    m_responseActive = true;
    m_responseBuffering = true;
    m_responseHeadless = headless;

    m_player->clear();
    if (!m_responseHeadless)
        m_player->play();
    emit responseSpeechChanged();
    pumpResponse();
}

void Narration::stopResponse() {
    if (!m_responseActive && !m_responseBuffering && m_responseId.isEmpty() && m_responseChunks.isEmpty())
        return;
    ++m_responseSession;
    const QList<ResponseChunk> chunks = std::exchange(m_responseChunks, {});
    for (const ResponseChunk &chunk : chunks)
        if (chunk.reply)
            chunk.reply->abort();
    m_responseCurrent = 0;
    m_responseQueued = -1;
    m_responseVoice.clear();
    const bool ownedPlayer = m_responseActive;
    m_responseActive = false;
    m_responseBuffering = false;
    m_responseHeadless = false;
    m_responseId.clear();
    if (ownedPlayer) {
        m_player->pause();
        m_player->clear();
    }
    emit responseSpeechChanged();
}

void Narration::pumpResponse() {
    if (!m_responseActive || !m_ai)
        return;
    int working = 0;
    for (const ResponseChunk &chunk : std::as_const(m_responseChunks))
        working += (chunk.state == ResponseChunk::Working && chunk.reply) ? 1 : 0;

    const int from = qMax(0, m_responseCurrent);
    const bool local = AiServices::isOnThisComputer(m_responseVoice);
    const int ahead = local ? 3 : 2;
    const int maxWorking = local ? 2 : 1;

    for (int i = from; i < m_responseChunks.size() && i < from + ahead; ++i) {
        ResponseChunk &chunk = m_responseChunks[i];
        if (chunk.state != ResponseChunk::Waiting)
            continue;
        if (chunk.voice.isEmpty())
            chunk.voice = m_responseVoice;
        Ai::SpeechSynthesizer *synth = m_ai->speechFor(chunk.voice);
        Ai::Provider *provider = m_ai->providerFor(chunk.voice);
        if (!synth || !provider) {
            failResponse(tr("Choose or download a voice first (P) to listen to Companion responses."));
            return;
        }
        if (m_cache) {
            const QString model = synth->speechModelName(m_economy);
            chunk.key = AudioCache::keyFor(provider->id(), model, chunk.voice, {}, chunk.text);
            const QString file = m_cache->lookup(chunk.key);
            if (!file.isEmpty()) {
                decodeResponse(i, file);
                continue;
            }
        }
        if (working >= maxWorking)
            continue;
        fetchResponse(i);
        ++working;
    }
    queueResponse();
}

void Narration::queueResponse() {
    while (m_responseQueued + 1 < m_responseChunks.size()) {
        const int nextIndex = m_responseQueued + 1;
        ResponseChunk &chunk = m_responseChunks[nextIndex];
        if (chunk.state == ResponseChunk::Waiting || chunk.state == ResponseChunk::Failed ||
            (chunk.state == ResponseChunk::Working && !chunk.reply))
            break;
        ++m_responseQueued;
        chunk.queued = true;
        m_player->addTrack(chunk.trackId, 0);
        if (!chunk.pcm.isEmpty())
            m_player->appendAudio(chunk.trackId, chunk.pcm);
        if (chunk.state == ResponseChunk::Complete)
            m_player->finishTrack(chunk.trackId);
    }
}

void Narration::fetchResponse(int index) {
    if (index < 0 || index >= m_responseChunks.size() || !m_ai)
        return;
    ResponseChunk &chunk = m_responseChunks[index];
    Ai::SpeechRequest request;
    request.feature = QStringLiteral("companion speech");
    request.text = Speech::speakable(chunk.text);
    request.voice = chunk.voice;
    request.economy = m_economy;
    request.stream = true;
    for (const QString &sentence : std::as_const(chunk.sentences))
        request.sentences.append(Speech::speakable(sentence));

    Ai::SpeechSynthesizer *synth = m_ai->speechFor(request.voice);
    if (!synth) {
        chunk.state = ResponseChunk::Failed;
        failResponse(tr("Choose or download a voice first (P) to listen to Companion responses."));
        return;
    }

    const bool local = AiServices::isOnThisComputer(request.voice);
    const double cost = local ? 0.0 : m_ai->listeningCost(qMax(0.05, chunk.text.size() / charsPerMinute), m_economy, false);
    if (!local && !m_ai->fitsBudget(cost)) {
        chunk.state = ResponseChunk::Failed;
        failResponse(m_ai->describe(int(Ai::Error::Budget)));
        return;
    }

    Ai::Reply *reply = synth->synthesize(request);
    m_ai->track(reply);
    chunk.reply = reply;
    chunk.state = ResponseChunk::Working;
    const quint64 session = m_responseSession;
    const int trackId = chunk.trackId;

    connect(reply, &Ai::Reply::audioReceived, this, [this, session, trackId, reply](const QByteArray &pcm) {
        if (!m_responseActive || session != m_responseSession)
            return;
        const int i = indexOfResponseChunk(trackId);
        if (i < 0 || m_responseChunks[i].reply != reply)
            return;
        ResponseChunk &c = m_responseChunks[i];
        c.pcm += pcm;
        if (c.queued)
            m_player->appendAudio(c.trackId, pcm);
        else
            queueResponse();
        if (!pcm.isEmpty() && m_responseBuffering)
            setResponseBuffering(false);
    });

    connect(reply, &Ai::Reply::finished, this, [this, session, trackId, reply] {
        reply->deleteLater();
        if (!m_responseActive || session != m_responseSession)
            return;
        const int i = indexOfResponseChunk(trackId);
        if (i < 0 || m_responseChunks[i].reply != reply)
            return;
        ResponseChunk &c = m_responseChunks[i];
        c.reply = nullptr;
        if (reply->error()) {
            const Ai::Error err = reply->error();
            if (err.kind == Ai::Error::Cancelled)
                return;
            // Seamless cloud-to-local fallback if an installed local voice is ready.
            const bool isCloud = !AiServices::isOnThisComputer(c.voice);
            if (isCloud && !c.queued &&
                (err.kind == Ai::Error::Offline || err.kind == Ai::Error::Limited || err.kind == Ai::Error::Budget ||
                 err.kind == Ai::Error::NoKey || err.kind == Ai::Error::BadKey)) {
                const auto [fbId, fbName] = bestLocalVoice();
                Q_UNUSED(fbName);
                if (!fbId.isEmpty() && fbId != c.voice && m_ai->canSpeakWith(fbId)) {
                    m_responseVoice = fbId;
                    for (int k = i; k < m_responseChunks.size(); ++k) {
                        if (m_responseChunks[k].queued)
                            continue;
                        if (m_responseChunks[k].reply) {
                            QPointer<Ai::Reply> obsolete = m_responseChunks[k].reply;
                            m_responseChunks[k].reply = nullptr;
                            obsolete->abort();
                        }
                        m_responseChunks[k].state = ResponseChunk::Waiting;
                        m_responseChunks[k].pcm = QByteArray();
                        m_responseChunks[k].key.clear();
                        m_responseChunks[k].voice = fbId;
                    }
                    pumpResponse();
                    return;
                }
            }
            c.state = ResponseChunk::Failed;
            if (c.queued)
                m_player->finishTrack(c.trackId);
            const QString msg = m_ai->describe(int(err.kind), err.message);
            if (i == 0 || m_responseHeadless || m_player->drainedOut()) {
                failResponse(msg);
                return;
            }
            setProblem(msg);
            queueResponse();
            return;
        }
        c.state = ResponseChunk::Complete;
        if (c.queued)
            m_player->finishTrack(c.trackId);
        else
            queueResponse();
        if (!c.pcm.isEmpty() && m_responseBuffering)
            setResponseBuffering(false);
        if (m_cache && !c.pcm.isEmpty() && !c.key.isEmpty() && reply->sampleRate() > 0)
            m_cache->store(c.key, c.pcm, reply->sampleRate());
        // Guard against an empty response yielding zero audio samples across all chunks.
        bool allDone = true;
        bool anyAudio = false;
        for (const ResponseChunk &item : std::as_const(m_responseChunks)) {
            if (item.state == ResponseChunk::Waiting || item.state == ResponseChunk::Working)
                allDone = false;
            if (!item.pcm.isEmpty())
                anyAudio = true;
        }
        if (allDone && !anyAudio && m_player->drainedOut()) {
            stopResponse();
            return;
        }
        pumpResponse();
    });
}

void Narration::decodeResponse(int index, const QString &file) {
    if (index < 0 || index >= m_responseChunks.size() || !m_cache)
        return;
    ResponseChunk &chunk = m_responseChunks[index];
    chunk.state = ResponseChunk::Working;
    const QString key = chunk.key;
    const int trackId = chunk.trackId;
    const quint64 session = m_responseSession;
    m_cache->decode(file, sampleRate, this, [this, session, trackId, key](const QByteArray &pcm, bool ok) {
        if (!m_responseActive || session != m_responseSession)
            return;
        const int i = indexOfResponseChunk(trackId);
        if (i < 0 || m_responseChunks[i].state != ResponseChunk::Working || m_responseChunks[i].reply)
            return;
        ResponseChunk &c = m_responseChunks[i];
        if (!ok) {
            m_cache->remove(key);
            c.state = ResponseChunk::Waiting;
            c.queued = false;
            c.key.clear();
            pumpResponse();
            return;
        }
        c.pcm = pcm;
        c.state = ResponseChunk::Complete;
        if (c.queued) {
            m_player->appendAudio(c.trackId, c.pcm);
            m_player->finishTrack(c.trackId);
        } else {
            queueResponse();
        }
        if (!c.pcm.isEmpty() && m_responseBuffering)
            setResponseBuffering(false);
        pumpResponse();
    });
}
