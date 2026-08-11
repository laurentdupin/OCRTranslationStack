#ifndef LOCAL_AI_H
#define LOCAL_AI_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#if defined(_WIN32)
#  if defined(LOCALAI_BUILD_DLL)
#    define LOCALAI_API __declspec(dllexport)
#  else
#    define LOCALAI_API __declspec(dllimport)
#  endif
#  define LOCALAI_CALL __cdecl
#else
#  define LOCALAI_API __attribute__((visibility("default")))
#  define LOCALAI_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LOCAL_AI_API_VERSION_MAJOR 1u
#define LOCAL_AI_API_VERSION_MINOR 2u
#define LOCAL_AI_API_VERSION \
    ((LOCAL_AI_API_VERSION_MAJOR << 16u) | LOCAL_AI_API_VERSION_MINOR)

typedef struct LocalAIEngine LocalAIEngine;

typedef enum LocalAIStatus {
    LOCAL_AI_OK = 0,
    LOCAL_AI_INVALID_ARGUMENT = 1,
    LOCAL_AI_FILE_NOT_FOUND = 2,
    LOCAL_AI_MODEL_LOAD_FAILED = 3,
    LOCAL_AI_OUT_OF_MEMORY = 4,
    LOCAL_AI_VULKAN_ERROR = 5,
    LOCAL_AI_CANCELLED = 6,
    LOCAL_AI_INFERENCE_FAILED = 7,
    LOCAL_AI_INVALID_STATE = 8,
    LOCAL_AI_BUSY = 9,
    LOCAL_AI_UNSUPPORTED = 10,
    LOCAL_AI_STRUCT_TOO_SMALL = 11
} LocalAIStatus;

typedef enum LocalAIConfigFlags {
    LOCAL_AI_CONFIG_NONE = 0u,
    /* Unload the inactive model before loading the other model. */
    LOCAL_AI_CONFIG_SEQUENTIAL_MODELS = 1u << 0u,
    /* Keep inactive model contexts resident when sequential mode is enabled. */
    LOCAL_AI_CONFIG_KEEP_INACTIVE_MODELS = 1u << 1u,
    /* Enable the optional native PP-OCRv6/furigana backend. */
    LOCAL_AI_CONFIG_ENABLE_FURIGANA = 1u << 2u
} LocalAIConfigFlags;

typedef struct LocalAIConfig {
    uint32_t struct_size;
    const wchar_t * ocr_model_path;
    const wchar_t * vision_projector_path;
    const wchar_t * translation_model_path;
    uint32_t translation_context_size;
    uint32_t ocr_context_size;
    int32_t gpu_layers;
    uint64_t maximum_vram_bytes;
    uint32_t flags;
    uint32_t reserved;
    /* Optional native PP-OCRv6 model and Japanese dictionary assets. */
    const wchar_t * ppocr_detection_model_path;
    const wchar_t * ppocr_recognition_model_path;
    const wchar_t * ppocr_dictionary_path;
    const wchar_t * japanese_dictionary_path;
    uint32_t ppocr_max_image_side;
    uint32_t ppocr_threads;
} LocalAIConfig;

typedef void (LOCALAI_CALL * LocalAITextCallback)(
    const char * utf8_text,
    size_t byte_count,
    void * user_data);

typedef struct LocalAIPoint {
    float x;
    float y;
} LocalAIPoint;

typedef struct LocalAIQuad {
    LocalAIPoint points[4];
} LocalAIQuad;

typedef struct LocalAIFuriganaToken {
    /* UTF-8 pointers are valid only for the duration of the callback. */
    const char * surface_utf8;
    size_t surface_byte_count;
    const char * reading_utf8;
    size_t reading_byte_count;
    /* Coordinates are in source-image pixels, clockwise from top-left. */
    LocalAIQuad image_quad;
    float confidence;
    uint32_t flags;
} LocalAIFuriganaToken;

typedef void (LOCALAI_CALL * LocalAIFuriganaCallback)(
    const LocalAIFuriganaToken * tokens,
    size_t token_count,
    void * user_data);

typedef struct LocalAITextRegion {
    /* UTF-8 pointer is valid only for the duration of the callback. */
    const char * text_utf8;
    size_t text_byte_count;
    /* Coordinates are in source-image pixels, clockwise from top-left. */
    LocalAIQuad image_quad;
    float confidence;
    uint32_t flags;
} LocalAITextRegion;

enum {
    LOCAL_AI_TEXT_REGION_DETECTED = 1u << 0u
};

typedef void (LOCALAI_CALL * LocalAITextRegionCallback)(
    const LocalAITextRegion * regions,
    size_t region_count,
    void * user_data);

typedef struct LocalAIMemoryInfo {
    uint32_t struct_size;
    uint32_t reliable;
    uint64_t current_allocated_bytes;
    uint64_t peak_allocated_bytes;
    uint64_t device_free_bytes;
    uint64_t device_total_bytes;
} LocalAIMemoryInfo;

LOCALAI_API uint32_t local_ai_get_api_version(void);

LOCALAI_API LocalAIStatus local_ai_create(
    const LocalAIConfig * config,
    LocalAIEngine ** engine);

LOCALAI_API LocalAIStatus local_ai_load_ocr(LocalAIEngine * engine);
LOCALAI_API LocalAIStatus local_ai_load_translation(LocalAIEngine * engine);

LOCALAI_API LocalAIStatus local_ai_ocr_file(
    LocalAIEngine * engine,
    const wchar_t * image_path,
    const char * source_language,
    LocalAITextCallback callback,
    void * user_data);

/*
 * Runs the optional native PP-OCRv6 structured Japanese path. The text
 * callback receives the recognized UTF-8 text. The token callback receives
 * surface/readings and image-space quadrilaterals. Both callbacks are made
 * synchronously on the calling thread and their data is temporary; callers
 * must copy it before returning. The engine is busy for the duration.
 */
LOCALAI_API LocalAIStatus local_ai_ocr_furigana_file(
    LocalAIEngine * engine,
    const wchar_t * image_path,
    const char * source_language,
    LocalAITextCallback text_callback,
    LocalAIFuriganaCallback token_callback,
    void * user_data);

/*
 * Extended structured OCR entry point. It has the same ownership and
 * threading rules as local_ai_ocr_furigana_file and additionally reports
 * every detected/recognized PP-OCR region. The region callback is optional;
 * its UTF-8 text and array are temporary and must be copied before return.
 */
LOCALAI_API LocalAIStatus local_ai_ocr_furigana_file_with_regions(
    LocalAIEngine * engine,
    const wchar_t * image_path,
    const char * source_language,
    LocalAITextCallback text_callback,
    LocalAIFuriganaCallback token_callback,
    LocalAITextRegionCallback region_callback,
    void * user_data);

LOCALAI_API LocalAIStatus local_ai_translate(
    LocalAIEngine * engine,
    const char * source_utf8,
    const char * source_language,
    const char * target_language,
    LocalAITextCallback callback,
    void * user_data);

LOCALAI_API void local_ai_cancel(LocalAIEngine * engine);
LOCALAI_API void local_ai_trim_memory(LocalAIEngine * engine);

/*
 * The returned pointer is a thread-local UTF-16 copy. It remains valid until
 * the calling thread invokes this function again. It is safe to call from
 * multiple threads, but the LocalAIEngine handle must remain alive while the
 * call is made. An empty string means that no error has been recorded.
 */
LOCALAI_API const wchar_t * local_ai_get_last_error(LocalAIEngine * engine);

LOCALAI_API LocalAIStatus local_ai_get_memory_info(
    LocalAIEngine * engine,
    LocalAIMemoryInfo * info);

LOCALAI_API void local_ai_destroy(LocalAIEngine * engine);

#ifdef __cplusplus
}
#endif

#endif
