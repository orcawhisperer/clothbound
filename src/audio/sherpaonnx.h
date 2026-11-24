#pragma once

// The parts of sherpa-onnx's C API (https://github.com/k2-fsa/sherpa-onnx,
// sherpa-onnx/c-api/c-api.h at v1.13.8, Apache-2.0, Copyright Xiaomi
// Corporation) that Clothbound's built-in voices use. The library itself is
// downloaded on first use and loaded at run time; these declarations must
// match its version exactly, which is checked before it's used.

#include <cstdint>

namespace SherpaOnnx {

constexpr const char *version = "1.13.8";

struct VitsModelConfig {
    const char *model;
    const char *lexicon;
    const char *tokens;
    const char *data_dir;
    float noise_scale;
    float noise_scale_w;
    float length_scale;
    const char *dict_dir;
};

struct MatchaModelConfig {
    const char *acoustic_model;
    const char *vocoder;
    const char *lexicon;
    const char *tokens;
    const char *data_dir;
    float noise_scale;
    float length_scale;
    const char *dict_dir;
};

struct KokoroModelConfig {
    const char *model;
    const char *voices;
    const char *tokens;
    const char *data_dir;
    float length_scale;
    const char *dict_dir;
    const char *lexicon;
    const char *lang;
};

struct KittenModelConfig {
    const char *model;
    const char *voices;
    const char *tokens;
    const char *data_dir;
    float length_scale;
};

struct ZipvoiceModelConfig {
    const char *tokens;
    const char *encoder;
    const char *decoder;
    const char *vocoder;
    const char *data_dir;
    const char *lexicon;
    float feat_scale;
    float t_shift;
    float target_rms;
    float guidance_scale;
};

struct PocketModelConfig {
    const char *lm_flow;
    const char *lm_main;
    const char *encoder;
    const char *decoder;
    const char *text_conditioner;
    const char *vocab_json;
    const char *token_scores_json;
    int32_t voice_embedding_cache_capacity;
};

struct SupertonicModelConfig {
    const char *duration_predictor;
    const char *text_encoder;
    const char *vector_estimator;
    const char *vocoder;
    const char *tts_json;
    const char *unicode_indexer;
    const char *voice_style;
};

struct OfflineTtsModelConfig {
    VitsModelConfig vits;
    int32_t num_threads;
    int32_t debug;
    const char *provider;
    MatchaModelConfig matcha;
    KokoroModelConfig kokoro;
    KittenModelConfig kitten;
    ZipvoiceModelConfig zipvoice;
    PocketModelConfig pocket;
    SupertonicModelConfig supertonic;
};

struct OfflineTtsConfig {
    OfflineTtsModelConfig model;
    const char *rule_fsts;
    int32_t max_num_sentences;
    const char *rule_fars;
    float silence_scale;
};

struct GeneratedAudio {
    const float *samples;
    int32_t n;
    int32_t sample_rate;
};

struct GenerationConfig {
    float silence_scale;
    float speed;
    int32_t sid;
    const float *reference_audio;
    int32_t reference_audio_len;
    int32_t reference_sample_rate;
    const char *reference_text;
    int32_t num_steps;
    const char *extra;
};

struct OfflineTts;

// Called as audio is made; returning 0 stops the generation.
using ProgressCallback = int32_t (*)(const float *samples, int32_t n, float progress, void *arg);

using CreateOfflineTts = const OfflineTts *(*)(const OfflineTtsConfig *config);
using DestroyOfflineTts = void (*)(const OfflineTts *tts);
using GenerateWithConfig = const GeneratedAudio *(*)(const OfflineTts *tts, const char *text,
                                                     const GenerationConfig *config, ProgressCallback callback,
                                                     void *arg);
using DestroyGeneratedAudio = void (*)(const GeneratedAudio *audio);
using SampleRate = int32_t (*)(const OfflineTts *tts);
using NumSpeakers = int32_t (*)(const OfflineTts *tts);
using VersionString = const char *(*)();

}  // namespace SherpaOnnx
