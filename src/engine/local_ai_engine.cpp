#include "local_ai_engine.h"

#include "furigana_ocr.h"

#include "ggml-backend.h"
#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#if defined(_WIN32)
#  include <windows.h>
#  include <objbase.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace localai {

namespace {

std::once_flag g_backend_once;

struct ScopedLlamaLogCapture final {
    ggml_log_callback previous_callback = nullptr;
    void * previous_user_data = nullptr;
    std::string text;

    ScopedLlamaLogCapture() {
        llama_log_get(&previous_callback, &previous_user_data);
        llama_log_set(&ScopedLlamaLogCapture::callback, this);
    }

    ~ScopedLlamaLogCapture() {
        llama_log_set(previous_callback, previous_user_data);
    }

    ScopedLlamaLogCapture(const ScopedLlamaLogCapture &) = delete;
    ScopedLlamaLogCapture & operator=(const ScopedLlamaLogCapture &) = delete;

    std::string summary() const {
        if (text.empty()) {
            return {};
        }
        constexpr size_t maximum = 2048;
        if (text.size() <= maximum) {
            return text;
        }
        return text.substr(text.size() - maximum);
    }

private:
    static void callback(ggml_log_level, const char * message, void * user_data) noexcept {
        auto * capture = static_cast<ScopedLlamaLogCapture *>(user_data);
        if (capture == nullptr || message == nullptr) {
            return;
        }
        try {
            capture->text.append(message);
        } catch (...) {
            // Logging must never interfere with model loading.
        }
    }
};

void initialize_backend() {
    std::call_once(g_backend_once, [] {
        llama_backend_init();
    });
}

ggml_backend_dev_t primary_gpu_device() {
    ggml_backend_dev_t fallback = nullptr;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        if (device == nullptr) {
            continue;
        }
        const auto type = ggml_backend_dev_type(device);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
            continue;
        }
        if (fallback == nullptr) {
            fallback = device;
        }
        const char * name = ggml_backend_dev_name(device);
        if (name != nullptr && std::strstr(name, "Vulkan") != nullptr) {
            return device;
        }
    }
    return fallback;
}

bool path_exists(const std::wstring & path) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return std::filesystem::is_regular_file(path);
#endif
}

bool wide_to_utf8(const std::wstring & input, std::string & output) {
#if defined(_WIN32)
    if (input.empty()) {
        output.clear();
        return true;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return false;
    }
    output.resize(static_cast<size_t>(required));
    return WideCharToMultiByte(
               CP_UTF8,
               WC_ERR_INVALID_CHARS,
               input.data(),
               static_cast<int>(input.size()),
               output.data(),
               required,
               nullptr,
               nullptr) == required;
#else
    output.assign(input.begin(), input.end());
    return true;
#endif
}

std::wstring ascii_to_wide(const std::string & input) {
#if defined(_WIN32)
    if (input.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0);
    if (required <= 0) {
        return L"Invalid UTF-8 error text";
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            input.data(),
            static_cast<int>(input.size()),
            result.data(),
            required) != required) {
        return L"Invalid UTF-8 error text";
    }
    return result;
#else
    return std::wstring(input.begin(), input.end());
#endif
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_auto_language(const std::string & language) {
    const std::string lowered = lower_ascii(language);
    return lowered.empty() || lowered == "auto" || lowered == "auto detect" ||
           lowered == "auto-detect";
}

constexpr char kOcrBlockMarker[] = "[[LOCALAI_BLOCK]]";

std::string trim_ascii_whitespace(std::string value) {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t begin = 0;
    while (begin < value.size() && is_space(value[begin])) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && is_space(value[end - 1])) {
        --end;
    }
    value.erase(end);
    value.erase(0, begin);
    return value;
}

std::string normalize_ocr_blocks(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            normalized.push_back('\n');
        } else {
            normalized.push_back(text[i]);
        }
    }

    // Three newlines are the internal hard boundary between visually distinct
    // blocks. Keep ordinary paragraph separation at two newlines, but avoid
    // carrying arbitrary runs of blank lines into the editor and translator.
    std::string compact;
    compact.reserve(normalized.size());
    size_t newline_run = 0;
    for (const char c : normalized) {
        if (c == '\n') {
            if (newline_run < 3) {
                compact.push_back(c);
            }
            ++newline_run;
        } else {
            newline_run = 0;
            compact.push_back(c);
        }
    }
    normalized.swap(compact);

    std::vector<std::string> blocks;
    std::string current;
    bool marker_seen = false;
    size_t line_start = 0;
    while (line_start <= normalized.size()) {
        const size_t line_end = normalized.find('\n', line_start);
        const size_t end = line_end == std::string::npos ? normalized.size() : line_end;
        const std::string line = normalized.substr(line_start, end - line_start);
        const std::string trimmed = trim_ascii_whitespace(line);
        if (trimmed == kOcrBlockMarker || trimmed == "[LOCALAI_BLOCK]") {
            marker_seen = true;
            const std::string block = trim_ascii_whitespace(std::move(current));
            if (!block.empty()) {
                blocks.push_back(block);
            }
            current.clear();
        } else {
            if (!current.empty()) {
                current.push_back('\n');
            }
            current += line;
        }
        if (line_end == std::string::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (marker_seen) {
        const std::string block = trim_ascii_whitespace(std::move(current));
        if (!block.empty()) {
            blocks.push_back(block);
        }
        std::string result;
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (i != 0) {
                result += "\n\n\n";
            }
            result += blocks[i];
        }
        return result;
    }
    return trim_ascii_whitespace(std::move(normalized));
}

std::string make_ocr_prompt(const std::string & source_language) {
    std::string prompt =
        "Extract all readable content from the image in natural human reading "
        "order and output the result as a single Markdown document. For charts "
        "or images, do not add captions or descriptions; only transcribe visible "
        "text, and use an HTML image tag with a bbox coordinate only when a "
        "non-text visual region must be represented. Format formulas as LaTeX. "
        "Format tables as HTML: <table>...</table>. Transcribe all other text as "
        "standard Markdown. Preserve the original text without translation or "
        "paraphrasing. Keep every visually distinct text block as an independent "
        "Markdown paragraph, with one blank line between independent blocks. Keep "
        "line breaks that belong to the same block. Do not concatenate unrelated "
        "screen controls, labels, captions, subtitles, columns or dialogue. "
        "Preserve paragraphs, punctuation, tables and line breaks. Do not fabricate "
        "missing text.";
    if (!is_auto_language(source_language)) {
        prompt += " Prioritize text written in " + source_language +
                  ", but retain names, numbers and other visible foreign-language "
                  "fragments instead of filtering them out.";
    }
    return prompt;
}

std::string make_ocr_layout_recovery_prompt(const std::string & source_language) {
    std::string prompt =
        "Re-read the image only for OCR layout recovery. Transcribe every visible "
        "text region in natural reading order. Output only the original text, "
        "without translation, captions, explanations, labels or quotation marks. "
        "Put exactly one blank line between independent visual text blocks. A "
        "block may contain multiple lines; keep those line breaks inside the block. "
        "Never put unrelated text from separate screen regions on the same line. "
        "In particular, keep HUD or control text separate from dialogue or subtitles, "
        "and keep columns, table cells and separate labels in their original structure. "
        "Do not invent missing text or silently discard names, numbers or foreign-language "
        "fragments.";
    if (!is_auto_language(source_language)) {
        prompt += " Prioritize text written in " + source_language +
                  ", but retain names, numbers and other visible foreign-language "
                  "fragments instead of filtering them out.";
    }
    return prompt;
}

std::string remove_thinking_blocks(std::string text) {
    for (;;) {
        const size_t begin = text.find("<think>");
        if (begin == std::string::npos) {
            break;
        }
        const size_t end = text.find("</think>", begin + 7);
        if (end == std::string::npos) {
            text.erase(begin);
            break;
        }
        text.erase(begin, end + 8 - begin);
    }
    return text;
}

std::string remove_visual_image_tags(std::string text) {
    const std::string prefix = "<img src=\"images/bbox_";
    for (;;) {
        const size_t begin = text.find(prefix);
        if (begin == std::string::npos) {
            break;
        }
        const size_t end = text.find(" />", begin + prefix.size());
        if (end == std::string::npos) {
            text.erase(begin);
            break;
        }
        text.replace(begin, end + 3 - begin, "\n" + std::string(kOcrBlockMarker) + "\n");
    }
    return text;
}

bool has_ocr_block_boundary(const std::string & text) {
    return text.find("\n\n") != std::string::npos ||
           text.find(kOcrBlockMarker) != std::string::npos;
}

size_t ocr_line_break_count(const std::string & text) {
    return static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
}

bool likely_concatenated_ocr(const std::string & text) {
    if (text.empty() || has_ocr_block_boundary(text)) {
        return false;
    }

    const size_t line_breaks = ocr_line_break_count(text);

    // A short single-line result can legitimately be one text block. Longer
    // results with no paragraph boundary are the common failure mode for game
    // HUDs and dialogue, so give Ovis one layout-only retry in that case.
    return (line_breaks == 0 && text.size() >= 24) ||
           (line_breaks <= 1 && text.size() >= 64);
}

std::vector<std::string> split_translation_segments(const std::string & text) {
    constexpr size_t kMaxSegmentChars = 6000;
    const std::string normalized = normalize_ocr_blocks(text);
    std::vector<std::string> segments;
    size_t start = 0;
    while (start < normalized.size()) {
        while (start < normalized.size() &&
               (normalized[start] == '\n' || normalized[start] == ' ' || normalized[start] == '\t')) {
            ++start;
        }
        if (start >= normalized.size()) {
            break;
        }
        const bool has_hard_boundaries = normalized.find("\n\n\n") != std::string::npos;
        const std::string boundary_marker = has_hard_boundaries ? "\n\n\n" : "\n\n";
        size_t boundary = normalized.find(boundary_marker, start);
        if (boundary == std::string::npos) {
            boundary = normalized.size();
        }

        std::string paragraph = trim_ascii_whitespace(normalized.substr(start, boundary - start));
        if (paragraph.empty()) {
            if (boundary == normalized.size()) {
                break;
            }
            start = boundary + boundary_marker.size();
            continue;
        }
        if (paragraph.size() <= kMaxSegmentChars ||
            paragraph.find("<table") != std::string::npos ||
            paragraph.find("\n|") != std::string::npos ||
            paragraph.rfind("|", 0) != std::string::npos) {
            segments.push_back(std::move(paragraph));
        } else {
            size_t part_start = 0;
            while (part_start < paragraph.size()) {
                size_t part_end = paragraph.find('\n', part_start);
                if (part_end == std::string::npos) {
                    part_end = paragraph.size();
                }
                if (part_end == part_start && part_end < paragraph.size()) {
                    ++part_end;
                }
                segments.push_back(paragraph.substr(part_start, part_end - part_start));
                part_start = part_end;
                if (part_start < paragraph.size() && paragraph[part_start] == '\n') {
                    ++part_start;
                }
            }
        }

        if (boundary == normalized.size()) {
            break;
        }
        start = boundary + boundary_marker.size();
    }
    if (segments.empty()) {
        segments.push_back(normalized);
    }
    return segments;
}

std::string apply_text_chat_template(const llama_model * model, const std::string & content) {
    llama_chat_message message{"user", content.c_str()};
    const char * model_template = llama_model_chat_template(model, nullptr);
    const char * templates[] = {model_template, "hunyuan-dense"};
    for (const char * tmpl : templates) {
        if (tmpl == nullptr || *tmpl == '\0') {
            continue;
        }
        const int32_t required = llama_chat_apply_template(
            tmpl, &message, 1, true, nullptr, 0);
        if (required < 0) {
            continue;
        }
        std::string result(static_cast<size_t>(required) + 1, '\0');
        if (required == 0 || llama_chat_apply_template(
                                  tmpl,
                                  &message,
                                  1,
                                  true,
                                  result.data(),
                                  required + 1) == required) {
            result.resize(static_cast<size_t>(required));
            return result;
        }
    }
    return {};
}

std::string make_translation_prompt(
    const llama_model * model,
    const std::string & segment,
    const std::string & source_language,
    const std::string & target_language) {
    const std::string constraints =
        "Preserve paragraph boundaries, line breaks, numbers, URLs, placeholders "
        "and table structure. Translate only visible text. Do not add explanations, "
        "commentary or quotation marks. Do not translate code tags, keys or "
        "placeholders.";
    std::string prompt;
    if (!is_auto_language(source_language)) {
        prompt = "Translate the following " + source_language + " text into " +
                 target_language +
                 ". Note that you should only output the translated result without "
                 "any additional explanation. " + constraints + "\n" + segment;
    } else {
        prompt = "Translate the following text into " + target_language +
                 ". Note that you should only output the translated result without "
                 "any additional explanation. " + constraints + "\n" + segment;
    }
    return apply_text_chat_template(model, prompt);
}

struct BitmapDeleter {
    void operator()(mtmd_bitmap * bitmap) const {
        if (bitmap != nullptr) {
            mtmd_bitmap_free(bitmap);
        }
    }
};

using BitmapPtr = std::unique_ptr<mtmd_bitmap, BitmapDeleter>;

#if defined(_WIN32)

class ComScope final {
public:
    ComScope() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = result == S_OK || result == S_FALSE;
        valid_ = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
    ~ComScope() {
        if (initialized_) {
            CoUninitialize();
        }
    }
    bool valid() const { return valid_; }

private:
    bool initialized_ = false;
    bool valid_ = false;
};

BitmapPtr load_bitmap_from_file(const std::wstring & path, std::string & error) {
    ImageBuffer image;
    if (!load_image_rgb(path, image, error)) {
        return nullptr;
    }
    return BitmapPtr(mtmd_bitmap_init(image.width, image.height, image.rgb.data()));
}

#else

BitmapPtr load_bitmap_from_file(const std::wstring &, std::string & error) {
    error = "Windows image loading is unavailable in this build";
    return nullptr;
}

#endif

} // namespace

struct Engine::Runtime {
    llama_model * model = nullptr;
    llama_context * context = nullptr;
    mtmd_context * mtmd = nullptr;
    uint32_t context_size = 0;
    uint32_t batch_size = 0;
};

Engine::Engine(const LocalAIConfig & config)
    : ocr_model_path_(config.ocr_model_path == nullptr ? L"" : config.ocr_model_path),
      projector_path_(config.vision_projector_path == nullptr ? L"" : config.vision_projector_path),
      translation_model_path_(config.translation_model_path == nullptr ? L"" : config.translation_model_path),
      ppocr_detection_model_path_(
          config.ppocr_detection_model_path == nullptr ? L"" : config.ppocr_detection_model_path),
      ppocr_recognition_model_path_(
          config.ppocr_recognition_model_path == nullptr ? L"" : config.ppocr_recognition_model_path),
      ppocr_dictionary_path_(
          config.ppocr_dictionary_path == nullptr ? L"" : config.ppocr_dictionary_path),
      japanese_dictionary_path_(
          config.japanese_dictionary_path == nullptr ? L"" : config.japanese_dictionary_path),
      translation_context_size_(config.translation_context_size == 0 ? 2048 : config.translation_context_size),
      ocr_context_size_(config.ocr_context_size == 0 ? 8192 : config.ocr_context_size),
      ppocr_max_image_side_(config.ppocr_max_image_side == 0 ? 1280 : config.ppocr_max_image_side),
      ppocr_threads_(config.ppocr_threads),
      gpu_layers_(config.gpu_layers),
      maximum_vram_bytes_(config.maximum_vram_bytes),
      flags_(config.flags) {
    initialize_backend();
    refresh_memory();
}

Engine::~Engine() {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    unload_ocr_locked();
    unload_translation_locked();
    furigana_.reset();
}

bool Engine::is_cancelled() const noexcept {
    return cancel_requested_.load(std::memory_order_relaxed);
}

bool Engine::model_progress(float, void * user_data) {
    return user_data != nullptr && !static_cast<Engine *>(user_data)->is_cancelled();
}

bool Engine::mtmd_progress(float, void * user_data) {
    return user_data != nullptr && !static_cast<Engine *>(user_data)->is_cancelled();
}

bool Engine::abort_callback(void * user_data) {
    return user_data == nullptr || static_cast<Engine *>(user_data)->is_cancelled();
}

void Engine::set_error(const std::string & message) {
    set_error(ascii_to_wide(message));
}

void Engine::set_error(const std::wstring & message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = message;
}

void Engine::clear_error() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_.clear();
}

const wchar_t * Engine::last_error() const {
    thread_local std::wstring copy;
    std::lock_guard<std::mutex> lock(error_mutex_);
    copy = last_error_;
    return copy.c_str();
}

void Engine::cancel() noexcept {
    cancel_requested_.store(true, std::memory_order_relaxed);
}

void Engine::set_unexpected_error(const char * message) noexcept {
    try {
        set_error(message == nullptr ? "unexpected native exception" : message);
    } catch (...) {
    }
}

void Engine::unload_ocr_locked() noexcept {
    if (!ocr_) {
        return;
    }
    if (ocr_->mtmd != nullptr) {
        mtmd_free(ocr_->mtmd);
    }
    if (ocr_->context != nullptr) {
        llama_free(ocr_->context);
    }
    if (ocr_->model != nullptr) {
        llama_model_free(ocr_->model);
    }
    ocr_.reset();
    refresh_memory();
}

void Engine::unload_translation_locked() noexcept {
    if (!translation_) {
        return;
    }
    if (translation_->context != nullptr) {
        llama_free(translation_->context);
    }
    if (translation_->model != nullptr) {
        llama_model_free(translation_->model);
    }
    translation_.reset();
    refresh_memory();
}

void Engine::trim_memory() noexcept {
    std::unique_lock<std::mutex> lock(operation_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        cancel();
        return;
    }
    unload_ocr_locked();
    unload_translation_locked();
    furigana_.reset();
}

LocalAIStatus Engine::load_ocr() {
    std::unique_lock<std::mutex> lock(operation_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        set_error("another OCR or translation operation is active");
        return LOCAL_AI_BUSY;
    }
    cancel_requested_.store(false, std::memory_order_relaxed);
    return load_ocr_locked();
}

LocalAIStatus Engine::load_translation() {
    std::unique_lock<std::mutex> lock(operation_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        set_error("another OCR or translation operation is active");
        return LOCAL_AI_BUSY;
    }
    cancel_requested_.store(false, std::memory_order_relaxed);
    return load_translation_locked();
}

LocalAIStatus Engine::load_ocr_locked() {
    if (ocr_) {
        return LOCAL_AI_OK;
    }
    if (ocr_model_path_.empty() || projector_path_.empty()) {
        set_error("OCR model and BF16 vision projector paths must be configured");
        return LOCAL_AI_FILE_NOT_FOUND;
    }
    if (!path_exists(ocr_model_path_)) {
        set_error(L"OCR model file was not found: " + ocr_model_path_);
        return LOCAL_AI_FILE_NOT_FOUND;
    }
    if (!path_exists(projector_path_)) {
        set_error(L"OCR vision projector file was not found: " + projector_path_);
        return LOCAL_AI_FILE_NOT_FOUND;
    }
    if ((flags_ & LOCAL_AI_CONFIG_SEQUENTIAL_MODELS) != 0u &&
        (flags_ & LOCAL_AI_CONFIG_KEEP_INACTIVE_MODELS) == 0u) {
        unload_translation_locked();
    }

    std::string model_path;
    std::string projector_path;
    if (!wide_to_utf8(ocr_model_path_, model_path) ||
        !wide_to_utf8(projector_path_, projector_path)) {
        set_error("model paths are not valid UTF-16");
        return LOCAL_AI_INVALID_ARGUMENT;
    }

    auto runtime = std::make_unique<Runtime>();
    runtime->context_size = ocr_context_size_;
    runtime->batch_size = std::min<uint32_t>(2048u, ocr_context_size_);

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers_;
    ggml_backend_dev_t model_devices[] = {primary_gpu_device(), nullptr};
    if (gpu_layers_ != 0 && model_devices[0] != nullptr) {
        // Avoid silently distributing this small model over unrelated GPUs.
        // Cross-device transfers can dominate OCR latency and make the VRAM
        // measurement meaningless for a single-GPU deployment.
        model_params.devices = model_devices;
    }
    model_params.progress_callback = &Engine::model_progress;
    model_params.progress_callback_user_data = this;
    runtime->model = llama_model_load_from_file(model_path.c_str(), model_params);
    if (runtime->model == nullptr) {
        if (is_cancelled()) {
            set_error("OCR model loading was cancelled");
            return LOCAL_AI_CANCELLED;
        }
        set_error("failed to load the OvisOCR2 GGUF model");
        return LOCAL_AI_MODEL_LOAD_FAILED;
    }

    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx = ocr_context_size_;
    context_params.n_batch = runtime->batch_size;
    context_params.n_ubatch = std::min<uint32_t>(512u, runtime->batch_size);
    context_params.n_seq_max = 1;
    context_params.abort_callback = &Engine::abort_callback;
    context_params.abort_callback_data = this;
    runtime->context = llama_init_from_model(runtime->model, context_params);
    if (runtime->context == nullptr) {
        llama_model_free(runtime->model);
        runtime->model = nullptr;
        set_error("failed to create the OCR inference context");
        return LOCAL_AI_MODEL_LOAD_FAILED;
    }

    mtmd_context_params mtmd_params = mtmd_context_params_default();
    // Keep the projector placement consistent with the requested text-model
    // placement.  gpu_layers == 0 is the documented CPU-only diagnostic mode.
    mtmd_params.use_gpu = gpu_layers_ != 0;
    mtmd_params.n_threads = context_params.n_threads;
    mtmd_params.media_marker = mtmd_default_marker();
    mtmd_params.image_min_tokens = 256;
    mtmd_params.image_max_tokens = 4096;
    mtmd_params.progress_callback = &Engine::mtmd_progress;
    mtmd_params.progress_callback_user_data = this;
    ScopedLlamaLogCapture projector_logs;
    runtime->mtmd = mtmd_init_from_file(projector_path.c_str(), runtime->model, mtmd_params);
    if (runtime->mtmd == nullptr) {
        llama_free(runtime->context);
        llama_model_free(runtime->model);
        runtime->context = nullptr;
        runtime->model = nullptr;
        std::string error =
            "failed to load the OvisOCR2 BF16 projector; verify that it is the "
            "matching qwen3vl_merger projector";
        const std::string diagnostic = projector_logs.summary();
        if (!diagnostic.empty()) {
            error += ": ";
            error += diagnostic;
        }
        set_error(error);
        return LOCAL_AI_MODEL_LOAD_FAILED;
    }

    ocr_ = std::move(runtime);
    refresh_memory();
    if (over_vram_budget()) {
        unload_ocr_locked();
        set_error("OCR model loading exceeded the configured VRAM budget");
        return LOCAL_AI_OUT_OF_MEMORY;
    }
    clear_error();
    return LOCAL_AI_OK;
}

LocalAIStatus Engine::load_translation_locked() {
    if (translation_) {
        return LOCAL_AI_OK;
    }
    if (translation_model_path_.empty()) {
        set_error("translation model path must be configured");
        return LOCAL_AI_FILE_NOT_FOUND;
    }
    if (!path_exists(translation_model_path_)) {
        set_error(L"translation model file was not found: " + translation_model_path_);
        return LOCAL_AI_FILE_NOT_FOUND;
    }
    if ((flags_ & LOCAL_AI_CONFIG_SEQUENTIAL_MODELS) != 0u &&
        (flags_ & LOCAL_AI_CONFIG_KEEP_INACTIVE_MODELS) == 0u) {
        unload_ocr_locked();
    }

    std::string model_path;
    if (!wide_to_utf8(translation_model_path_, model_path)) {
        set_error("translation model path is not valid UTF-16");
        return LOCAL_AI_INVALID_ARGUMENT;
    }
    auto runtime = std::make_unique<Runtime>();
    runtime->context_size = translation_context_size_;
    runtime->batch_size = std::min<uint32_t>(2048u, translation_context_size_);

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers_;
    ggml_backend_dev_t model_devices[] = {primary_gpu_device(), nullptr};
    if (gpu_layers_ != 0 && model_devices[0] != nullptr) {
        model_params.devices = model_devices;
    }
    model_params.progress_callback = &Engine::model_progress;
    model_params.progress_callback_user_data = this;
    runtime->model = llama_model_load_from_file(model_path.c_str(), model_params);
    if (runtime->model == nullptr) {
        if (is_cancelled()) {
            set_error("translation model loading was cancelled");
            return LOCAL_AI_CANCELLED;
        }
        set_error("failed to load the Hy-MT2-1.8B GGUF model");
        return LOCAL_AI_MODEL_LOAD_FAILED;
    }

    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx = translation_context_size_;
    context_params.n_batch = runtime->batch_size;
    context_params.n_ubatch = std::min<uint32_t>(512u, runtime->batch_size);
    context_params.n_seq_max = 1;
    context_params.abort_callback = &Engine::abort_callback;
    context_params.abort_callback_data = this;
    runtime->context = llama_init_from_model(runtime->model, context_params);
    if (runtime->context == nullptr) {
        llama_model_free(runtime->model);
        runtime->model = nullptr;
        set_error("failed to create the translation inference context");
        return LOCAL_AI_MODEL_LOAD_FAILED;
    }

    translation_ = std::move(runtime);
    refresh_memory();
    if (over_vram_budget()) {
        unload_translation_locked();
        set_error("translation model loading exceeded the configured VRAM budget");
        return LOCAL_AI_OUT_OF_MEMORY;
    }
    clear_error();
    return LOCAL_AI_OK;
}

LocalAIStatus Engine::run_generation_locked(
    Runtime & runtime,
    const std::string & prompt,
    mtmd_bitmap * bitmap,
    uint32_t maximum_tokens,
    std::string & output) {
    if (is_cancelled()) {
        return LOCAL_AI_CANCELLED;
    }
    llama_memory_clear(llama_get_memory(runtime.context), true);
    llama_set_causal_attn(runtime.context, true);

    llama_pos n_past = 0;
    if (bitmap != nullptr) {
        if (runtime.mtmd == nullptr) {
            set_error("the selected runtime has no multimodal projector");
            return LOCAL_AI_INVALID_STATE;
        }
        mtmd_input_text input_text{
            prompt.c_str(),
            prompt.size(),
            false,
            true};
        const mtmd_bitmap * bitmaps[] = {bitmap};
        std::unique_ptr<mtmd_input_chunks, decltype(&mtmd_input_chunks_free)> chunks(
            mtmd_input_chunks_init(), &mtmd_input_chunks_free);
        if (!chunks || mtmd_tokenize(runtime.mtmd, chunks.get(), &input_text, bitmaps, 1) != 0) {
            set_error("the image could not be tokenized by libmtmd");
            return is_cancelled() ? LOCAL_AI_CANCELLED : LOCAL_AI_INFERENCE_FAILED;
        }
        const int32_t result = mtmd_helper_eval_chunks(
            runtime.mtmd,
            runtime.context,
            chunks.get(),
            0,
            0,
            static_cast<int32_t>(runtime.batch_size),
            true,
            &n_past);
        if (result != 0) {
            if (is_cancelled() || result == 2) {
                set_error("OCR was cancelled");
                return LOCAL_AI_CANCELLED;
            }
            set_error("libmtmd or Vulkan failed while encoding the image");
            return LOCAL_AI_INFERENCE_FAILED;
        }
    } else {
        const llama_vocab * vocab = llama_model_get_vocab(runtime.model);
        const int32_t required = llama_tokenize(
            vocab,
            prompt.c_str(),
            static_cast<int32_t>(prompt.size()),
            nullptr,
            0,
            false,
            true);
        if (required >= 0 || required == INT32_MIN) {
            set_error("the translation prompt could not be tokenized");
            return LOCAL_AI_INFERENCE_FAILED;
        }
        std::vector<llama_token> tokens(static_cast<size_t>(-required));
        const int32_t token_count = llama_tokenize(
            vocab,
            prompt.c_str(),
            static_cast<int32_t>(prompt.size()),
            tokens.data(),
            required < 0 ? -required : 0,
            false,
            true);
        if (token_count < 0) {
            set_error("the translation prompt could not be tokenized");
            return LOCAL_AI_INFERENCE_FAILED;
        }
        llama_batch batch = llama_batch_init(token_count, 0, 1);
        if (batch.token == nullptr) {
            set_error("out of memory while preparing the translation prompt");
            return LOCAL_AI_OUT_OF_MEMORY;
        }
        batch.n_tokens = token_count;
        for (int32_t i = 0; i < token_count; ++i) {
            batch.token[i] = tokens[static_cast<size_t>(i)];
            batch.pos[i] = i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = i == token_count - 1;
        }
        const int32_t result = llama_decode(runtime.context, batch);
        llama_batch_free(batch);
        if (result != 0) {
            if (is_cancelled() || result == 2) {
                set_error("translation was cancelled");
                return LOCAL_AI_CANCELLED;
            }
            set_error("Vulkan failed while evaluating the translation prompt");
            return LOCAL_AI_INFERENCE_FAILED;
        }
        n_past = token_count;
    }

    llama_sampler * sampler = llama_sampler_init_greedy();
    if (sampler == nullptr) {
        set_error("failed to initialize the deterministic sampler");
        return LOCAL_AI_OUT_OF_MEMORY;
    }
    const llama_vocab * vocab = llama_model_get_vocab(runtime.model);
    llama_batch next = llama_batch_init(1, 0, 1);
    if (next.token == nullptr) {
        llama_sampler_free(sampler);
        set_error("out of memory while preparing generation");
        return LOCAL_AI_OUT_OF_MEMORY;
    }

    output.clear();
    LocalAIStatus status = LOCAL_AI_OK;
    for (uint32_t i = 0; i < maximum_tokens; ++i) {
        if (is_cancelled()) {
            status = LOCAL_AI_CANCELLED;
            set_error("inference was cancelled");
            break;
        }
        const llama_token token = llama_sampler_sample(sampler, runtime.context, -1);
        if (llama_vocab_is_eog(vocab, token)) {
            break;
        }
        llama_sampler_accept(sampler, token);
        char piece_buffer[256];
        int32_t piece_size = llama_token_to_piece(
            vocab, token, piece_buffer, static_cast<int32_t>(sizeof(piece_buffer)), 0, false);
        std::string piece;
        if (piece_size < 0) {
            piece.resize(static_cast<size_t>(-piece_size));
            piece_size = llama_token_to_piece(
                vocab,
                token,
                piece.data(),
                static_cast<int32_t>(piece.size()),
                0,
                false);
        } else if (piece_size > 0) {
            piece.assign(piece_buffer, static_cast<size_t>(piece_size));
        }
        if (piece_size < 0) {
            status = LOCAL_AI_INFERENCE_FAILED;
            set_error("failed to decode a generated token");
            break;
        }
        output += piece;

        next.n_tokens = 1;
        next.token[0] = token;
        next.pos[0] = n_past++;
        next.n_seq_id[0] = 1;
        next.seq_id[0][0] = 0;
        next.logits[0] = i + 1 < maximum_tokens;
        const int32_t result = llama_decode(runtime.context, next);
        if (result != 0) {
            if (is_cancelled() || result == 2) {
                status = LOCAL_AI_CANCELLED;
                set_error("inference was cancelled");
            } else {
                status = LOCAL_AI_INFERENCE_FAILED;
                set_error("Vulkan failed while generating text");
            }
            break;
        }
    }
    llama_synchronize(runtime.context);
    llama_batch_free(next);
    llama_sampler_free(sampler);
    refresh_memory();
    if (status == LOCAL_AI_OK) {
        output = remove_thinking_blocks(std::move(output));
    }
    return status;
}

LocalAIStatus Engine::ocr_file(
    const wchar_t * image_path,
    const char * source_language,
    LocalAITextCallback callback,
    void * user_data) {
    if (image_path == nullptr || *image_path == L'\0' || callback == nullptr) {
        set_error("image path and output callback are required");
        return LOCAL_AI_INVALID_ARGUMENT;
    }
    std::unique_lock<std::mutex> lock(operation_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        set_error("another OCR or translation operation is active");
        return LOCAL_AI_BUSY;
    }
    cancel_requested_.store(false, std::memory_order_relaxed);
    LocalAIStatus status = load_ocr_locked();
    if (status != LOCAL_AI_OK) {
        return status;
    }
    if (!path_exists(image_path)) {
        set_error(L"image file was not found: " + std::wstring(image_path));
        return LOCAL_AI_FILE_NOT_FOUND;
    }

    ComScope com_scope;
    if (!com_scope.valid()) {
        set_error("COM could not be initialized for Windows image decoding");
        return LOCAL_AI_INFERENCE_FAILED;
    }
    std::string image_error;
    BitmapPtr bitmap = load_bitmap_from_file(image_path, image_error);
    if (!bitmap) {
        set_error(image_error.empty() ? "the selected image could not be decoded" : image_error);
        return LOCAL_AI_INFERENCE_FAILED;
    }

    std::string source = source_language == nullptr ? "Auto" : source_language;
    std::string prompt =
        "<|im_start|>user\n" + std::string(mtmd_default_marker()) + "\n" +
        make_ocr_prompt(source) +
        "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
    std::string output;
    status = run_generation_locked(*ocr_, prompt, bitmap.get(), 4096, output);
    if (status != LOCAL_AI_OK) {
        return status;
    }
    output = remove_visual_image_tags(std::move(output));
    output = normalize_ocr_blocks(std::move(output));

    // Ovis normally returns useful Markdown paragraphs, but game screenshots
    // can still be emitted as one long line (for example HUD controls followed
    // by dialogue). Retry only that concatenated case with a layout-specific
    // instruction. This keeps the common path at one image encoding/generation
    // pass while giving the model a chance to recover visual block boundaries.
    if (likely_concatenated_ocr(output)) {
        std::string layout_prompt =
            "<|im_start|>user\n" + std::string(mtmd_default_marker()) + "\n" +
            make_ocr_layout_recovery_prompt(source) +
            "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
        std::string recovered;
        const LocalAIStatus recovery_status = run_generation_locked(
            *ocr_, layout_prompt, bitmap.get(), 4096, recovered);
        if (recovery_status == LOCAL_AI_OK) {
            recovered = remove_visual_image_tags(std::move(recovered));
            recovered = normalize_ocr_blocks(std::move(recovered));
            if (!recovered.empty() &&
                (has_ocr_block_boundary(recovered) ||
                 ocr_line_break_count(recovered) > ocr_line_break_count(output))) {
                output = std::move(recovered);
            }
        } else if (recovery_status != LOCAL_AI_CANCELLED) {
            // The first OCR result is still usable. Do not turn an optional
            // layout recovery failure into a failed OCR request.
            clear_error();
        } else {
            return recovery_status;
        }
    }
    callback(output.data(), output.size(), user_data);
    clear_error();
    return LOCAL_AI_OK;
}

LocalAIStatus Engine::ocr_furigana_file(
    const wchar_t * image_path,
    const char * source_language,
    LocalAITextCallback text_callback,
    LocalAIFuriganaCallback token_callback,
    LocalAITextRegionCallback region_callback,
    void * user_data) {
    if (image_path == nullptr || *image_path == L'\0' || text_callback == nullptr) {
        set_error("image path and text output callback are required");
        return LOCAL_AI_INVALID_ARGUMENT;
    }
    std::unique_lock<std::mutex> lock(operation_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        set_error("another OCR or translation operation is active");
        return LOCAL_AI_BUSY;
    }
    cancel_requested_.store(false, std::memory_order_relaxed);
    if (ppocr_detection_model_path_.empty() || ppocr_recognition_model_path_.empty() ||
        ppocr_dictionary_path_.empty() || japanese_dictionary_path_.empty()) {
        set_error(
            "native furigana OCR requires PP-OCRv6 detector/recognizer, character dictionary "
            "and Japanese reading dictionary paths");
        return LOCAL_AI_FILE_NOT_FOUND;
    }
    if (!path_exists(image_path)) {
        set_error(L"image file was not found: " + std::wstring(image_path));
        return LOCAL_AI_FILE_NOT_FOUND;
    }
    ComScope com_scope;
    if (!com_scope.valid()) {
        set_error("COM could not be initialized for Windows image decoding");
        return LOCAL_AI_INFERENCE_FAILED;
    }
    ImageBuffer image;
    std::string image_error;
    if (!load_image_rgb(image_path, image, image_error)) {
        set_error(image_error.empty() ? "the selected image could not be decoded" : image_error);
        return LOCAL_AI_INFERENCE_FAILED;
    }
    if (!furigana_) {
        furigana_ = std::make_unique<NativeFuriganaOcr>(
            ppocr_detection_model_path_,
            ppocr_recognition_model_path_,
            ppocr_dictionary_path_,
            japanese_dictionary_path_,
            ppocr_max_image_side_,
            ppocr_threads_);
    }
    FuriganaOutput output;
    std::string backend_error;
    const LocalAIStatus status = furigana_->run(
        image,
        source_language,
        cancel_requested_,
        output,
        backend_error);
    if (status != LOCAL_AI_OK) {
        set_error(backend_error.empty() ? "native furigana OCR failed" : backend_error);
        return status;
    }

    text_callback(output.text.data(), output.text.size(), user_data);
    if (token_callback != nullptr) {
        std::vector<LocalAIFuriganaToken> tokens;
        tokens.reserve(output.tokens.size());
        for (const FuriganaToken & token : output.tokens) {
            LocalAIFuriganaToken abi_token{};
            abi_token.surface_utf8 = token.surface_utf8.data();
            abi_token.surface_byte_count = token.surface_utf8.size();
            abi_token.reading_utf8 = token.reading_utf8.data();
            abi_token.reading_byte_count = token.reading_utf8.size();
            abi_token.image_quad = token.image_quad;
            abi_token.confidence = token.confidence;
            abi_token.flags = token.flags;
            tokens.push_back(abi_token);
        }
        token_callback(tokens.data(), tokens.size(), user_data);
    }
    if (region_callback != nullptr) {
        std::vector<LocalAITextRegion> regions;
        regions.reserve(output.regions.size());
        for (const OcrRegion & region : output.regions) {
            LocalAITextRegion abi_region{};
            abi_region.text_utf8 = region.text_utf8.data();
            abi_region.text_byte_count = region.text_utf8.size();
            abi_region.image_quad = region.image_quad;
            abi_region.confidence = region.confidence;
            abi_region.flags = region.flags;
            regions.push_back(abi_region);
        }
        region_callback(regions.data(), regions.size(), user_data);
    }
    clear_error();
    return LOCAL_AI_OK;
}

LocalAIStatus Engine::run_translation_segment_locked(
    const std::string & segment,
    const std::string & source_language,
    const std::string & target_language,
    std::string & output) {
    const std::string prompt = make_translation_prompt(
        translation_->model,
        segment,
        source_language,
        target_language);
    if (prompt.empty()) {
        set_error("the Hy-MT2 chat template is not supported by this llama.cpp revision");
        return LOCAL_AI_UNSUPPORTED;
    }
    return run_generation_locked(*translation_, prompt, nullptr, 2048, output);
}

LocalAIStatus Engine::translate(
    const char * source_utf8,
    const char * source_language,
    const char * target_language,
    LocalAITextCallback callback,
    void * user_data) {
    if (source_utf8 == nullptr || *source_utf8 == '\0' || target_language == nullptr ||
        *target_language == '\0' || callback == nullptr) {
        set_error("source text, target language and output callback are required");
        return LOCAL_AI_INVALID_ARGUMENT;
    }
    std::unique_lock<std::mutex> lock(operation_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        set_error("another OCR or translation operation is active");
        return LOCAL_AI_BUSY;
    }
    cancel_requested_.store(false, std::memory_order_relaxed);
    LocalAIStatus status = load_translation_locked();
    if (status != LOCAL_AI_OK) {
        return status;
    }

    const std::string source = source_language == nullptr ? "Auto" : source_language;
    const std::string target = target_language;
    const std::vector<std::string> segments = split_translation_segments(source_utf8);
    std::string result;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (is_cancelled()) {
            set_error("translation was cancelled");
            return LOCAL_AI_CANCELLED;
        }
        std::string translated;
        status = run_translation_segment_locked(segments[i], source, target, translated);
        if (status != LOCAL_AI_OK) {
            return status;
        }
        if (i != 0) {
            result += "\n\n";
        }
        result += translated;
    }
    callback(result.data(), result.size(), user_data);
    clear_error();
    return LOCAL_AI_OK;
}

void Engine::refresh_memory() const noexcept {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    ggml_backend_dev_t selected = primary_gpu_device();
    if (selected == nullptr) {
        memory_reliable_ = false;
        return;
    }
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    ggml_backend_dev_memory(selected, &free_bytes, &total_bytes);
    if (total_bytes == 0) {
        memory_reliable_ = false;
        return;
    }
    if (!memory_reliable_) {
        baseline_free_bytes_ = static_cast<uint64_t>(free_bytes);
        peak_allocated_bytes_ = 0;
        memory_reliable_ = true;
    }
    device_free_bytes_ = static_cast<uint64_t>(free_bytes);
    device_total_bytes_ = static_cast<uint64_t>(total_bytes);
    current_allocated_bytes_ = baseline_free_bytes_ > device_free_bytes_
                                   ? baseline_free_bytes_ - device_free_bytes_
                                   : 0;
    peak_allocated_bytes_ = std::max(peak_allocated_bytes_, current_allocated_bytes_);
}

bool Engine::over_vram_budget() const noexcept {
    refresh_memory();
    std::lock_guard<std::mutex> lock(memory_mutex_);
    return maximum_vram_bytes_ != 0 && memory_reliable_ &&
           peak_allocated_bytes_ > maximum_vram_bytes_;
}

LocalAIStatus Engine::memory_info(LocalAIMemoryInfo * info) const {
    if (info == nullptr) {
        return LOCAL_AI_INVALID_ARGUMENT;
    }
    const uint32_t minimum_size = static_cast<uint32_t>(
        offsetof(LocalAIMemoryInfo, current_allocated_bytes) + sizeof(uint64_t));
    if (info->struct_size < minimum_size) {
        return LOCAL_AI_STRUCT_TOO_SMALL;
    }
    refresh_memory();
    std::lock_guard<std::mutex> lock(memory_mutex_);
    info->reliable = memory_reliable_ ? 1u : 0u;
    info->current_allocated_bytes = current_allocated_bytes_;
    info->peak_allocated_bytes = peak_allocated_bytes_;
    info->device_free_bytes = device_free_bytes_;
    info->device_total_bytes = device_total_bytes_;
    return LOCAL_AI_OK;
}

} // namespace localai
