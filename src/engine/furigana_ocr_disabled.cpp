#include "furigana_ocr.h"

namespace localai {

bool load_image_rgb(const std::wstring &, ImageBuffer &, std::string & error) {
    error = "the native PP-OCRv6 backend was disabled at build time";
    return false;
}

struct NativeFuriganaOcr::Impl {};

NativeFuriganaOcr::NativeFuriganaOcr(
    std::wstring,
    std::wstring,
    std::wstring,
    std::wstring,
    uint32_t,
    uint32_t)
    : impl_(std::make_unique<Impl>()) {}

NativeFuriganaOcr::~NativeFuriganaOcr() = default;

LocalAIStatus NativeFuriganaOcr::run(
    const ImageBuffer &,
    const char *,
    const std::atomic_bool &,
    FuriganaOutput &,
    std::string & error) {
    error = "the native PP-OCRv6 backend was disabled at build time";
    return LOCAL_AI_UNSUPPORTED;
}

} // namespace localai
