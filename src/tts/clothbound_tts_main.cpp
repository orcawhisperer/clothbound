// Out-of-Process Speech Helper (`clothbound-tts`)
// ===============================================
// Pure C++20 standalone executable (ZERO Qt!) wrapping `sherpa-onnx` +
// `espeak-ng` via dynamic `dlopen`/`dlsym` over a length-prefixed binary
// stdin/stdout pipe protocol (`src/tts/tts_protocol.h`).
//
// Architectural guarantees:
// - Keeps Clothbound's main address space completely free of GPL-3.0 espeak-ng
//   and ONNX Runtime (zero Hardened Runtime exceptions on macOS).
// - Full crash isolation: an ONNX/espeak crash kills only this helper.
// - 0 MB idle ONNX RAM on exit: exits cleanly on SHUTDOWN or idle teardown.
// - Zero orphan risk: exits immediately with status 0 on stdin EOF if the
//   parent app crashes or is force-quit.
// - Protocol stream safety: duplicates STDOUT_FILENO to a private fd and
//   redirects fd 1 -> STDERR_FILENO so stray library stdout prints can never
//   corrupt binary IPC frames.

#include "audio/sherpaonnx.h"
#include "tts/tts_protocol.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifndef CLOTHBOUND_VERSION
#define CLOTHBOUND_VERSION "0.5.2"
#endif

namespace {

using namespace ClothboundTts;

#if defined(__APPLE__)
constexpr const char *kSherpaLibName = "libsherpa-onnx-c-api.dylib";
constexpr const char *kOnnxRuntimeLibName = "libonnxruntime.dylib";
#else
constexpr const char *kSherpaLibName = "libsherpa-onnx-c-api.so";
constexpr const char *kOnnxRuntimeLibName = "libonnxruntime.so";
#endif

std::string executableDirectory() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size > 0) {
        std::string buf(size, '\0');
        if (_NSGetExecutablePath(buf.data(), &size) == 0) {
            std::error_code ec;
            auto p = std::filesystem::weakly_canonical(std::filesystem::path(buf.c_str()), ec);
            if (!ec)
                return p.parent_path().string();
        }
    }
#elif defined(__linux__)
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return p.parent_path().string();
#endif
    return {};
}

// ---------------------------------------------------------------------------
// Reliable POSIX pipe I/O
// ---------------------------------------------------------------------------

enum class ReadResult {
    Ok,
    Eof,
    Error,
};

ReadResult readExact(int fd, uint8_t *dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, dst + got, n - got);
        if (r == 0)
            return got == 0 ? ReadResult::Eof : ReadResult::Error;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return ReadResult::Error;
        }
        got += static_cast<size_t>(r);
    }
    return ReadResult::Ok;
}

bool writeExact(int fd, const uint8_t *src, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        const ssize_t w = ::write(fd, src + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (w == 0)
            return false;
        sent += static_cast<size_t>(w);
    }
    return true;
}

class FrameWriter {
public:
    explicit FrameWriter(int fd) : m_fd(fd) {}

    bool send(FrameType type, const uint8_t *payload, size_t len) {
        const auto wire = encodeFrame(type, payload, len);
        if (wire.empty())
            return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        return writeExact(m_fd, wire.data(), wire.size());
    }

    bool sendJson(FrameType type, std::string_view json) {
        return send(type, reinterpret_cast<const uint8_t *>(json.data()), json.size());
    }

    bool sendError(uint64_t requestId, std::string_view code, std::string_view message) {
        const std::string json = "{\"request_id\":" + std::to_string(requestId) +
                                 ",\"code\":" + Json::escapeString(code) +
                                 ",\"message\":" + Json::escapeString(message) + "}";
        return sendJson(FrameType::Error, json);
    }

private:
    int m_fd = -1;
    std::mutex m_mutex;
};

// ---------------------------------------------------------------------------
// Minimal pure-C++20 RIFF/WAVE PCM16 loader (for Pocket reference voices)
// ---------------------------------------------------------------------------

bool loadWavFloatMono(const std::string &path, std::vector<float> &samplesOut, int &sampleRateOut) {
    samplesOut.clear();
    sampleRateOut = 24000;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
        std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        return false;

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bitsPerSample = 0;
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const uint32_t chunkSize = readLe32(data.data() + pos + 4);
        const size_t bodyPos = pos + 8;
        if (bodyPos + chunkSize > data.size())
            break;
        if (std::memcmp(data.data() + pos, "fmt ", 4) == 0 && chunkSize >= 16) {
            audioFormat = static_cast<uint16_t>(data[bodyPos] | (data[bodyPos + 1] << 8));
            channels = static_cast<uint16_t>(data[bodyPos + 2] | (data[bodyPos + 3] << 8));
            rate = readLe32(data.data() + bodyPos + 4);
            bitsPerSample = static_cast<uint16_t>(data[bodyPos + 14] | (data[bodyPos + 15] << 8));
        } else if (std::memcmp(data.data() + pos, "data", 4) == 0) {
            if (audioFormat != 1 || bitsPerSample != 16 || (channels != 1 && channels != 2) || rate == 0)
                return false;
            const size_t frameBytes = static_cast<size_t>(channels) * 2u;
            const size_t frames = chunkSize / frameBytes;
            samplesOut.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                const size_t off = bodyPos + i * frameBytes;
                const int16_t s0 = static_cast<int16_t>(data[off] | (data[off + 1] << 8));
                if (channels == 1) {
                    samplesOut[i] = static_cast<float>(s0) / 32768.0f;
                } else {
                    const int16_t s1 = static_cast<int16_t>(data[off + 2] | (data[off + 3] << 8));
                    samplesOut[i] = (static_cast<float>(s0) + static_cast<float>(s1)) / 65536.0f;
                }
            }
            sampleRateOut = static_cast<int>(rate);
            return !samplesOut.empty();
        }
        pos = bodyPos + chunkSize + (chunkSize & 1u);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Dynamic sherpa-onnx C API wrapper (zero Qt)
// ---------------------------------------------------------------------------

struct LoadModelParams {
    std::string kind;
    std::string model;
    std::string tokens;
    std::string data_dir;
    std::string voices;
    std::string lexicon;
    std::string dict_dir;
    std::string lang;
    std::string dir;
    std::string reference_wav;
    std::string lib_path;
    std::string provider = "cpu";
    int32_t threads = 0;
};

class SherpaDynamicRuntime {
public:
    explicit SherpaDynamicRuntime(std::string defaultLibDir, std::string explicitLibPath)
        : m_defaultLibDir(std::move(defaultLibDir)), m_explicitLibPath(std::move(explicitLibPath)) {}

    ~SherpaDynamicRuntime() {
        unloadModel();
        if (m_libHandle) {
            ::dlclose(m_libHandle);
            m_libHandle = nullptr;
        }
        if (m_ortHandle) {
            ::dlclose(m_ortHandle);
            m_ortHandle = nullptr;
        }
    }

    bool isModelLoaded() const noexcept { return m_tts != nullptr; }
    int32_t sampleRate() const noexcept { return m_sampleRate; }
    int32_t numSpeakers() const noexcept { return m_numSpeakers; }
    const std::string &activeKind() const noexcept { return m_kind; }

    void unloadModel() {
        if (m_tts && m_destroy) {
            m_destroy(m_tts);
            m_tts = nullptr;
        }
        m_sampleRate = 0;
        m_numSpeakers = 0;
        m_kind.clear();
        m_reference.clear();
        m_referenceRate = 24000;
    }

    bool loadModel(const LoadModelParams &params, std::string &errOut) {
        if (!ensureLibraryLoaded(params.lib_path, errOut))
            return false;

        unloadModel();

        const std::string kind = params.kind.empty() ? std::string("piper") : params.kind;
        if (kind == "pocket" && !params.reference_wav.empty()) {
            if (!loadWavFloatMono(params.reference_wav, m_reference, m_referenceRate)) {
                errOut = "Failed to load Pocket reference WAV: " + params.reference_wav;
                return false;
            }
        }

        auto joinDir = [&params](const char *name) -> std::string {
            if (params.dir.empty())
                return name;
            return params.dir + "/" + name;
        };
        const std::string lmFlow = joinDir("lm_flow.int8.onnx");
        const std::string lmMain = joinDir("lm_main.int8.onnx");
        const std::string encoder = joinDir("encoder.onnx");
        const std::string decoder = joinDir("decoder.int8.onnx");
        const std::string conditioner = joinDir("text_conditioner.onnx");
        const std::string vocab = joinDir("vocab.json");
        const std::string scores = joinDir("token_scores.json");

        SherpaOnnx::OfflineTtsConfig config;
        std::memset(&config, 0, sizeof(config));

        if (kind == "pocket") {
            config.model.pocket.lm_flow = lmFlow.c_str();
            config.model.pocket.lm_main = lmMain.c_str();
            config.model.pocket.encoder = encoder.c_str();
            config.model.pocket.decoder = decoder.c_str();
            config.model.pocket.text_conditioner = conditioner.c_str();
            config.model.pocket.vocab_json = vocab.c_str();
            config.model.pocket.token_scores_json = scores.c_str();
            config.model.pocket.voice_embedding_cache_capacity = 8;
        } else if (kind == "kokoro") {
            config.model.kokoro.model = params.model.c_str();
            config.model.kokoro.voices = params.voices.c_str();
            config.model.kokoro.tokens = params.tokens.c_str();
            config.model.kokoro.data_dir = params.data_dir.c_str();
            config.model.kokoro.length_scale = 1.0f;
            config.model.kokoro.dict_dir = params.dict_dir.c_str();
            config.model.kokoro.lexicon = params.lexicon.c_str();
            config.model.kokoro.lang = params.lang.c_str();
        } else if (kind == "piper" || kind == "vits") {
            config.model.vits.model = params.model.c_str();
            config.model.vits.tokens = params.tokens.c_str();
            config.model.vits.data_dir = params.data_dir.c_str();
            config.model.vits.lexicon = params.lexicon.c_str();
            config.model.vits.dict_dir = params.dict_dir.c_str();
            config.model.vits.noise_scale = 0.667f;
            config.model.vits.noise_scale_w = 0.8f;
            config.model.vits.length_scale = 1.0f;
        } else {
            errOut = "Unsupported TTS model kind: " + kind;
            return false;
        }

        int32_t threads = params.threads;
        if (threads <= 0) {
            const unsigned hw = std::thread::hardware_concurrency();
            threads = static_cast<int32_t>(hw > 2 ? std::min<unsigned>(4, hw / 2) : 1);
        }
        config.model.num_threads = threads;
        config.model.debug = 0;
        config.model.provider = params.provider.empty() ? "cpu" : params.provider.c_str();
        config.max_num_sentences = 1;
        config.silence_scale = 0.2f;

        m_tts = m_create(&config);
        if (!m_tts) {
            errOut = "SherpaOnnxCreateOfflineTts returned null for model kind '" + kind + "'.";
            return false;
        }
        m_sampleRate = m_rateOf(m_tts);
        m_numSpeakers = m_numSpeakersOf ? m_numSpeakersOf(m_tts) : 1;
        if (m_numSpeakers <= 0)
            m_numSpeakers = 1;
        m_kind = kind;
        return true;
    }

    bool synthesize(uint64_t requestId,
                    const std::string &text,
                    int32_t speakerId,
                    float speed,
                    PcmSampleFormat format,
                    const std::atomic<uint64_t> &cancelRequestId,
                    const std::atomic_bool &shuttingDown,
                    FrameWriter &writer,
                    uint64_t &totalSamplesOut,
                    bool &cancelledOut,
                    std::string &errOut) {
        totalSamplesOut = 0;
        cancelledOut = false;
        if (!m_tts) {
            errOut = "No TTS model is loaded.";
            return false;
        }
        if (text.empty())
            return true;

        SherpaOnnx::GenerationConfig config;
        std::memset(&config, 0, sizeof(config));
        config.silence_scale = 0.2f;
        config.speed = (std::isfinite(speed) && speed > 0.1f && speed <= 4.0f) ? speed : 1.0f;
        config.sid = speakerId;
        if (!m_reference.empty()) {
            config.reference_audio = m_reference.data();
            config.reference_audio_len = static_cast<int32_t>(m_reference.size());
            config.reference_sample_rate = m_referenceRate;
        }

        struct StreamState {
            uint64_t requestId = 0;
            uint32_t sampleRate = 24000;
            PcmSampleFormat format = PcmSampleFormat::Float32;
            const std::atomic<uint64_t> *cancelId = nullptr;
            const std::atomic_bool *shuttingDown = nullptr;
            FrameWriter *writer = nullptr;
            uint64_t streamedSamples = 0;
            bool cancelled = false;
            bool writeFailed = false;
        };

        StreamState state;
        state.requestId = requestId;
        state.sampleRate = static_cast<uint32_t>(m_sampleRate > 0 ? m_sampleRate : 24000);
        state.format = format;
        state.cancelId = &cancelRequestId;
        state.shuttingDown = &shuttingDown;
        state.writer = &writer;

        auto sendSamples = [&](const float *samples, int32_t n, uint32_t rate) -> bool {
            if (!samples || n <= 0)
                return true;
            constexpr size_t kChunkMaxSamples = 16384u;
            size_t offset = 0;
            const size_t total = static_cast<size_t>(n);
            while (offset < total) {
                const size_t count = std::min(kChunkMaxSamples, total - offset);
                std::vector<uint8_t> payload;
                if (format == PcmSampleFormat::Int16) {
                    const auto pcm16 = encodePcm16FromFloat(samples + offset, count);
                    payload = encodePcmChunkPayload(requestId, rate, static_cast<uint32_t>(count),
                                                    PcmSampleFormat::Int16, pcm16.data(), pcm16.size());
                } else {
                    payload = encodePcmChunkPayload(requestId, rate, static_cast<uint32_t>(count),
                                                    PcmSampleFormat::Float32, samples + offset,
                                                    count * sizeof(float));
                }
                if (payload.empty() || !writer.send(FrameType::PcmChunk, payload.data(), payload.size()))
                    return false;
                offset += count;
            }
            return true;
        };

        SherpaOnnx::ProgressCallback cb = [](const float *samples, int32_t n, float, void *arg) -> int32_t {
            auto *st = static_cast<StreamState *>(arg);
            if (st->shuttingDown->load(std::memory_order_acquire) ||
                st->cancelId->load(std::memory_order_acquire) == st->requestId) {
                st->cancelled = true;
                return 0;
            }
            if (samples && n > 0) {
                constexpr size_t kChunkMaxSamples = 16384u;
                size_t offset = 0;
                const size_t total = static_cast<size_t>(n);
                while (offset < total) {
                    const size_t count = std::min(kChunkMaxSamples, total - offset);
                    std::vector<uint8_t> payload;
                    if (st->format == PcmSampleFormat::Int16) {
                        const auto pcm16 = encodePcm16FromFloat(samples + offset, count);
                        payload = encodePcmChunkPayload(st->requestId, st->sampleRate,
                                                        static_cast<uint32_t>(count), PcmSampleFormat::Int16,
                                                        pcm16.data(), pcm16.size());
                    } else {
                        payload = encodePcmChunkPayload(st->requestId, st->sampleRate,
                                                        static_cast<uint32_t>(count), PcmSampleFormat::Float32,
                                                        samples + offset, count * sizeof(float));
                    }
                    if (payload.empty() || !st->writer->send(FrameType::PcmChunk, payload.data(), payload.size())) {
                        st->writeFailed = true;
                        return 0;
                    }
                    offset += count;
                }
                st->streamedSamples += static_cast<uint64_t>(n);
            }
            return 1;
        };

        const SherpaOnnx::GeneratedAudio *audio = m_generate(m_tts, text.c_str(), &config, cb, &state);
        if (audio) {
            if (audio->sample_rate > 0)
                m_sampleRate = audio->sample_rate;
            if (!state.cancelled && !state.writeFailed && state.streamedSamples == 0 && audio->samples && audio->n > 0) {
                const uint32_t rate = static_cast<uint32_t>(audio->sample_rate > 0 ? audio->sample_rate : m_sampleRate);
                if (!sendSamples(audio->samples, audio->n, rate))
                    state.writeFailed = true;
                else
                    state.streamedSamples = static_cast<uint64_t>(audio->n);
            }
            m_free(audio);
        }

        if (state.writeFailed) {
            errOut = "Failed writing PCM output to pipe.";
            return false;
        }
        cancelledOut = state.cancelled;
        totalSamplesOut = state.streamedSamples;
        return true;
    }

private:
    std::vector<std::string> candidatePaths(const std::string &overridePath) const {
        std::vector<std::string> candidates;
        auto pushDir = [&candidates](const std::string &dir) {
            if (!dir.empty())
                candidates.push_back((std::filesystem::path(dir) / kSherpaLibName).string());
        };
        if (!overridePath.empty())
            candidates.push_back(overridePath);
        if (!m_explicitLibPath.empty())
            candidates.push_back(m_explicitLibPath);
        pushDir(m_defaultLibDir);
        if (const char *envDir = std::getenv("CLOTHBOUND_TTS_LIB_DIR"))
            pushDir(envDir);
        const std::string exeDir = executableDirectory();
        if (!exeDir.empty()) {
            pushDir(exeDir);
            pushDir((std::filesystem::path(exeDir) / ".." / "Frameworks").lexically_normal().string());
            pushDir((std::filesystem::path(exeDir) / ".." / "lib").lexically_normal().string());
        }
        candidates.push_back(kSherpaLibName);
        return candidates;
    }

    void preloadSiblingOnnxRuntime(const std::string &sherpaPath) {
        if (m_ortHandle)
            return;
        const std::filesystem::path p(sherpaPath);
        if (!p.has_parent_path())
            return;
        const std::filesystem::path ort = p.parent_path() / kOnnxRuntimeLibName;
        std::error_code ec;
        if (std::filesystem::exists(ort, ec))
            m_ortHandle = ::dlopen(ort.c_str(), RTLD_NOW | RTLD_GLOBAL);
    }

    bool ensureLibraryLoaded(const std::string &overridePath, std::string &errOut) {
        if (m_libHandle)
            return true;

        std::string lastErr;
        for (const std::string &candidate : candidatePaths(overridePath)) {
            preloadSiblingOnnxRuntime(candidate);
            void *handle = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                const char *dl = ::dlerror();
                if (dl)
                    lastErr = dl;
                continue;
            }

            auto versionFn = reinterpret_cast<SherpaOnnx::VersionString>(::dlsym(handle, "SherpaOnnxGetVersionStr"));
            if (!versionFn || std::string_view(versionFn()) != SherpaOnnx::version) {
                const std::string found = versionFn ? std::string(versionFn()) : std::string("?");
                ::dlclose(handle);
                lastErr = "Incompatible sherpa-onnx version '" + found + "' (expected " + SherpaOnnx::version + ")";
                continue;
            }

            auto createFn = reinterpret_cast<SherpaOnnx::CreateOfflineTts>(::dlsym(handle, "SherpaOnnxCreateOfflineTts"));
            auto destroyFn =
                reinterpret_cast<SherpaOnnx::DestroyOfflineTts>(::dlsym(handle, "SherpaOnnxDestroyOfflineTts"));
            auto generateFn = reinterpret_cast<SherpaOnnx::GenerateWithConfig>(
                ::dlsym(handle, "SherpaOnnxOfflineTtsGenerateWithConfig"));
            auto freeFn = reinterpret_cast<SherpaOnnx::DestroyGeneratedAudio>(
                ::dlsym(handle, "SherpaOnnxDestroyOfflineTtsGeneratedAudio"));
            auto rateFn = reinterpret_cast<SherpaOnnx::SampleRate>(::dlsym(handle, "SherpaOnnxOfflineTtsSampleRate"));
            auto numSpeakersFn =
                reinterpret_cast<SherpaOnnx::NumSpeakers>(::dlsym(handle, "SherpaOnnxOfflineTtsNumSpeakers"));

            if (!createFn || !destroyFn || !generateFn || !freeFn || !rateFn) {
                ::dlclose(handle);
                lastErr = "sherpa-onnx C API library is missing required symbols.";
                continue;
            }

            m_libHandle = handle;
            m_create = createFn;
            m_destroy = destroyFn;
            m_generate = generateFn;
            m_free = freeFn;
            m_rateOf = rateFn;
            m_numSpeakersOf = numSpeakersFn;
            return true;
        }

        errOut = lastErr.empty() ? std::string("Unable to load ") + kSherpaLibName : lastErr;
        return false;
    }

    std::string m_defaultLibDir;
    std::string m_explicitLibPath;
    void *m_ortHandle = nullptr;
    void *m_libHandle = nullptr;
    SherpaOnnx::CreateOfflineTts m_create = nullptr;
    SherpaOnnx::DestroyOfflineTts m_destroy = nullptr;
    SherpaOnnx::GenerateWithConfig m_generate = nullptr;
    SherpaOnnx::DestroyGeneratedAudio m_free = nullptr;
    SherpaOnnx::SampleRate m_rateOf = nullptr;
    SherpaOnnx::NumSpeakers m_numSpeakersOf = nullptr;
    const SherpaOnnx::OfflineTts *m_tts = nullptr;
    int32_t m_sampleRate = 0;
    int32_t m_numSpeakers = 0;
    std::string m_kind;
    std::vector<float> m_reference;
    int m_referenceRate = 24000;
};

// ---------------------------------------------------------------------------
// Command queue executed sequentially on a dedicated worker thread so the
// main thread immediately detects CANCEL, SHUTDOWN, and stdin EOF.
// ---------------------------------------------------------------------------

struct WorkerCommand {
    FrameType type = FrameType::LoadModel;
    LoadModelParams loadParams;
    uint64_t requestId = 0;
    std::string text;
    int32_t speakerId = 0;
    float speed = 1.0f;
    PcmSampleFormat pcmFormat = PcmSampleFormat::Float32;
};

int runSelfTest() {
    // Exercise framing, JSON parsing, PCM encoding, and header validation without requiring a model.
    const std::string sampleJson = R"({"request_id":42,"text":"Hello \u2603","speaker_id":1,"speed":1.25})";
    const auto wire = encodeFrame(FrameType::Synthesize, sampleJson);
    Frame decoded;
    size_t consumed = 0;
    if (tryDecodeFrame(wire.data(), wire.size(), decoded, consumed) != DecodeStatus::Ok || consumed != wire.size()) {
        std::fprintf(stderr, "clothbound-tts self-test failed: frame round-trip mismatch\n");
        return 1;
    }
    const auto obj = Json::parseObject(
        std::string_view(reinterpret_cast<const char *>(decoded.payload.data()), decoded.payload.size()));
    if (!obj || Json::getUint(*obj, "request_id") != 42 || Json::getInt(*obj, "speaker_id") != 1) {
        std::fprintf(stderr, "clothbound-tts self-test failed: JSON parse mismatch\n");
        return 1;
    }
    const float samples[4] = {-1.0f, -0.5f, 0.5f, 1.0f};
    const auto pcm16 = encodePcm16FromFloat(samples, 4);
    const auto chunk = encodePcmChunkPayload(42, 24000, 4, PcmSampleFormat::Int16, pcm16.data(), pcm16.size());
    PcmChunkHeader hdr;
    const uint8_t *pcmPtr = nullptr;
    size_t pcmLen = 0;
    if (!decodePcmChunkPayload(chunk.data(), chunk.size(), hdr, pcmPtr, pcmLen) || hdr.request_id != 42 ||
        hdr.sample_rate != 24000 || hdr.sample_count != 4 || pcmLen != 8) {
        std::fprintf(stderr, "clothbound-tts self-test failed: PCM chunk round-trip mismatch\n");
        return 1;
    }
    std::printf("{\"ok\":true,\"protocol\":%u,\"sherpa_api_version\":\"%s\",\"pid\":%lld}\n",
                kProtocolVersion, SherpaOnnx::version, static_cast<long long>(::getpid()));
    std::fflush(stdout);
    return 0;
}

void printUsage() {
    std::fputs(
        "Usage: clothbound-tts [--lib-dir <dir>] [--lib <path>] [--self-test] [--version] [--help]\n"
        "\n"
        "Out-of-process sherpa-onnx speech synthesis helper for Clothbound.\n"
        "Speaks binary length-prefixed frames on stdin/stdout and exits cleanly on stdin EOF.\n",
        stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char *argv[]) {
    std::string libDir;
    std::string libPath;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--self-test") {
            return runSelfTest();
        } else if (arg == "--version" || arg == "-v") {
            std::printf("clothbound-tts %s (protocol %u, sherpa-onnx-api %s)\n",
                        CLOTHBOUND_VERSION, kProtocolVersion, SherpaOnnx::version);
            std::fflush(stdout);
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--lib-dir" && i + 1 < argc) {
            libDir = argv[++i];
        } else if (arg == "--lib" && i + 1 < argc) {
            libPath = argv[++i];
        } else {
            std::fprintf(stderr, "clothbound-tts: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    std::signal(SIGPIPE, SIG_IGN);

    // Move the binary IPC output pipe off STDOUT_FILENO and redirect fd 1 to
    // stderr so stray writes from ONNX Runtime / espeak-ng never corrupt frames.
    const int protocolOutFd = ::dup(STDOUT_FILENO);
    if (protocolOutFd < 0) {
        std::fprintf(stderr, "clothbound-tts: dup(stdout) failed: %s\n", std::strerror(errno));
        return 1;
    }
    ::fcntl(protocolOutFd, F_SETFD, FD_CLOEXEC);
    ::dup2(STDERR_FILENO, STDOUT_FILENO);

    FrameWriter writer(protocolOutFd);
    SherpaDynamicRuntime runtime(std::move(libDir), std::move(libPath));

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<WorkerCommand> queue;
    std::atomic_bool shuttingDown{false};
    std::atomic<uint64_t> cancelRequestId{0};
    std::atomic_bool modelLoadedFlag{false};

    std::thread worker([&]() {
        while (true) {
            WorkerCommand cmd;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [&]() { return shuttingDown.load(std::memory_order_acquire) || !queue.empty(); });
                if (shuttingDown.load(std::memory_order_acquire))
                    break;
                cmd = std::move(queue.front());
                queue.pop_front();
            }

            if (cmd.type == FrameType::LoadModel) {
                std::string err;
                if (!runtime.loadModel(cmd.loadParams, err)) {
                    modelLoadedFlag.store(false, std::memory_order_release);
                    writer.sendError(0, "load_failed", err);
                } else {
                    modelLoadedFlag.store(true, std::memory_order_release);
                    const std::string readyJson = "{\"sample_rate\":" + std::to_string(runtime.sampleRate()) +
                                                  ",\"num_speakers\":" + std::to_string(runtime.numSpeakers()) +
                                                  ",\"kind\":" + Json::escapeString(runtime.activeKind()) + "}";
                    writer.sendJson(FrameType::Ready, readyJson);
                }
            } else if (cmd.type == FrameType::Synthesize) {
                if (cancelRequestId.load(std::memory_order_acquire) == cmd.requestId) {
                    const std::string doneJson = "{\"request_id\":" + std::to_string(cmd.requestId) +
                                                 ",\"cancelled\":true,\"total_samples\":0}";
                    writer.sendJson(FrameType::Done, doneJson);
                    continue;
                }
                uint64_t totalSamples = 0;
                bool cancelled = false;
                std::string err;
                if (!runtime.synthesize(cmd.requestId, cmd.text, cmd.speakerId, cmd.speed, cmd.pcmFormat,
                                        cancelRequestId, shuttingDown, writer, totalSamples, cancelled, err)) {
                    writer.sendError(cmd.requestId, runtime.isModelLoaded() ? "synthesis_failed" : "model_not_loaded",
                                     err);
                } else {
                    const std::string doneJson = "{\"request_id\":" + std::to_string(cmd.requestId) +
                                                 ",\"cancelled\":" + (cancelled ? "true" : "false") +
                                                 ",\"total_samples\":" + std::to_string(totalSamples) + "}";
                    writer.sendJson(FrameType::Done, doneJson);
                }
            }
        }
        runtime.unloadModel();
    });

    int exitCode = 0;
    uint8_t hdrBytes[kFrameHeaderBytes];
    std::vector<uint8_t> payload;

    while (true) {
        const ReadResult rr = readExact(STDIN_FILENO, hdrBytes, kFrameHeaderBytes);
        if (rr == ReadResult::Eof) {
            // Parent closed stdin or crashed: exit cleanly with status 0 (zero orphan risk).
            exitCode = 0;
            break;
        }
        if (rr == ReadResult::Error) {
            writer.sendError(0, "invalid_frame", "Truncated frame header on stdin.");
            exitCode = 1;
            break;
        }

        FrameHeader hdr;
        const DecodeStatus status = parseFrameHeader(hdrBytes, kFrameHeaderBytes, hdr);
        if (status == DecodeStatus::BadMagic) {
            writer.sendError(0, "invalid_frame", "Invalid frame magic bytes.");
            exitCode = 1;
            break;
        }
        if (status == DecodeStatus::FrameTooLarge) {
            writer.sendError(0, "invalid_frame", "Frame payload exceeds 16 MiB maximum limit.");
            exitCode = 1;
            break;
        }

        payload.resize(hdr.payload_len);
        if (hdr.payload_len > 0) {
            const ReadResult pr = readExact(STDIN_FILENO, payload.data(), hdr.payload_len);
            if (pr != ReadResult::Ok) {
                writer.sendError(0, "invalid_frame", "Unexpected EOF while reading frame payload.");
                exitCode = pr == ReadResult::Eof ? 0 : 1;
                break;
            }
        }

        const auto frameType = static_cast<FrameType>(hdr.type);
        const std::string_view payloadView(reinterpret_cast<const char *>(payload.data()), payload.size());

        if (frameType == FrameType::Ping) {
            const std::string pongJson =
                "{\"protocol\":" + std::to_string(kProtocolVersion) +
                ",\"sherpa_api_version\":" + Json::escapeString(SherpaOnnx::version) +
                ",\"model_loaded\":" + (modelLoadedFlag.load(std::memory_order_acquire) ? "true" : "false") +
                ",\"pid\":" + std::to_string(static_cast<long long>(::getpid())) + "}";
            if (!writer.sendJson(FrameType::Pong, pongJson))
                break;
        } else if (frameType == FrameType::Shutdown) {
            exitCode = 0;
            break;
        } else if (frameType == FrameType::Cancel) {
            uint64_t reqId = 0;
            if (payload.size() == sizeof(uint64_t)) {
                reqId = readLe64(payload.data());
            } else {
                const auto obj = Json::parseObject(payloadView);
                if (!obj) {
                    writer.sendError(0, "invalid_json", "CANCEL payload must be JSON or 8-byte uint64.");
                    continue;
                }
                reqId = Json::getUint(*obj, "request_id", 0);
            }
            if (reqId > 0)
                cancelRequestId.store(reqId, std::memory_order_release);
        } else if (frameType == FrameType::LoadModel) {
            const auto obj = Json::parseObject(payloadView);
            if (!obj) {
                writer.sendError(0, "invalid_json", "LOAD_MODEL payload must be a JSON object.");
                continue;
            }
            WorkerCommand cmd;
            cmd.type = FrameType::LoadModel;
            cmd.loadParams.kind = Json::getString(*obj, "kind", "piper");
            cmd.loadParams.model = Json::getString(*obj, "model");
            cmd.loadParams.tokens = Json::getString(*obj, "tokens");
            cmd.loadParams.data_dir = Json::getString(*obj, "data_dir");
            cmd.loadParams.voices = Json::getString(*obj, "voices");
            cmd.loadParams.lexicon = Json::getString(*obj, "lexicon");
            cmd.loadParams.dict_dir = Json::getString(*obj, "dict_dir");
            cmd.loadParams.lang = Json::getString(*obj, "lang");
            cmd.loadParams.dir = Json::getString(*obj, "dir");
            cmd.loadParams.reference_wav = Json::getString(*obj, "reference_wav");
            cmd.loadParams.lib_path = Json::getString(*obj, "lib_path");
            cmd.loadParams.provider = Json::getString(*obj, "provider", "cpu");
            cmd.loadParams.threads = static_cast<int32_t>(Json::getInt(*obj, "threads", 0));
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                queue.push_back(std::move(cmd));
            }
            queueCv.notify_one();
        } else if (frameType == FrameType::Synthesize) {
            const auto obj = Json::parseObject(payloadView);
            if (!obj) {
                writer.sendError(0, "invalid_json", "SYNTHESIZE payload must be a JSON object.");
                continue;
            }
            WorkerCommand cmd;
            cmd.type = FrameType::Synthesize;
            cmd.requestId = Json::getUint(*obj, "request_id", 0);
            cmd.text = Json::getString(*obj, "text");
            cmd.speakerId = static_cast<int32_t>(Json::getInt(*obj, "speaker_id", 0));
            cmd.speed = static_cast<float>(Json::getDouble(*obj, "speed", 1.0));
            const std::string fmt = Json::getString(*obj, "pcm_format", "f32");
            cmd.pcmFormat = (fmt == "i16" || fmt == "int16") ? PcmSampleFormat::Int16 : PcmSampleFormat::Float32;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                queue.push_back(std::move(cmd));
            }
            queueCv.notify_one();
        } else {
            writer.sendError(0, "unknown_frame", "Unsupported frame type.");
        }
    }

    shuttingDown.store(true, std::memory_order_release);
    queueCv.notify_all();
    if (worker.joinable())
        worker.join();
    ::close(protocolOutFd);
    return exitCode;
}
