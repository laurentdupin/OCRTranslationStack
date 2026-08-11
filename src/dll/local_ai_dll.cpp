#include "local_ai_engine.h"

#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <new>

struct LocalAIEngine {
    std::unique_ptr<localai::Engine> implementation;
};

namespace {

constexpr uint32_t config_required_size() noexcept {
    return static_cast<uint32_t>(
        offsetof(LocalAIConfig, maximum_vram_bytes) + sizeof(uint64_t));
}

bool field_is_present(const LocalAIConfig * config, size_t offset, size_t size) noexcept {
    return config != nullptr &&
           static_cast<size_t>(config->struct_size) >= offset + size;
}

LocalAIConfig normalize_config(const LocalAIConfig * input) noexcept {
    LocalAIConfig result{};
    result.struct_size = sizeof(result);
    result.gpu_layers = -1;
    if (field_is_present(input, offsetof(LocalAIConfig, ocr_model_path),
                         sizeof(input->ocr_model_path))) {
        result.ocr_model_path = input->ocr_model_path;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, vision_projector_path),
                         sizeof(input->vision_projector_path))) {
        result.vision_projector_path = input->vision_projector_path;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, translation_model_path),
                         sizeof(input->translation_model_path))) {
        result.translation_model_path = input->translation_model_path;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, translation_context_size),
                         sizeof(input->translation_context_size))) {
        result.translation_context_size = input->translation_context_size;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, ocr_context_size),
                         sizeof(input->ocr_context_size))) {
        result.ocr_context_size = input->ocr_context_size;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, gpu_layers),
                         sizeof(input->gpu_layers))) {
        result.gpu_layers = input->gpu_layers;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, maximum_vram_bytes),
                         sizeof(input->maximum_vram_bytes))) {
        result.maximum_vram_bytes = input->maximum_vram_bytes;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, flags), sizeof(input->flags))) {
        result.flags = input->flags;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, ppocr_detection_model_path),
                         sizeof(input->ppocr_detection_model_path))) {
        result.ppocr_detection_model_path = input->ppocr_detection_model_path;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, ppocr_recognition_model_path),
                         sizeof(input->ppocr_recognition_model_path))) {
        result.ppocr_recognition_model_path = input->ppocr_recognition_model_path;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, ppocr_dictionary_path),
                         sizeof(input->ppocr_dictionary_path))) {
        result.ppocr_dictionary_path = input->ppocr_dictionary_path;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, japanese_dictionary_path),
                         sizeof(input->japanese_dictionary_path))) {
        result.japanese_dictionary_path = input->japanese_dictionary_path;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, ppocr_max_image_side),
                         sizeof(input->ppocr_max_image_side))) {
        result.ppocr_max_image_side = input->ppocr_max_image_side;
    }
    if (field_is_present(input, offsetof(LocalAIConfig, ppocr_threads),
                         sizeof(input->ppocr_threads))) {
        result.ppocr_threads = input->ppocr_threads;
    }
    return result;
}

template <typename Function>
LocalAIStatus call_status(LocalAIEngine * handle, Function && function) noexcept {
    if (handle == nullptr || !handle->implementation) {
        return LOCAL_AI_INVALID_ARGUMENT;
    }
    try {
        return function(*handle->implementation);
    } catch (const std::bad_alloc &) {
        handle->implementation->set_unexpected_error("out of memory in the native backend");
        return LOCAL_AI_OUT_OF_MEMORY;
    } catch (const std::exception & exception) {
        handle->implementation->set_unexpected_error(exception.what());
        return LOCAL_AI_INFERENCE_FAILED;
    } catch (...) {
        handle->implementation->set_unexpected_error("unexpected native exception");
        return LOCAL_AI_INFERENCE_FAILED;
    }
}

} // namespace

extern "C" {

LOCALAI_API uint32_t LOCALAI_CALL local_ai_get_api_version(void) {
    return LOCAL_AI_API_VERSION;
}

LOCALAI_API LocalAIStatus LOCALAI_CALL local_ai_create(
    const LocalAIConfig * config,
    LocalAIEngine ** engine) {
    if (config == nullptr || engine == nullptr) {
        return LOCAL_AI_INVALID_ARGUMENT;
    }
    *engine = nullptr;
    if (config->struct_size < config_required_size()) {
        return LOCAL_AI_STRUCT_TOO_SMALL;
    }
    try {
        auto handle = std::make_unique<LocalAIEngine>();
        handle->implementation = std::make_unique<localai::Engine>(normalize_config(config));
        *engine = handle.release();
        return LOCAL_AI_OK;
    } catch (const std::bad_alloc &) {
        return LOCAL_AI_OUT_OF_MEMORY;
    } catch (...) {
        return LOCAL_AI_MODEL_LOAD_FAILED;
    }
}

LOCALAI_API LocalAIStatus LOCALAI_CALL local_ai_load_ocr(LocalAIEngine * engine) {
    return call_status(engine, [](localai::Engine & implementation) {
        return implementation.load_ocr();
    });
}

LOCALAI_API LocalAIStatus LOCALAI_CALL local_ai_load_translation(LocalAIEngine * engine) {
    return call_status(engine, [](localai::Engine & implementation) {
        return implementation.load_translation();
    });
}

LOCALAI_API LocalAIStatus LOCALAI_CALL local_ai_ocr_file(
    LocalAIEngine * engine,
    const wchar_t * image_path,
    const char * source_language,
    LocalAITextCallback callback,
    void * user_data) {
    return call_status(engine, [&](localai::Engine & implementation) {
        return implementation.ocr_file(image_path, source_language, callback, user_data);
    });
}

LOCALAI_API LocalAIStatus LOCALAI_CALL local_ai_ocr_furigana_file(
    LocalAIEngine * engine,
    const wchar_t * image_path,
    const char * source_language,
    LocalAITextCallback text_callback,
    LocalAIFuriganaCallback token_callback,
    void * user_data) {
    return call_status(engine, [&](localai::Engine & implementation) {
        return implementation.ocr_furigana_file(
            image_path,
            source_language,
            text_callback,
            token_callback,
            nullptr,
            user_data);
    });
}

LOCALAI_API LocalAIStatus LOCALAI_CALL local_ai_ocr_furigana_file_with_regions(
    LocalAIEngine * engine,
    const wchar_t * image_path,
    const char * source_language,
    LocalAITextCallback text_callback,
    LocalAIFuriganaCallback token_callback,
    LocalAITextRegionCallback region_callback,
    void * user_data) {
    return call_status(engine, [&](localai::Engine & implementation) {
        return implementation.ocr_furigana_file(
            image_path,
            source_language,
            text_callback,
            token_callback,
            region_callback,
            user_data);
    });
}

LOCALAI_API LocalAIStatus LOCALAI_CALL local_ai_translate(
    LocalAIEngine * engine,
    const char * source_utf8,
    const char * source_language,
    const char * target_language,
    LocalAITextCallback callback,
    void * user_data) {
    return call_status(engine, [&](localai::Engine & implementation) {
        return implementation.translate(
            source_utf8, source_language, target_language, callback, user_data);
    });
}

LOCALAI_API void LOCALAI_CALL local_ai_cancel(LocalAIEngine * engine) {
    if (engine == nullptr || !engine->implementation) {
        return;
    }
    try {
        engine->implementation->cancel();
    } catch (...) {
    }
}

LOCALAI_API void LOCALAI_CALL local_ai_trim_memory(LocalAIEngine * engine) {
    if (engine == nullptr || !engine->implementation) {
        return;
    }
    try {
        engine->implementation->trim_memory();
    } catch (...) {
    }
}

LOCALAI_API const wchar_t * LOCALAI_CALL local_ai_get_last_error(LocalAIEngine * engine) {
    static const wchar_t empty[] = L"";
    if (engine == nullptr || !engine->implementation) {
        return empty;
    }
    try {
        return engine->implementation->last_error();
    } catch (...) {
        return empty;
    }
}

LOCALAI_API LocalAIStatus LOCALAI_CALL local_ai_get_memory_info(
    LocalAIEngine * engine,
    LocalAIMemoryInfo * info) {
    return call_status(engine, [&](localai::Engine & implementation) {
        return implementation.memory_info(info);
    });
}

LOCALAI_API void LOCALAI_CALL local_ai_destroy(LocalAIEngine * engine) {
    delete engine;
}

} // extern "C"
