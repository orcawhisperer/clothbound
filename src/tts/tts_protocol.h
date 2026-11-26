#pragma once

// Pure C++20 binary-framed IPC protocol shared between the out-of-process
// `clothbound-tts` helper executable (ZERO Qt!) and `TtsProcessClient`.
//
// Wire framing (all integers little-endian):
//   [0..3]   uint32_t magic       = 0x43425453 ("CBTS")
//   [4..7]   uint32_t type        = FrameType
//   [8..11]  uint32_t payload_len = byte length of payload (<= kMaxFramePayloadBytes)
//   [12..]   uint8_t  payload[payload_len]
//
// Host -> Helper:
//   PING       (empty or optional JSON)
//   LOAD_MODEL (JSON: kind, model, tokens, data_dir, voices, lexicon, dict_dir,
//               lang, dir, reference_wav, lib_path, provider, threads)
//   SYNTHESIZE (JSON: request_id, text, speaker_id, speed, pcm_format)
//   CANCEL     (JSON: {"request_id": N} or raw 8-byte little-endian uint64_t)
//   SHUTDOWN   (empty)
//
// Helper -> Host:
//   PONG       (JSON: protocol, sherpa_api_version, model_loaded, pid)
//   READY      (JSON: sample_rate, num_speakers, kind)
//   PCM_CHUNK  (24-byte PcmChunkHeader + raw float32 or int16 mono PCM bytes)
//   DONE       (JSON: request_id, cancelled, total_samples)
//   ERROR      (JSON: request_id, code, message)

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ClothboundTts {

inline constexpr uint32_t kFrameMagic = 0x43425453u;  // "CBTS"
inline constexpr uint32_t kProtocolVersion = 1u;
inline constexpr size_t kFrameHeaderBytes = 12u;
inline constexpr uint32_t kMaxFramePayloadBytes = 16u * 1024u * 1024u;  // 16 MiB hard cap

enum class FrameType : uint32_t {
    // Host -> Helper
    Ping = 0x0001u,
    LoadModel = 0x0002u,
    Synthesize = 0x0003u,
    Cancel = 0x0004u,
    Shutdown = 0x0005u,

    // Helper -> Host
    Pong = 0x0101u,
    Ready = 0x0102u,
    PcmChunk = 0x0103u,
    Done = 0x0104u,
    Error = 0x0105u,
};

enum class PcmSampleFormat : uint32_t {
    Float32 = 1u,  // Little-endian IEEE-754 float32 mono [-1.0, 1.0]
    Int16 = 2u,    // Little-endian signed 16-bit integer mono
};

struct FrameHeader {
    uint32_t magic = kFrameMagic;
    uint32_t type = 0;
    uint32_t payload_len = 0;
};

struct Frame {
    FrameType type = FrameType::Ping;
    std::vector<uint8_t> payload;
};

struct PcmChunkHeader {
    uint64_t request_id = 0;
    uint32_t sample_rate = 0;
    uint32_t sample_count = 0;
    uint32_t sample_format = static_cast<uint32_t>(PcmSampleFormat::Float32);
    uint32_t flags = 0;
};

inline constexpr size_t kPcmChunkHeaderBytes = 24u;

enum class DecodeStatus {
    Ok,
    NeedMoreData,
    BadMagic,
    FrameTooLarge,
};

// ---------------------------------------------------------------------------
// Little-endian wire helpers
// ---------------------------------------------------------------------------

inline void writeLe32(uint8_t *dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void writeLe64(uint8_t *dst, uint64_t v) noexcept {
    writeLe32(dst, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    writeLe32(dst + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

inline uint32_t readLe32(const uint8_t *src) noexcept {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

inline uint64_t readLe64(const uint8_t *src) noexcept {
    return static_cast<uint64_t>(readLe32(src)) | (static_cast<uint64_t>(readLe32(src + 4)) << 32);
}

inline DecodeStatus parseFrameHeader(const uint8_t *data, size_t len, FrameHeader &out) noexcept {
    if (len < kFrameHeaderBytes)
        return DecodeStatus::NeedMoreData;
    out.magic = readLe32(data);
    out.type = readLe32(data + 4);
    out.payload_len = readLe32(data + 8);
    if (out.magic != kFrameMagic)
        return DecodeStatus::BadMagic;
    if (out.payload_len > kMaxFramePayloadBytes)
        return DecodeStatus::FrameTooLarge;
    return DecodeStatus::Ok;
}

inline std::vector<uint8_t> encodeFrame(FrameType type, const uint8_t *payload, size_t payload_len) {
    if (payload_len > kMaxFramePayloadBytes)
        return {};
    std::vector<uint8_t> out(kFrameHeaderBytes + payload_len);
    writeLe32(out.data(), kFrameMagic);
    writeLe32(out.data() + 4, static_cast<uint32_t>(type));
    writeLe32(out.data() + 8, static_cast<uint32_t>(payload_len));
    if (payload_len > 0 && payload != nullptr)
        std::memcpy(out.data() + kFrameHeaderBytes, payload, payload_len);
    return out;
}

inline std::vector<uint8_t> encodeFrame(FrameType type, std::string_view utf8_payload) {
    return encodeFrame(type, reinterpret_cast<const uint8_t *>(utf8_payload.data()), utf8_payload.size());
}

inline std::vector<uint8_t> encodeFrame(FrameType type) { return encodeFrame(type, nullptr, 0); }

inline DecodeStatus tryDecodeFrame(const uint8_t *data, size_t len, Frame &out, size_t &consumed) {
    consumed = 0;
    FrameHeader hdr;
    const DecodeStatus hdrStatus = parseFrameHeader(data, len, hdr);
    if (hdrStatus != DecodeStatus::Ok)
        return hdrStatus;
    const size_t total = kFrameHeaderBytes + static_cast<size_t>(hdr.payload_len);
    if (len < total)
        return DecodeStatus::NeedMoreData;
    out.type = static_cast<FrameType>(hdr.type);
    out.payload.assign(data + kFrameHeaderBytes, data + total);
    consumed = total;
    return DecodeStatus::Ok;
}

inline size_t bytesPerSample(PcmSampleFormat fmt) noexcept {
    return fmt == PcmSampleFormat::Int16 ? sizeof(int16_t) : sizeof(float);
}

inline std::vector<uint8_t> encodePcmChunkPayload(uint64_t request_id,
                                                  uint32_t sample_rate,
                                                  uint32_t sample_count,
                                                  PcmSampleFormat format,
                                                  const void *pcm_bytes,
                                                  size_t pcm_byte_len) {
    const size_t expected = static_cast<size_t>(sample_count) * bytesPerSample(format);
    if (pcm_byte_len != expected || kPcmChunkHeaderBytes + pcm_byte_len > kMaxFramePayloadBytes)
        return {};
    std::vector<uint8_t> out(kPcmChunkHeaderBytes + pcm_byte_len);
    writeLe64(out.data() + 0, request_id);
    writeLe32(out.data() + 8, sample_rate);
    writeLe32(out.data() + 12, sample_count);
    writeLe32(out.data() + 16, static_cast<uint32_t>(format));
    writeLe32(out.data() + 20, 0u);
    if (pcm_byte_len > 0 && pcm_bytes != nullptr)
        std::memcpy(out.data() + kPcmChunkHeaderBytes, pcm_bytes, pcm_byte_len);
    return out;
}

inline bool decodePcmChunkPayload(const uint8_t *payload,
                                  size_t payload_len,
                                  PcmChunkHeader &hdr,
                                  const uint8_t *&pcm_out,
                                  size_t &pcm_len_out) noexcept {
    if (!payload || payload_len < kPcmChunkHeaderBytes)
        return false;
    hdr.request_id = readLe64(payload + 0);
    hdr.sample_rate = readLe32(payload + 8);
    hdr.sample_count = readLe32(payload + 12);
    hdr.sample_format = readLe32(payload + 16);
    hdr.flags = readLe32(payload + 20);
    if (hdr.sample_format != static_cast<uint32_t>(PcmSampleFormat::Float32) &&
        hdr.sample_format != static_cast<uint32_t>(PcmSampleFormat::Int16))
        return false;
    const size_t expected =
        static_cast<size_t>(hdr.sample_count) * bytesPerSample(static_cast<PcmSampleFormat>(hdr.sample_format));
    if (payload_len != kPcmChunkHeaderBytes + expected)
        return false;
    pcm_out = payload + kPcmChunkHeaderBytes;
    pcm_len_out = expected;
    return true;
}

inline std::vector<uint8_t> encodePcm16FromFloat(const float *samples, size_t count) {
    std::vector<uint8_t> out(count * 2u);
    for (size_t i = 0; i < count; ++i) {
        float s = samples[i];
        if (!std::isfinite(s))
            s = 0.0f;
        if (s > 1.0f)
            s = 1.0f;
        else if (s < -1.0f)
            s = -1.0f;
        const int32_t v = static_cast<int32_t>(std::lrintf(s * 32767.0f));
        const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)));
        out[i * 2u] = static_cast<uint8_t>(u & 0xFFu);
        out[i * 2u + 1u] = static_cast<uint8_t>((u >> 8) & 0xFFu);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Minimal dependency-free JSON serializer & parser (RFC 8259 subset/full UTF-8)
// ---------------------------------------------------------------------------

namespace Json {

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    using Storage = std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>;
    Storage data{nullptr};

    bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool() const noexcept { return std::holds_alternative<bool>(data); }
    bool isInt() const noexcept { return std::holds_alternative<int64_t>(data); }
    bool isNumber() const noexcept {
        return std::holds_alternative<int64_t>(data) || std::holds_alternative<double>(data);
    }
    bool isString() const noexcept { return std::holds_alternative<std::string>(data); }
    bool isArray() const noexcept { return std::holds_alternative<Array>(data); }
    bool isObject() const noexcept { return std::holds_alternative<Object>(data); }

    std::string asString(std::string_view fallback = {}) const {
        if (const auto *s = std::get_if<std::string>(&data))
            return *s;
        return std::string(fallback);
    }
    int64_t asInt(int64_t fallback = 0) const noexcept {
        if (const auto *v = std::get_if<int64_t>(&data))
            return *v;
        if (const auto *d = std::get_if<double>(&data))
            return static_cast<int64_t>(*d);
        return fallback;
    }
    uint64_t asUint(uint64_t fallback = 0) const noexcept {
        if (const auto *v = std::get_if<int64_t>(&data))
            return *v >= 0 ? static_cast<uint64_t>(*v) : fallback;
        if (const auto *d = std::get_if<double>(&data))
            return *d >= 0.0 ? static_cast<uint64_t>(*d) : fallback;
        return fallback;
    }
    double asDouble(double fallback = 0.0) const noexcept {
        if (const auto *d = std::get_if<double>(&data))
            return *d;
        if (const auto *v = std::get_if<int64_t>(&data))
            return static_cast<double>(*v);
        return fallback;
    }
    bool asBool(bool fallback = false) const noexcept {
        if (const auto *b = std::get_if<bool>(&data))
            return *b;
        return fallback;
    }
    const Object *asObject() const noexcept { return std::get_if<Object>(&data); }
};

inline std::string escapeString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20u) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[(c >> 4) & 0xFu]);
                out.push_back(kHex[c & 0xFu]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

inline void appendUtf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view input) : m_in(input) {}

    std::optional<Value> parse() {
        skipWs();
        auto v = parseValue(0);
        if (!v)
            return std::nullopt;
        skipWs();
        if (m_pos != m_in.size())
            return std::nullopt;
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;

    void skipWs() noexcept {
        while (m_pos < m_in.size()) {
            const char c = m_in[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++m_pos;
            else
                break;
        }
    }

    bool consume(char expected) noexcept {
        if (m_pos < m_in.size() && m_in[m_pos] == expected) {
            ++m_pos;
            return true;
        }
        return false;
    }

    std::optional<Value> parseValue(int depth) {
        if (depth > kMaxDepth || m_pos >= m_in.size())
            return std::nullopt;
        const char c = m_in[m_pos];
        if (c == '{')
            return parseObject(depth + 1);
        if (c == '[')
            return parseArray(depth + 1);
        if (c == '"') {
            auto s = parseString();
            if (!s)
                return std::nullopt;
            return Value{std::move(*s)};
        }
        if (c == 't') {
            if (m_in.substr(m_pos, 4) == "true") {
                m_pos += 4;
                return Value{true};
            }
            return std::nullopt;
        }
        if (c == 'f') {
            if (m_in.substr(m_pos, 5) == "false") {
                m_pos += 5;
                return Value{false};
            }
            return std::nullopt;
        }
        if (c == 'n') {
            if (m_in.substr(m_pos, 4) == "null") {
                m_pos += 4;
                return Value{nullptr};
            }
            return std::nullopt;
        }
        if (c == '-' || (c >= '0' && c <= '9'))
            return parseNumber();
        return std::nullopt;
    }

    std::optional<uint32_t> parseHex4() noexcept {
        if (m_pos + 4 > m_in.size())
            return std::nullopt;
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_in[m_pos++];
            val <<= 4;
            if (c >= '0' && c <= '9')
                val |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                val |= static_cast<uint32_t>(10 + (c - 'a'));
            else if (c >= 'A' && c <= 'F')
                val |= static_cast<uint32_t>(10 + (c - 'A'));
            else
                return std::nullopt;
        }
        return val;
    }

    std::optional<std::string> parseString() {
        if (!consume('"'))
            return std::nullopt;
        std::string out;
        while (m_pos < m_in.size()) {
            const unsigned char c = static_cast<unsigned char>(m_in[m_pos++]);
            if (c == '"')
                return out;
            if (c < 0x20u)
                return std::nullopt;
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (m_pos >= m_in.size())
                return std::nullopt;
            const char esc = m_in[m_pos++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto u1 = parseHex4();
                if (!u1)
                    return std::nullopt;
                uint32_t cp = *u1;
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if (m_pos + 2 > m_in.size() || m_in[m_pos] != '\\' || m_in[m_pos + 1] != 'u')
                        return std::nullopt;
                    m_pos += 2;
                    auto u2 = parseHex4();
                    if (!u2 || *u2 < 0xDC00u || *u2 > 0xDFFFu)
                        return std::nullopt;
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (*u2 - 0xDC00u));
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    return std::nullopt;
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> parseNumber() {
        const size_t start = m_pos;
        if (m_pos < m_in.size() && m_in[m_pos] == '-')
            ++m_pos;
        if (m_pos >= m_in.size())
            return std::nullopt;
        if (m_in[m_pos] == '0') {
            ++m_pos;
        } else if (m_in[m_pos] >= '1' && m_in[m_pos] <= '9') {
            while (m_pos < m_in.size() && m_in[m_pos] >= '0' && m_in[m_pos] <= '9')
                ++m_pos;
        } else {
            return std::nullopt;
        }
        bool isFloat = false;
        if (m_pos < m_in.size() && m_in[m_pos] == '.') {
            isFloat = true;
            ++m_pos;
            if (m_pos >= m_in.size() || m_in[m_pos] < '0' || m_in[m_pos] > '9')
                return std::nullopt;
            while (m_pos < m_in.size() && m_in[m_pos] >= '0' && m_in[m_pos] <= '9')
                ++m_pos;
        }
        if (m_pos < m_in.size() && (m_in[m_pos] == 'e' || m_in[m_pos] == 'E')) {
            isFloat = true;
            ++m_pos;
            if (m_pos < m_in.size() && (m_in[m_pos] == '+' || m_in[m_pos] == '-'))
                ++m_pos;
            if (m_pos >= m_in.size() || m_in[m_pos] < '0' || m_in[m_pos] > '9')
                return std::nullopt;
            while (m_pos < m_in.size() && m_in[m_pos] >= '0' && m_in[m_pos] <= '9')
                ++m_pos;
        }
        const std::string_view token = m_in.substr(start, m_pos - start);
        if (!isFloat) {
            int64_t iv = 0;
            const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), iv);
            if (ec == std::errc{} && ptr == token.data() + token.size())
                return Value{iv};
        }
        const std::string copy(token);
        char *endPtr = nullptr;
        const double dv = std::strtod(copy.c_str(), &endPtr);
        if (!endPtr || static_cast<size_t>(endPtr - copy.c_str()) != copy.size() || !std::isfinite(dv))
            return std::nullopt;
        return Value{dv};
    }

    std::optional<Value> parseArray(int depth) {
        if (!consume('['))
            return std::nullopt;
        Array arr;
        skipWs();
        if (consume(']'))
            return Value{std::move(arr)};
        while (true) {
            skipWs();
            auto elem = parseValue(depth);
            if (!elem)
                return std::nullopt;
            arr.push_back(std::move(*elem));
            skipWs();
            if (consume(']'))
                return Value{std::move(arr)};
            if (!consume(','))
                return std::nullopt;
        }
    }

    std::optional<Value> parseObject(int depth) {
        if (!consume('{'))
            return std::nullopt;
        Object obj;
        skipWs();
        if (consume('}'))
            return Value{std::move(obj)};
        while (true) {
            skipWs();
            auto key = parseString();
            if (!key)
                return std::nullopt;
            skipWs();
            if (!consume(':'))
                return std::nullopt;
            skipWs();
            auto val = parseValue(depth);
            if (!val)
                return std::nullopt;
            obj.insert_or_assign(std::move(*key), std::move(*val));
            skipWs();
            if (consume('}'))
                return Value{std::move(obj)};
            if (!consume(','))
                return std::nullopt;
        }
    }

    std::string_view m_in;
    size_t m_pos = 0;
};

inline std::optional<Object> parseObject(std::string_view input) {
    auto v = Parser(input).parse();
    if (!v || !v->isObject())
        return std::nullopt;
    return std::get<Object>(std::move(v->data));
}

inline std::string getString(const Object &obj, const std::string &key, std::string_view fallback = {}) {
    const auto it = obj.find(key);
    return it != obj.end() ? it->second.asString(fallback) : std::string(fallback);
}

inline int64_t getInt(const Object &obj, const std::string &key, int64_t fallback = 0) noexcept {
    const auto it = obj.find(key);
    return it != obj.end() ? it->second.asInt(fallback) : fallback;
}

inline uint64_t getUint(const Object &obj, const std::string &key, uint64_t fallback = 0) noexcept {
    const auto it = obj.find(key);
    return it != obj.end() ? it->second.asUint(fallback) : fallback;
}

inline double getDouble(const Object &obj, const std::string &key, double fallback = 0.0) noexcept {
    const auto it = obj.find(key);
    return it != obj.end() ? it->second.asDouble(fallback) : fallback;
}

}  // namespace Json

}  // namespace ClothboundTts
