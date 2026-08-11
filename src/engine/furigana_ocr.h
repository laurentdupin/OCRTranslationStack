#ifndef LOCALAI_FURIGANA_OCR_H
#define LOCALAI_FURIGANA_OCR_H

#include "local_ai.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace localai {

struct ImageBuffer {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<unsigned char> rgb;
};

bool load_image_rgb(const std::wstring & path, ImageBuffer & image, std::string & error);

struct FuriganaToken {
    std::string surface_utf8;
    std::string reading_utf8;
    LocalAIQuad image_quad{};
    float confidence = 0.0f;
    uint32_t flags = 0;
};

struct OcrRegion {
    std::string text_utf8;
    LocalAIQuad image_quad{};
    float confidence = 0.0f;
    uint32_t flags = 0;
};

struct FuriganaOutput {
    std::string text;
    std::vector<FuriganaToken> tokens;
    std::vector<OcrRegion> regions;
};

class NativeFuriganaOcr final {
public:
    NativeFuriganaOcr(
        std::wstring detection_model_path,
        std::wstring recognition_model_path,
        std::wstring dictionary_path,
        std::wstring japanese_dictionary_path,
        uint32_t maximum_image_side,
        uint32_t threads);
    ~NativeFuriganaOcr();

    NativeFuriganaOcr(const NativeFuriganaOcr &) = delete;
    NativeFuriganaOcr & operator=(const NativeFuriganaOcr &) = delete;

    LocalAIStatus run(
        const ImageBuffer & image,
        const char * source_language,
        const std::atomic_bool & cancel_requested,
        FuriganaOutput & output,
        std::string & error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace localai

#endif
