#include "formats/audiopcm.h"

#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace AudioPcm {

namespace {

quint16 le16(const char *p) {
    return quint16(quint8(p[0])) | quint16(quint8(p[1])) << 8;
}

quint32 le32(const char *p) {
    return quint32(quint8(p[0])) | quint32(quint8(p[1])) << 8 | quint32(quint8(p[2])) << 16 |
           quint32(quint8(p[3])) << 24;
}

}  // namespace

bool looksLikeNonPcmContainerOrText(const QByteArray &bytes) {
    if (bytes.isEmpty())
        return true;
    const QByteArray trimmed = bytes.left(64).trimmed();
    if (trimmed.isEmpty())
        return false;
    const char first = trimmed[0];
    if (first == '<' || first == '{' || first == '[')
        return true;
    if (bytes.startsWith("RIFF") || bytes.startsWith("ID3") || bytes.startsWith("OggS") || bytes.startsWith("fLaC") ||
        bytes.startsWith("HTTP/") || bytes.startsWith("\x89PNG") || bytes.startsWith("\xFF\xD8\xFF") ||
        bytes.startsWith("%PDF-")) {
        return true;
    }
    // Raw MPEG / MP3 frame sync header (11 set bits: 0xFF 0xE0..0xFF, valid layer != 0).
    if (bytes.size() >= 2 && quint8(bytes[0]) == 0xFF && (quint8(bytes[1]) & 0xE0) == 0xE0 &&
        (quint8(bytes[1]) & 0x06) != 0) {
        return true;
    }
    return false;
}

QByteArray pcmFromWav(const QByteArray &wav, int *sampleRate, bool strict) {
    if (wav.size() < 12 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE") {
        if (strict || looksLikeNonPcmContainerOrText(wav))
            return {};
        return wav;
    }
    qint64 at = 12;
    const qint64 total = wav.size();
    bool hasFmt = false;
    int fmtChannels = 1;
    int fmtRate = 0;
    bool hasData = false;
    qint64 dataOffset = 0;
    qint64 dataLen = 0;
    while (at + 8 <= total) {
        const QByteArray id = wav.mid(qsizetype(at), 4);
        const qint64 size = qint64(le32(wav.constData() + at + 4));
        const qint64 available = total - (at + 8);
        if (id == "fmt ") {
            if (size < 16 || available < 16)
                return {};
            const char *fmt = wav.constData() + at + 8;
            const quint16 audioFormat = le16(fmt);
            const quint16 channels = le16(fmt + 2);
            const quint32 rate = le32(fmt + 4);
            const quint16 bitsPerSample = le16(fmt + 14);
            bool isPcm = (audioFormat == 1);
            if (audioFormat == 0xFFFE && size >= 40 && available >= 40) {
                static constexpr char kPcmSubFormat[16] = {
                    '\x01', '\x00', '\x00', '\x00', '\x00', '\x00', '\x10', '\x00',
                    '\x80', '\x00', '\x00', '\xAA', '\x00', '\x38', '\x9B', '\x71'};
                isPcm = (std::memcmp(fmt + 24, kPcmSubFormat, 16) == 0);
            }
            if (!isPcm || bitsPerSample != 16 || (channels != 1 && channels != 2) || rate == 0 || rate > 384000)
                return {};
            hasFmt = true;
            fmtChannels = int(channels);
            fmtRate = int(rate);
            if (hasData)
                break;
        } else if (id == "data") {
            hasData = true;
            dataOffset = at + 8;
            dataLen = qMin(size, available);
            if (hasFmt)
                break;
        }
        const qint64 step = 8 + size + (size & 1);
        if (step <= 0 || at > total - step)
            break;
        at += step;
    }
    if (!hasData || (strict && !hasFmt))
        return {};
    if (hasFmt && sampleRate && fmtRate > 0)
        *sampleRate = fmtRate;
    const qsizetype frameBytes = (hasFmt ? fmtChannels : 1) * 2;
    const qsizetype alignedLen = (qsizetype(dataLen) / frameBytes) * frameBytes;
    if (alignedLen <= 0)
        return {};
    QByteArray pcm = wav.mid(qsizetype(dataOffset), alignedLen);
    if (hasFmt && fmtChannels == 2) {
        const qsizetype frames = pcm.size() / 4;
        QByteArray mono(frames * 2, Qt::Uninitialized);
        const char *src = pcm.constData();
        char *dst = mono.data();
        for (qsizetype i = 0; i < frames; ++i) {
            const qint16 left = qint16(le16(src + i * 4));
            const qint16 right = qint16(le16(src + i * 4 + 2));
            const qint16 avg = qint16((int(left) + int(right)) / 2);
            dst[i * 2] = char(avg & 0xff);
            dst[i * 2 + 1] = char((avg >> 8) & 0xff);
        }
        return mono;
    }
    return pcm;
}

QByteArray wavFromPcm16(const QByteArray &pcm, int sampleRate, int channels) {
    const quint16 ch = quint16(std::clamp(channels, 1, 2));
    const quint32 rate = quint32(qMax(1, sampleRate));
    const quint16 blockAlign = quint16(ch * 2);
    QByteArray out;
    out.reserve(44 + pcm.size());
    auto put32 = [&out](quint32 v) {
        const quint32 le = qToLittleEndian(v);
        out.append(reinterpret_cast<const char *>(&le), 4);
    };
    auto put16 = [&out](quint16 v) {
        const quint16 le = qToLittleEndian(v);
        out.append(reinterpret_cast<const char *>(&le), 2);
    };
    out.append("RIFF");
    put32(quint32(36 + pcm.size()));
    out.append("WAVEfmt ");
    put32(16);
    put16(1);
    put16(ch);
    put32(rate);
    put32(rate * quint32(blockAlign));
    put16(blockAlign);
    put16(16);
    out.append("data");
    put32(quint32(pcm.size()));
    out.append(pcm);
    return out;
}

QVector<float> pcm16ToFloat(const QByteArray &pcm) {
    const qsizetype count = pcm.size() / 2;
    QVector<float> out(count);
    const char *src = pcm.constData();
    for (qsizetype i = 0; i < count; ++i) {
        const qint16 sample = qint16(le16(src + i * 2));
        out[i] = float(sample) / 32768.0f;
    }
    return out;
}

QByteArray floatToPcm16(const QVector<float> &samples) {
    QByteArray out(samples.size() * 2, Qt::Uninitialized);
    auto *to = reinterpret_cast<qint16 *>(out.data());
    for (qsizetype n = 0; n < samples.size(); ++n)
        to[n] = qToLittleEndian(qint16(std::clamp(std::lround(samples[n] * 32767.0f), -32768L, 32767L)));
    return out;
}

QVector<float> resample(const QVector<float> &samples, int fromRate, int toRate) {
    if (fromRate == toRate || fromRate <= 0 || toRate <= 0 || samples.isEmpty())
        return samples;
    const qsizetype count = qsizetype(double(samples.size()) * toRate / fromRate);
    if (count <= 0)
        return {};
    QVector<float> out(count);
    const double step = double(fromRate) / double(toRate);
    auto at = [&samples](qsizetype i) { return samples[qBound<qsizetype>(0, i, samples.size() - 1)]; };

    if (fromRate > toRate) {
        // Downsampling: Blackman-windowed sinc low-pass anti-aliasing filter
        // (cutoff = 0.45 * toRate / fromRate cycles per input sample, 32 taps at 2:1)
        // evaluated with exact double-precision angle-addition recurrences and
        // unity DC gain normalization.
        constexpr double kPi = 3.14159265358979323846;
        const int halfWidth = std::clamp(int(std::ceil(8.0 * step)), 8, 32);
        const double winRadius = double(halfWidth);
        const double cutoff = 0.45 * double(toRate) / double(fromRate);
        const double omega = 2.0 * kPi * cutoff;
        const double cosOmega = std::cos(omega);
        const double sinOmega = std::sin(omega);
        const double winStep = kPi / winRadius;
        const double cosWinStep = std::cos(winStep);
        const double sinWinStep = std::sin(winStep);

        for (qsizetype n = 0; n < count; ++n) {
            const double position = n * step;
            const qsizetype center = qsizetype(std::floor(position));
            const qsizetype kStart = center - halfWidth + 1;
            const qsizetype kEnd = center + halfWidth;

            const double d0 = double(kStart) - position;
            double alpha = omega * d0;
            double sinA = std::sin(alpha);
            double cosA = std::cos(alpha);
            const double beta0 = winStep * d0;
            double sinB = std::sin(beta0);
            double cosB = std::cos(beta0);

            double sum = 0.0;
            double weightSum = 0.0;
            for (qsizetype k = kStart; k <= kEnd; ++k) {
                const double sinc = (std::abs(alpha) < 1e-7) ? 1.0 : (sinA / alpha);
                const double cos2B = 2.0 * cosB * cosB - 1.0;
                const double win = 0.42 + 0.50 * cosB + 0.08 * cos2B;
                const double w = sinc * win;
                sum += w * double(at(k));
                weightSum += w;

                alpha += omega;
                const double nextSinA = sinA * cosOmega + cosA * sinOmega;
                const double nextCosA = cosA * cosOmega - sinA * sinOmega;
                sinA = nextSinA;
                cosA = nextCosA;

                const double nextSinB = sinB * cosWinStep + cosB * sinWinStep;
                const double nextCosB = cosB * cosWinStep - sinB * sinWinStep;
                sinB = nextSinB;
                cosB = nextCosB;
            }
            out[n] = (weightSum > 0.0) ? float(sum / weightSum) : 0.0f;
        }
        return out;
    }

    for (qsizetype n = 0; n < count; ++n) {
        const double position = n * step;
        const qsizetype i = qsizetype(position);
        const float t = float(position - double(i));
        // Catmull-Rom through the four samples around the point.
        const float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        out[n] = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
    }
    return out;
}

QByteArray resamplePcm16(const QByteArray &pcm, int fromRate, int toRate) {
    if (fromRate == toRate || fromRate <= 0 || toRate <= 0 || pcm.isEmpty())
        return pcm;
    return floatToPcm16(resample(pcm16ToFloat(pcm), fromRate, toRate));
}

}  // namespace AudioPcm
