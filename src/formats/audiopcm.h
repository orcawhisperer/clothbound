#pragma once

#include <QByteArray>
#include <QVector>

// Pure leaf PCM / WAV audio utilities (zero QtMultimedia, AI, or Audio deps).
// Provides strict RIFF/WAVE validation, stereo-to-mono downmixing, PCM16/float
// conversion, and Blackman-windowed sinc anti-aliased resampling.
namespace AudioPcm {

// Rejects obvious non-PCM containers or HTTP/JSON/HTML error bodies when
// raw PCM fallback is permitted.
bool looksLikeNonPcmContainerOrText(const QByteArray &bytes);

// Extracts 16-bit little-endian mono PCM samples from a WAV payload (downmixing
// stereo to mono). When `strict` is true, requires a valid RIFF/WAVE header,
// 16-bit PCM format (audioFormat == 1 or WAVE_FORMAT_EXTENSIBLE PCM subformat),
// 1-2 channels, valid sample rate (1..384000 Hz), and a data chunk.
QByteArray pcmFromWav(const QByteArray &wav, int *sampleRate = nullptr, bool strict = false);

// Wraps 16-bit little-endian PCM bytes in a canonical 44-byte RIFF/WAVE header.
QByteArray wavFromPcm16(const QByteArray &pcm, int sampleRate, int channels = 1);

// Decodes 16-bit little-endian signed PCM bytes into normalized [-1, 1] floats.
QVector<float> pcm16ToFloat(const QByteArray &pcm);

// Encodes normalized [-1, 1] floats into 16-bit little-endian signed PCM bytes.
QByteArray floatToPcm16(const QVector<float> &samples);

// Resamples mono float audio between arbitrary positive sample rates:
// Blackman-windowed sinc low-pass anti-aliasing filter when downsampling
// (fromRate > toRate), and Catmull-Rom cubic interpolation when upsampling.
QVector<float> resample(const QVector<float> &samples, int fromRate, int toRate);

// Convenience pipeline: PCM16 -> float -> resample -> PCM16.
QByteArray resamplePcm16(const QByteArray &pcm, int fromRate, int toRate);

}  // namespace AudioPcm
