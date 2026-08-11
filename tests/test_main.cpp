#include "local_ai.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Capture {
    std::string text;
};

struct FuriganaCapture {
    std::string text;
    struct Token {
        std::string surface;
        std::string reading;
        LocalAIQuad quad{};
    };
    std::vector<Token> tokens;
    struct Region {
        std::string text;
        LocalAIQuad quad{};
    };
    std::vector<Region> regions;
};

void LOCALAI_CALL capture_text(const char * text, size_t byte_count, void * user_data) {
    auto * capture = static_cast<Capture *>(user_data);
    if (capture != nullptr) {
        capture->text.assign(text == nullptr ? "" : text, byte_count);
    }
}

void LOCALAI_CALL capture_furigana_tokens(
    const LocalAIFuriganaToken * tokens,
    size_t token_count,
    void * user_data) {
    auto * capture = static_cast<FuriganaCapture *>(user_data);
    if (capture == nullptr) {
        return;
    }
    capture->tokens.clear();
    for (size_t index = 0; index < token_count; ++index) {
        FuriganaCapture::Token token;
        token.surface.assign(
            tokens[index].surface_utf8 == nullptr ? "" : tokens[index].surface_utf8,
            tokens[index].surface_byte_count);
        token.reading.assign(
            tokens[index].reading_utf8 == nullptr ? "" : tokens[index].reading_utf8,
            tokens[index].reading_byte_count);
        token.quad = tokens[index].image_quad;
        capture->tokens.push_back(std::move(token));
    }
}

void LOCALAI_CALL capture_text_regions(
    const LocalAITextRegion * regions,
    size_t region_count,
    void * user_data) {
    auto * capture = static_cast<FuriganaCapture *>(user_data);
    if (capture == nullptr) {
        return;
    }
    capture->regions.clear();
    for (size_t index = 0; index < region_count; ++index) {
        FuriganaCapture::Region region;
        region.text.assign(
            regions[index].text_utf8 == nullptr ? "" : regions[index].text_utf8,
            regions[index].text_byte_count);
        region.quad = regions[index].image_quad;
        capture->regions.push_back(std::move(region));
    }
}

std::wstring environment(const wchar_t * name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::wstring value(static_cast<size_t>(required), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return {};
    }
    value.resize(written);
    return value;
}

int32_t environment_int(const wchar_t * name, int32_t fallback) {
    const std::wstring value = environment(name);
    if (value.empty()) {
        return fallback;
    }
    try {
        size_t consumed = 0;
        const int result = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            return fallback;
        }
        return static_cast<int32_t>(result);
    } catch (...) {
        return fallback;
    }
}

bool expect(bool condition, const char * message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
    }
    return condition;
}

int run_basic_api_checks() {
    bool ok = true;
    ok &= expect((local_ai_get_api_version() >> 16u) == LOCAL_AI_API_VERSION_MAJOR, "API major version");
    ok &= expect(
        local_ai_create(nullptr, nullptr) == LOCAL_AI_INVALID_ARGUMENT,
        "null create arguments");

    LocalAIConfig too_small{};
    too_small.struct_size = sizeof(uint32_t);
    LocalAIEngine * engine = nullptr;
    ok &= expect(
        local_ai_create(&too_small, &engine) == LOCAL_AI_STRUCT_TOO_SMALL,
        "versioned config size check");

    const std::wstring missing_model =
        (std::filesystem::temp_directory_path() / L"LocalAI-不存在-Ovis.gguf").wstring();
    const std::wstring missing_projector =
        (std::filesystem::temp_directory_path() / L"LocalAI-不存在-projector.gguf").wstring();
    const std::wstring missing_translation =
        (std::filesystem::temp_directory_path() / L"LocalAI-不存在-Hy.gguf").wstring();
    LocalAIConfig config{};
    config.struct_size = sizeof(config);
    config.ocr_model_path = missing_model.c_str();
    config.vision_projector_path = missing_projector.c_str();
    config.translation_model_path = missing_translation.c_str();
    config.gpu_layers = -1;
    config.translation_context_size = 2048;
    config.ocr_context_size = 8192;
    config.maximum_vram_bytes = 6ull * 1024ull * 1024ull * 1024ull;
    config.flags = LOCAL_AI_CONFIG_SEQUENTIAL_MODELS;
    ok &= expect(local_ai_create(&config, &engine) == LOCAL_AI_OK, "engine creation");
    if (engine == nullptr) {
        return 1;
    }
    ok &= expect(local_ai_load_ocr(engine) == LOCAL_AI_FILE_NOT_FOUND, "missing OCR model status");
    const wchar_t * error = local_ai_get_last_error(engine);
    ok &= expect(error != nullptr && *error != L'\0', "UTF-16 error text");
    ok &= expect(
        local_ai_load_translation(engine) == LOCAL_AI_FILE_NOT_FOUND,
        "missing translation model status");
    LocalAIMemoryInfo too_small_memory{};
    too_small_memory.struct_size = sizeof(uint32_t);
    ok &= expect(
        local_ai_get_memory_info(engine, &too_small_memory) == LOCAL_AI_STRUCT_TOO_SMALL,
        "memory info size check");
    LocalAIMemoryInfo memory{};
    memory.struct_size = sizeof(memory);
    ok &= expect(local_ai_get_memory_info(engine, &memory) == LOCAL_AI_OK, "memory info call");
    local_ai_cancel(engine);
    local_ai_trim_memory(engine);
    local_ai_destroy(engine);
    return ok ? 0 : 1;
}

int run_optional_smoke() {
    const std::wstring image = environment(L"LOCALAI_TEST_IMAGE");
    const std::wstring ocr_model = environment(L"LOCALAI_OCR_MODEL");
    const std::wstring projector = environment(L"LOCALAI_PROJECTOR");
    const std::wstring translation_model = environment(L"LOCALAI_TRANSLATION_MODEL");
    if (image.empty() || ocr_model.empty() || projector.empty() || translation_model.empty() ||
        !std::filesystem::is_regular_file(image) ||
        !std::filesystem::is_regular_file(ocr_model) ||
        !std::filesystem::is_regular_file(projector) ||
        !std::filesystem::is_regular_file(translation_model)) {
        std::cout << "SMOKE SKIPPED: set LOCALAI_TEST_IMAGE, LOCALAI_OCR_MODEL, "
                     "LOCALAI_PROJECTOR and LOCALAI_TRANSLATION_MODEL.\n";
        return 77;
    }

    LocalAIConfig config{};
    config.struct_size = sizeof(config);
    config.ocr_model_path = ocr_model.c_str();
    config.vision_projector_path = projector.c_str();
    config.translation_model_path = translation_model.c_str();
    config.translation_context_size = 2048;
    config.ocr_context_size = 8192;
    config.gpu_layers = environment_int(L"LOCALAI_TEST_GPU_LAYERS", -1);
    config.maximum_vram_bytes = 6ull * 1024ull * 1024ull * 1024ull;
    config.flags = LOCAL_AI_CONFIG_SEQUENTIAL_MODELS;
    LocalAIEngine * engine = nullptr;
    if (local_ai_create(&config, &engine) != LOCAL_AI_OK || engine == nullptr) {
        std::cerr << "SMOKE FAIL: engine creation\n";
        return 1;
    }

    const std::wstring benchmark_image = environment(L"LOCALAI_BENCHMARK_IMAGE");
    if (!benchmark_image.empty()) {
        if (!std::filesystem::is_regular_file(benchmark_image)) {
            std::wcerr << L"BENCHMARK FAIL: image not found: " << benchmark_image << L"\n";
            local_ai_destroy(engine);
            return 1;
        }

        const auto load_begin = std::chrono::steady_clock::now();
        const LocalAIStatus load_status = local_ai_load_ocr(engine);
        const auto load_end = std::chrono::steady_clock::now();
        const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            load_end - load_begin).count();
        if (load_status != LOCAL_AI_OK) {
            std::wcerr << L"BENCHMARK FAIL: OCR load status " << load_status << L": "
                       << local_ai_get_last_error(engine) << L"\n";
            local_ai_destroy(engine);
            return 1;
        }

        Capture benchmark_output;
        const auto ocr_begin = std::chrono::steady_clock::now();
        const LocalAIStatus ocr_status = local_ai_ocr_file(
            engine,
            benchmark_image.c_str(),
            "Auto",
            &capture_text,
            &benchmark_output);
        const auto ocr_end = std::chrono::steady_clock::now();
        const auto ocr_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            ocr_end - ocr_begin).count();
        if (ocr_status != LOCAL_AI_OK) {
            std::wcerr << L"BENCHMARK FAIL: OCR status " << ocr_status << L": "
                       << local_ai_get_last_error(engine) << L"\n";
            local_ai_destroy(engine);
            return 1;
        }

        LocalAIMemoryInfo memory{};
        memory.struct_size = sizeof(memory);
        local_ai_get_memory_info(engine, &memory);
        std::cout << "BENCHMARK OCR load_ms=" << load_ms
                  << " inference_ms=" << ocr_ms
                  << " output_bytes=" << benchmark_output.text.size()
                  << " peak_allocated_bytes=" << memory.peak_allocated_bytes
                  << " reliable=" << memory.reliable << "\n";
        std::cout << "BENCHMARK OCR OUTPUT BEGIN\n"
                  << benchmark_output.text
                  << "\nBENCHMARK OCR OUTPUT END\n";
        local_ai_destroy(engine);
        return 0;
    }

    if (!environment(L"LOCALAI_SMOKE_LOAD_ONLY").empty()) {
        const LocalAIStatus status = local_ai_load_ocr(engine);
        std::wcerr << L"OCR load status " << status << L": "
                   << local_ai_get_last_error(engine) << L"\n";
        local_ai_destroy(engine);
        return status == LOCAL_AI_OK ? 0 : 1;
    }

    Capture ocr_output;
    const LocalAIStatus ocr_status = local_ai_ocr_file(
        engine,
        image.c_str(),
        "Auto",
        &capture_text,
        &ocr_output);
    if (ocr_status != LOCAL_AI_OK || ocr_output.text.empty()) {
        std::wcerr << L"SMOKE FAIL: OCR status " << ocr_status << L": "
                   << local_ai_get_last_error(engine) << L"\n";
        local_ai_destroy(engine);
        return 1;
    }
    std::cout << "OCR UTF-8 bytes: " << ocr_output.text.size() << "\n";

    const char * text = "Bonjour, world! Prix: 12,50 € — café.";
    for (int iteration = 0; iteration < 2; ++iteration) {
        Capture english_to_french;
        Capture french_to_english;
        if (local_ai_translate(
                engine,
                text,
                "Auto",
                "French",
                &capture_text,
                &english_to_french) != LOCAL_AI_OK ||
            local_ai_translate(
                engine,
                text,
                "French",
                "English",
                &capture_text,
                &french_to_english) != LOCAL_AI_OK ||
            english_to_french.text.empty() || french_to_english.text.empty()) {
            std::wcerr << L"SMOKE FAIL: translation: " << local_ai_get_last_error(engine) << L"\n";
            local_ai_destroy(engine);
            return 1;
        }
    }
    LocalAIMemoryInfo memory{};
    memory.struct_size = sizeof(memory);
    local_ai_get_memory_info(engine, &memory);
    std::cout << "Peak allocated bytes: " << memory.peak_allocated_bytes
              << " reliable=" << memory.reliable << "\n";
    local_ai_cancel(engine);
    local_ai_trim_memory(engine);
    local_ai_destroy(engine);
    return 0;
}

int run_furigana_smoke() {
    const std::wstring image = environment(L"LOCALAI_FURIGANA_IMAGE");
    const std::wstring detector = environment(L"LOCALAI_PPOCR_DET");
    const std::wstring recognizer = environment(L"LOCALAI_PPOCR_REC");
    const std::wstring dictionary = environment(L"LOCALAI_PPOCR_DICT");
    const std::wstring japanese_dictionary = environment(L"LOCALAI_JAPANESE_DICT");
    if (image.empty() || detector.empty() || recognizer.empty() || dictionary.empty() ||
        japanese_dictionary.empty() ||
        !std::filesystem::is_regular_file(image) ||
        !std::filesystem::is_regular_file(detector) ||
        !std::filesystem::is_regular_file(recognizer) ||
        !std::filesystem::is_regular_file(dictionary) ||
        (!std::filesystem::is_regular_file(japanese_dictionary) &&
         !std::filesystem::is_directory(japanese_dictionary))) {
        std::cout << "FURIGANA SMOKE SKIPPED: set LOCALAI_FURIGANA_IMAGE, LOCALAI_PPOCR_DET, "
                     "LOCALAI_PPOCR_REC, LOCALAI_PPOCR_DICT and LOCALAI_JAPANESE_DICT.\n";
        return 77;
    }
    LocalAIConfig config{};
    config.struct_size = sizeof(config);
    config.ppocr_detection_model_path = detector.c_str();
    config.ppocr_recognition_model_path = recognizer.c_str();
    config.ppocr_dictionary_path = dictionary.c_str();
    config.japanese_dictionary_path = japanese_dictionary.c_str();
    config.ppocr_max_image_side = 1280;
    config.ppocr_threads = environment_int(L"LOCALAI_PPOCR_THREADS", 0);
    config.flags = LOCAL_AI_CONFIG_ENABLE_FURIGANA;
    LocalAIEngine * engine = nullptr;
    if (local_ai_create(&config, &engine) != LOCAL_AI_OK || engine == nullptr) {
        std::cerr << "FURIGANA SMOKE FAIL: engine creation\n";
        return 1;
    }
    FuriganaCapture capture;
    const LocalAIStatus status = local_ai_ocr_furigana_file_with_regions(
        engine,
        image.c_str(),
        "Japanese",
        &capture_text,
        &capture_furigana_tokens,
        &capture_text_regions,
        &capture);
    if (status != LOCAL_AI_OK) {
        std::wcerr << L"FURIGANA SMOKE FAIL: " << local_ai_get_last_error(engine) << L"\n";
        local_ai_destroy(engine);
        return 1;
    }
    const bool has_expected_text = capture.text.find("今日") != std::string::npos &&
                                   capture.text.find("図書館") != std::string::npos;
    bool has_expected_reading = false;
    for (const auto & token : capture.tokens) {
        if (token.surface == "今日" && token.reading == "きょう") {
            has_expected_reading = true;
            break;
        }
    }
    float maximum_region_height = 0.0f;
    for (const auto & region : capture.regions) {
        float top = region.quad.points[0].y;
        float bottom = top;
        for (size_t index = 1; index < std::size(region.quad.points); ++index) {
            top = std::min(top, region.quad.points[index].y);
            bottom = std::max(bottom, region.quad.points[index].y);
        }
        maximum_region_height = std::max(maximum_region_height, bottom - top);
    }
    std::cout << "FURIGANA UTF-8 bytes: " << capture.text.size()
              << " tokens: " << capture.tokens.size()
              << " detected regions: " << capture.regions.size()
              << " max region height: " << maximum_region_height << "\n"
              << "FURIGANA OUTPUT BEGIN\n" << capture.text
              << "\nFURIGANA OUTPUT END\n";
    local_ai_destroy(engine);
    if (!has_expected_text || !has_expected_reading || capture.regions.size() < 2u ||
        maximum_region_height < 60.0f) {
        std::cerr << "FURIGANA SMOKE FAIL: expected Japanese text/reading was not returned.\n";
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const int basic_status = run_basic_api_checks();
    if (basic_status != 0) {
        return basic_status;
    }
    if (!environment(L"LOCALAI_RUN_SMOKE").empty()) {
        return run_optional_smoke();
    }
    if (!environment(L"LOCALAI_RUN_FURIGANA_SMOKE").empty()) {
        return run_furigana_smoke();
    }
    std::cout << "Basic API checks passed; model smoke skipped.\n";
    return 0;
}
