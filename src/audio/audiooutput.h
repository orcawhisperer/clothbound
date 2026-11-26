#pragma once

#include <QtGlobal>
#include <functional>
#include <memory>

// Abstract PCM audio output seam for SpeechPlayer.
// Linux implements this on top of QAudioSink (guarded under !CLOTHBOUND_NO_MULTIMEDIA);
// macOS injects a CoreAudio / AVAudioEngine backend without linking Qt6::Multimedia.
//
// All byte counts (write, bytesFree, bufferSize, bufferedBytes) are expressed in
// the source stream's mono 16-bit signed little-endian PCM format at the opened
// `sampleRate` so SpeechPlayer's position tracking stays backend-agnostic.
class AudioOutputBackend {
public:
    enum class State {
        Active,
        Suspended,
        Stopped,
        Idle,
    };

    enum class Error {
        NoError,
        OpenError,
        IoError,
        UnderrunError,
        FatalError,
    };

    using StateCallback = std::function<void(State, Error)>;
    using DeviceChangedCallback = std::function<void()>;

    virtual ~AudioOutputBackend() = default;

    // Opens the default audio output device for `sampleRate` Hz, `channels` Int16 PCM.
    virtual bool open(int sampleRate, int channels = 1) = 0;
    // Writes up to `len` bytes of mono Int16 PCM; returns bytes accepted (>= 0) or < 0 on error.
    virtual qint64 write(const char *data, qint64 len) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    // Flushes buffered audio and restarts the stream; returns true if ready for writes.
    virtual bool reset() = 0;
    // Stops playback and releases the underlying device handle.
    virtual void stop() = 0;

    virtual qint64 bytesFree() const = 0;
    virtual qint64 bufferSize() const = 0;
    virtual qint64 bufferedBytes() const { return qMax<qint64>(0, bufferSize() - bytesFree()); }
    virtual qint64 pendingBytes() const { return 0; }
    virtual bool flushPending() { return true; }

    virtual State state() const = 0;
    virtual Error error() const { return Error::NoError; }
    virtual void setVolume(double volume) = 0;
    virtual bool isOpen() const = 0;

    virtual void setStateCallback(StateCallback cb) { Q_UNUSED(cb); }
    virtual void setDeviceChangedCallback(DeviceChangedCallback cb) { Q_UNUSED(cb); }
};

using AudioOutputBackendFactory = std::function<std::unique_ptr<AudioOutputBackend>()>;
