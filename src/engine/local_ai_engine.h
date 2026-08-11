#ifndef LOCAL_AI_ENGINE_H
#define LOCAL_AI_ENGINE_H

#include "local_ai.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct llama_context;
struct llama_model;
struct mtmd_context;
struct mtmd_bitmap;
struct mtmd_input_chunks;

namespace localai {

class NativeFuriganaOcr;

class Engine final {
public:
    explicit Engine(const LocalAIConfig & config);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine & operator=(const Engine &) = delete;

    LocalAIStatus load_ocr();
    LocalAIStatus load_translation();
    LocalAIStatus ocr_file(
        const wchar_t * image_path,
        const char * source_language,
        LocalAITextCallback callback,
        void * user_data);
    LocalAIStatus ocr_pixels(
        const uint8_t * pixels,
        uint32_t width,
        uint32_t height,
        uint32_t row_stride_bytes,
        bool bgra,
        const char * source_language,
        LocalAITextCallback callback,
        void * user_data);
    LocalAIStatus ocr_furigana_file(
        const wchar_t * image_path,
        const char * source_language,
        LocalAITextCallback text_callback,
        LocalAIFuriganaCallback token_callback,
        LocalAITextRegionCallback region_callback,
        void * user_data);
    LocalAIStatus translate(
        const char * source_utf8,
        const char * source_language,
        const char * target_language,
        LocalAITextCallback callback,
        void * user_data);

    void cancel() noexcept;
    void trim_memory() noexcept;
    void set_unexpected_error(const char * message) noexcept;
    const wchar_t * last_error() const;
    LocalAIStatus memory_info(LocalAIMemoryInfo * info) const;

private:
    struct Runtime;

    LocalAIStatus load_ocr_locked();
    LocalAIStatus load_translation_locked();
    void unload_ocr_locked() noexcept;
    void unload_translation_locked() noexcept;

    LocalAIStatus run_generation_locked(
        Runtime & runtime,
        const std::string & prompt,
        mtmd_bitmap * bitmap,
        uint32_t maximum_tokens,
        std::string & output);

    LocalAIStatus run_translation_segment_locked(
        const std::string & segment,
        const std::string & source_language,
        const std::string & target_language,
        std::string & output);

    LocalAIStatus run_ocr_bitmap_locked(
        mtmd_bitmap * bitmap,
        const std::string & source_language,
        std::string & output);

    void set_error(const std::string & message);
    void set_error(const std::wstring & message);
    void clear_error();
    void refresh_memory() const noexcept;
    bool over_vram_budget() const noexcept;
    bool is_cancelled() const noexcept;

    static bool model_progress(float progress, void * user_data);
    static bool mtmd_progress(float progress, void * user_data);
    static bool abort_callback(void * user_data);

    std::wstring ocr_model_path_;
    std::wstring projector_path_;
    std::wstring translation_model_path_;
    std::wstring ppocr_detection_model_path_;
    std::wstring ppocr_recognition_model_path_;
    std::wstring ppocr_dictionary_path_;
    std::wstring japanese_dictionary_path_;
    uint32_t translation_context_size_ = 2048;
    uint32_t ocr_context_size_ = 8192;
    uint32_t ppocr_max_image_side_ = 1280;
    uint32_t ppocr_threads_ = 0;
    int32_t gpu_layers_ = -1;
    uint64_t maximum_vram_bytes_ = 0;
    uint32_t flags_ = LOCAL_AI_CONFIG_NONE;

    mutable std::mutex operation_mutex_;
    mutable std::mutex error_mutex_;
    std::atomic_bool cancel_requested_{false};
    std::wstring last_error_;

    std::unique_ptr<Runtime> ocr_;
    std::unique_ptr<Runtime> translation_;
    std::unique_ptr<NativeFuriganaOcr> furigana_;

    mutable std::mutex memory_mutex_;
    mutable uint64_t baseline_free_bytes_ = 0;
    mutable uint64_t peak_allocated_bytes_ = 0;
    mutable uint64_t current_allocated_bytes_ = 0;
    mutable uint64_t device_free_bytes_ = 0;
    mutable uint64_t device_total_bytes_ = 0;
    mutable bool memory_reliable_ = false;
};

} // namespace localai

#endif
