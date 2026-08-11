#include "inferbridge/inferbridge_harness.h"

#include "local_ai_engine.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

constexpr char kHarnessId[] = "inferbridge.local-ai-text";
constexpr char kHarnessVersion[] = "2.0.0";
constexpr uint32_t kDefaultOutputCapacity = 1024u * 1024u;
constexpr uint32_t kMaximumOutputCapacity = 16u * 1024u * 1024u;

std::mutex g_error_mutex;
std::string g_last_error;

void set_error(std::string message) {
    std::lock_guard<std::mutex> guard(g_error_mutex);
    g_last_error = std::move(message);
}

std::string copy_error() {
    std::lock_guard<std::mutex> guard(g_error_mutex);
    return g_last_error;
}

std::string_view view(ibrh_string_view value) {
    return value.data == nullptr ? std::string_view{} :
        std::string_view(value.data, value.size);
}

void append_utf8(std::string& output, uint32_t codepoint) {
    if (codepoint <= 0x7fu) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parse_json_string_at(std::string_view json, size_t& position,
                          std::string& output) {
    if (position >= json.size() || json[position] != '"') return false;
    ++position;
    output.clear();
    while (position < json.size()) {
        const char value = json[position++];
        if (value == '"') return true;
        if (value != '\\') {
            if (static_cast<unsigned char>(value) < 0x20u) return false;
            output.push_back(value);
            continue;
        }
        if (position >= json.size()) return false;
        const char escaped = json[position++];
        switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (position + 4u > json.size()) return false;
                uint32_t codepoint = 0u;
                for (uint32_t index = 0u; index < 4u; ++index) {
                    const int digit = hex_digit(json[position++]);
                    if (digit < 0) return false;
                    codepoint = (codepoint << 4u) | static_cast<uint32_t>(digit);
                }
                if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                    if (position + 6u > json.size() || json[position] != '\\' ||
                        json[position + 1u] != 'u') return false;
                    position += 2u;
                    uint32_t low = 0u;
                    for (uint32_t index = 0u; index < 4u; ++index) {
                        const int digit = hex_digit(json[position++]);
                        if (digit < 0) return false;
                        low = (low << 4u) | static_cast<uint32_t>(digit);
                    }
                    if (low < 0xdc00u || low > 0xdfffu) return false;
                    codepoint = 0x10000u + ((codepoint - 0xd800u) << 10u) +
                        (low - 0xdc00u);
                } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
                    return false;
                }
                append_utf8(output, codepoint);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool json_string(std::string_view json, std::string_view key,
                 std::string& output) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t position = json.find(needle);
    if (position == std::string_view::npos) return false;
    position += needle.size();
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) ++position;
    if (position >= json.size() || json[position++] != ':') return false;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) ++position;
    return parse_json_string_at(json, position, output);
}

uint32_t json_uint(std::string_view json, std::string_view key,
                   uint32_t fallback) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t position = json.find(needle);
    if (position == std::string_view::npos) return fallback;
    position = json.find(':', position + needle.size());
    if (position == std::string_view::npos) return fallback;
    ++position;
    while (position < json.size() && json[position] == ' ') ++position;
    uint64_t value = 0u;
    bool found = false;
    while (position < json.size() && json[position] >= '0' &&
           json[position] <= '9') {
        found = true;
        value = value * 10u + static_cast<uint32_t>(json[position++] - '0');
        if (value > std::numeric_limits<uint32_t>::max()) return fallback;
    }
    return found ? static_cast<uint32_t>(value) : fallback;
}

std::string json_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() + 32u);
    for (const unsigned char byte : value) {
        switch (byte) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20u) {
                    output += "\\u00";
                    output.push_back(hex[byte >> 4u]);
                    output.push_back(hex[byte & 0x0fu]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
        }
    }
    return output;
}

std::wstring utf8_to_wide(std::string_view value) {
#if defined(_WIN32)
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring output(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(),
                            required) != required) return {};
    return output;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

std::string engine_error(localai::Engine& engine) {
    const wchar_t* wide = engine.last_error();
    if (wide == nullptr || *wide == L'\0') return "native model operation failed";
#if defined(_WIN32)
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return "native model operation failed";
    std::string output(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                        output.data(), required, nullptr, nullptr);
    output.resize(static_cast<size_t>(required - 1));
    return output;
#else
    std::string output;
    while (*wide != L'\0') output.push_back(static_cast<char>(*wide++));
    return output;
#endif
}

ibrh_result map_status(LocalAIStatus status) {
    switch (status) {
        case LOCAL_AI_OK: return IBRH_OK;
        case LOCAL_AI_INVALID_ARGUMENT: return IBRH_ERROR_INVALID_ARGUMENT;
        case LOCAL_AI_FILE_NOT_FOUND: return IBRH_ERROR_NOT_FOUND;
        case LOCAL_AI_CANCELLED: return IBRH_ERROR_CANCELLED;
        case LOCAL_AI_BUSY: return IBRH_ERROR_INVALID_STATE;
        case LOCAL_AI_VULKAN_ERROR: return IBRH_ERROR_DEVICE_LOST;
        default: return IBRH_ERROR_INTERNAL;
    }
}

enum class Task { Ocr, Translation };

struct JobState {
    std::atomic<uint32_t> state{IBRH_JOB_QUEUED};
    std::atomic_bool cancel{false};
    uint64_t source_frame_id = 0u;
    uint64_t timestamp_ns = 0u;
    ibrh_transfer_binding input{};
    ibrh_transfer_binding output{};
    std::string parameters;
    std::string error;
};

} // namespace

struct ibrh_runtime {
    std::string requested_device_json;
};

struct ibrh_model {
    Task task = Task::Ocr;
    std::unique_ptr<localai::Engine> engine;
    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    std::deque<std::shared_ptr<JobState>> queue;
    std::thread worker;
    bool stopping = false;
    std::string default_source_language = "Auto";
    std::string default_target_language;

    void start_worker();
    void stop_worker();
    void run(const std::shared_ptr<JobState>& job);
};

struct ibrh_job {
    ibrh_model* model = nullptr;
    std::shared_ptr<JobState> state;
};

namespace {

void capture_text(const char* text, size_t size, void* user_data) {
    auto* output = static_cast<std::string*>(user_data);
    if (output != nullptr && text != nullptr) output->assign(text, size);
}

void write_job_output(const std::shared_ptr<JobState>& job,
                      std::string_view text) {
    std::string json = "{\"text\":\"" + json_escape(text) +
        "\",\"sourceFrameId\":" + std::to_string(job->source_frame_id) + "}";
    const auto& resource = job->output.resource;
    if (resource.domain != IBRH_RESOURCE_DOMAIN_HOST ||
        resource.kind != IBRH_RESOURCE_KIND_BUFFER ||
        resource.native_handle_type != IBRH_NATIVE_HANDLE_HOST_POINTER ||
        resource.native_handle == 0u || resource.byte_size == 0u)
        throw std::runtime_error("Core-owned text output is not writable host memory");
    if (json.size() + 1u > resource.byte_size)
        throw std::runtime_error("generated text exceeds the planned output capacity");
    auto* destination = reinterpret_cast<char*>(
        static_cast<uintptr_t>(resource.native_handle));
    std::memcpy(destination, json.data(), json.size());
    destination[json.size()] = '\0';
}

bool valid_text_output(const ibrh_transfer_binding& binding) {
    return binding.struct_size >= sizeof(binding) &&
        binding.api_version == IBRH_CURRENT_API_VERSION &&
        binding.resource.struct_size >= sizeof(binding.resource) &&
        binding.resource.api_version == IBRH_CURRENT_API_VERSION &&
        binding.resource.domain == IBRH_RESOURCE_DOMAIN_HOST &&
        binding.resource.kind == IBRH_RESOURCE_KIND_BUFFER &&
        binding.resource.access == IBRH_RESOURCE_ACCESS_WRITE &&
        binding.resource.pixel_format == IBRH_PAYLOAD_UTF8_JSON &&
        binding.synchronization.kind == IBRH_SYNC_NONE;
}

} // namespace

void ibrh_model::start_worker() {
    worker = std::thread([this] {
        for (;;) {
            std::shared_ptr<JobState> job;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_condition.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            if (job->cancel.load(std::memory_order_acquire)) {
                job->state.store(IBRH_JOB_CANCELLED, std::memory_order_release);
                continue;
            }
            job->state.store(IBRH_JOB_RUNNING, std::memory_order_release);
            run(job);
        }
    });
}

void ibrh_model::stop_worker() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        stopping = true;
        for (const auto& job : queue) job->cancel.store(true);
    }
    if (engine) engine->cancel();
    queue_condition.notify_all();
    if (worker.joinable()) worker.join();
}

void ibrh_model::run(const std::shared_ptr<JobState>& job) {
    try {
        std::string result;
        LocalAIStatus status = LOCAL_AI_INFERENCE_FAILED;
        if (task == Task::Ocr) {
            const auto& resource = job->input.resource;
            if (resource.domain != IBRH_RESOURCE_DOMAIN_HOST ||
                resource.kind != IBRH_RESOURCE_KIND_IMAGE_2D ||
                resource.native_handle_type != IBRH_NATIVE_HANDLE_HOST_POINTER ||
                resource.native_handle == 0u ||
                (resource.pixel_format != IBRH_PIXEL_BGRA8 &&
                 resource.pixel_format != IBRH_PIXEL_RGBA8))
                throw std::runtime_error("OCR requires a host BGRA8 or RGBA8 image");
            std::string source = default_source_language;
            json_string(job->parameters, "source_language", source);
            status = engine->ocr_pixels(
                reinterpret_cast<const uint8_t*>(
                    static_cast<uintptr_t>(resource.native_handle)),
                resource.width, resource.height, resource.row_stride_bytes,
                resource.pixel_format == IBRH_PIXEL_BGRA8, source.c_str(),
                capture_text, &result);
        } else {
            const auto& resource = job->input.resource;
            if (resource.domain != IBRH_RESOURCE_DOMAIN_HOST ||
                resource.kind != IBRH_RESOURCE_KIND_BUFFER ||
                resource.native_handle_type != IBRH_NATIVE_HANDLE_HOST_POINTER ||
                resource.native_handle == 0u ||
                resource.pixel_format != IBRH_PAYLOAD_UTF8_JSON)
                throw std::runtime_error("translation requires a host UTF-8 JSON buffer");
            const std::string_view request(
                reinterpret_cast<const char*>(
                    static_cast<uintptr_t>(resource.native_handle)),
                static_cast<size_t>(resource.byte_size));
            std::string text;
            if (!json_string(request, "text", text) || text.empty())
                throw std::runtime_error("translation JSON requires a non-empty text field");
            std::string source = default_source_language;
            std::string target = default_target_language;
            json_string(request, "source_language", source);
            json_string(request, "target_language", target);
            json_string(job->parameters, "source_language", source);
            json_string(job->parameters, "target_language", target);
            if (target.empty())
                throw std::runtime_error("translation requires target_language");
            status = engine->translate(text.c_str(), source.c_str(), target.c_str(),
                                       capture_text, &result);
        }
        if (job->cancel.load(std::memory_order_acquire) ||
            status == LOCAL_AI_CANCELLED) {
            job->state.store(IBRH_JOB_CANCELLED, std::memory_order_release);
            return;
        }
        if (status != LOCAL_AI_OK) {
            job->error = engine_error(*engine);
            set_error(job->error);
            job->state.store(IBRH_JOB_FAILED, std::memory_order_release);
            return;
        }
        write_job_output(job, result);
        job->state.store(IBRH_JOB_COMPLETE, std::memory_order_release);
    } catch (const std::exception& error) {
        job->error = error.what();
        set_error(job->error);
        job->state.store(IBRH_JOB_FAILED, std::memory_order_release);
    } catch (...) {
        job->error = "unknown native model failure";
        set_error(job->error);
        job->state.store(IBRH_JOB_FAILED, std::memory_order_release);
    }
}

namespace {

ibrh_result IBRH_CALL query_capabilities(size_t size,
                                         ibrh_capabilities* output) {
    if (output == nullptr) return IBRH_ERROR_INVALID_ARGUMENT;
    if (size < sizeof(*output)) return IBRH_ERROR_STRUCT_TOO_SMALL;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = IBRH_CURRENT_API_VERSION;
    output->flags = IBRH_CAP_ASYNC_SUBMIT | IBRH_CAP_CANCELLATION |
        IBRH_CAP_HOST_MEMORY;
    output->input_domain_mask = 1ull << IBRH_RESOURCE_DOMAIN_HOST;
    output->output_domain_mask = 1ull << IBRH_RESOURCE_DOMAIN_HOST;
    output->maximum_inputs = 1u;
    output->maximum_outputs = 1u;
    output->maximum_in_flight_jobs = 2u;
    output->harness_id = {kHarnessId, sizeof(kHarnessId) - 1u};
    output->harness_version = {kHarnessVersion, sizeof(kHarnessVersion) - 1u};
    return IBRH_OK;
}

ibrh_result IBRH_CALL runtime_create(
    size_t size, const ibrh_runtime_create_request* request,
    ibrh_runtime** output) {
    if (request == nullptr || output == nullptr) return IBRH_ERROR_INVALID_ARGUMENT;
    *output = nullptr;
    if (size < sizeof(*request) || request->struct_size < sizeof(*request))
        return IBRH_ERROR_STRUCT_TOO_SMALL;
    try {
        auto runtime = std::make_unique<ibrh_runtime>();
        runtime->requested_device_json.assign(view(request->requested_device_json));
        *output = runtime.release();
        return IBRH_OK;
    } catch (...) {
        return IBRH_ERROR_INTERNAL;
    }
}

void IBRH_CALL runtime_destroy(ibrh_runtime* runtime) { delete runtime; }

ibrh_result IBRH_CALL model_load(
    ibrh_runtime* runtime, size_t size, const ibrh_model_load_request* request,
    ibrh_model** output) {
    if (runtime == nullptr || request == nullptr || output == nullptr)
        return IBRH_ERROR_INVALID_ARGUMENT;
    *output = nullptr;
    if (size < sizeof(*request) || request->struct_size < sizeof(*request))
        return IBRH_ERROR_STRUCT_TOO_SMALL;
    try {
        const std::string model_path(view(request->model_path));
        const std::string parameters(view(request->parameters_json));
        std::string task;
        if (!json_string(parameters, "task", task))
            throw std::runtime_error("model parameters require task=ocr or translation");
        const std::wstring wide_model = utf8_to_wide(model_path);
        if (wide_model.empty()) throw std::runtime_error("model path is not valid UTF-8");
        auto model = std::make_unique<ibrh_model>();
        LocalAIConfig config{};
        config.struct_size = sizeof(config);
        config.gpu_layers = -1;
        config.maximum_vram_bytes = 6ull * 1024ull * 1024ull * 1024ull;
        config.flags = LOCAL_AI_CONFIG_SEQUENTIAL_MODELS;
        std::wstring projector;
        if (task == "ocr") {
            std::string projector_path;
            if (!json_string(parameters, "vision_projector_path", projector_path))
                throw std::runtime_error("OvisOCR2 requires vision_projector_path");
            projector = utf8_to_wide(projector_path);
            if (projector.empty())
                throw std::runtime_error("vision projector path is not valid UTF-8");
            config.ocr_model_path = wide_model.c_str();
            config.vision_projector_path = projector.c_str();
            model->task = Task::Ocr;
        } else if (task == "translation") {
            config.translation_model_path = wide_model.c_str();
            model->task = Task::Translation;
        } else {
            throw std::runtime_error("unsupported local AI task");
        }
        json_string(parameters, "source_language", model->default_source_language);
        json_string(parameters, "target_language", model->default_target_language);
        model->engine = std::make_unique<localai::Engine>(config);
        const LocalAIStatus load_status = model->task == Task::Ocr
            ? model->engine->load_ocr() : model->engine->load_translation();
        if (load_status != LOCAL_AI_OK) {
            set_error(engine_error(*model->engine));
            return map_status(load_status);
        }
        model->start_worker();
        *output = model.release();
        return IBRH_OK;
    } catch (const std::exception& error) {
        set_error(error.what());
        return IBRH_ERROR_INVALID_ARGUMENT;
    } catch (...) {
        set_error("unknown model load failure");
        return IBRH_ERROR_INTERNAL;
    }
}

void IBRH_CALL model_unload(ibrh_model* model) {
    if (model == nullptr) return;
    model->stop_worker();
    delete model;
}

ibrh_result IBRH_CALL model_describe_io(
    const ibrh_model* model, size_t size, ibrh_model_io_descriptor* output) {
    if (model == nullptr || output == nullptr) return IBRH_ERROR_INVALID_ARGUMENT;
    if (size < sizeof(*output)) return IBRH_ERROR_STRUCT_TOO_SMALL;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = IBRH_CURRENT_API_VERSION;
    output->input_count = 1u;
    output->output_count = 1u;
    return IBRH_OK;
}

ibrh_result IBRH_CALL model_get_port(
    const ibrh_model* model, uint32_t direction, uint32_t index, size_t size,
    ibrh_port_descriptor* output) {
    if (model == nullptr || output == nullptr || index != 0u)
        return IBRH_ERROR_INVALID_ARGUMENT;
    if (size < sizeof(*output)) return IBRH_ERROR_STRUCT_TOO_SMALL;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = IBRH_CURRENT_API_VERSION;
    output->index = index;
    output->direction = direction;
    output->depth = 1u;
    if (direction == IBRH_PORT_INPUT && model->task == Task::Ocr) {
        output->semantic = IBRH_SEMANTIC_IMAGE;
        output->payload_type = IBRH_PIXEL_BGRA8;
        output->pixel_format = IBRH_PIXEL_BGRA8;
        output->accepted_pixel_format_mask =
            (1ull << IBRH_PIXEL_BGRA8) | (1ull << IBRH_PIXEL_RGBA8);
        output->resource_kind = IBRH_RESOURCE_KIND_IMAGE_2D;
        output->flags = IBRH_DESCRIPTOR_DYNAMIC_WIDTH |
            IBRH_DESCRIPTOR_DYNAMIC_HEIGHT;
    } else if (direction == IBRH_PORT_INPUT &&
               model->task == Task::Translation) {
        output->semantic = IBRH_SEMANTIC_TEXT;
        output->payload_type = IBRH_PAYLOAD_UTF8_JSON;
        output->pixel_format = IBRH_PAYLOAD_UTF8_JSON;
        output->accepted_pixel_format_mask = 1ull << IBRH_PAYLOAD_UTF8_JSON;
        output->resource_kind = IBRH_RESOURCE_KIND_BUFFER;
        output->flags = IBRH_DESCRIPTOR_DYNAMIC_WIDTH;
        output->height = 1u;
    } else if (direction == IBRH_PORT_OUTPUT) {
        output->semantic = IBRH_SEMANTIC_TEXT;
        output->payload_type = IBRH_PAYLOAD_UTF8_JSON;
        output->pixel_format = IBRH_PAYLOAD_UTF8_JSON;
        output->accepted_pixel_format_mask = 1ull << IBRH_PAYLOAD_UTF8_JSON;
        output->resource_kind = IBRH_RESOURCE_KIND_BUFFER;
        output->flags = IBRH_DESCRIPTOR_DYNAMIC_WIDTH;
        output->height = 1u;
    } else {
        return IBRH_ERROR_INVALID_ARGUMENT;
    }
    return IBRH_OK;
}

ibrh_result IBRH_CALL model_plan_outputs(
    const ibrh_model* model, size_t size,
    const ibrh_output_plan_request* request, uint32_t capacity,
    ibrh_port_descriptor* outputs) {
    if (model == nullptr || request == nullptr || outputs == nullptr ||
        capacity < 1u) return IBRH_ERROR_INVALID_ARGUMENT;
    if (size < sizeof(*request) || request->struct_size < sizeof(*request))
        return IBRH_ERROR_STRUCT_TOO_SMALL;
    const ibrh_result result = model_get_port(
        model, IBRH_PORT_OUTPUT, 0u, sizeof(outputs[0]), &outputs[0]);
    if (result != IBRH_OK) return result;
    uint32_t bytes = json_uint(view(request->parameters_json),
                               "maximum_output_bytes", kDefaultOutputCapacity);
    bytes = std::clamp(bytes, 256u, kMaximumOutputCapacity);
    outputs[0].width = bytes;
    outputs[0].height = 1u;
    outputs[0].depth = 1u;
    outputs[0].flags = 0u;
    return IBRH_OK;
}

ibrh_result IBRH_CALL submit(
    ibrh_model* model, size_t size, const ibrh_submit_request* request,
    ibrh_job** output) {
    if (model == nullptr || request == nullptr || output == nullptr)
        return IBRH_ERROR_INVALID_ARGUMENT;
    *output = nullptr;
    if (size < sizeof(*request) || request->struct_size < sizeof(*request))
        return IBRH_ERROR_STRUCT_TOO_SMALL;
    if (request->input_count != 1u || request->output_count != 1u ||
        request->inputs == nullptr || request->outputs == nullptr ||
        !valid_text_output(request->outputs[0]))
        return IBRH_ERROR_INVALID_ARGUMENT;
    try {
        auto state = std::make_shared<JobState>();
        state->source_frame_id = request->source_frame_id;
        state->timestamp_ns = request->timestamp_ns;
        state->input = request->inputs[0];
        state->output = request->outputs[0];
        state->parameters.assign(view(request->parameters_json));
        auto job = std::make_unique<ibrh_job>();
        job->model = model;
        job->state = state;
        {
            std::lock_guard<std::mutex> lock(model->queue_mutex);
            if (model->stopping || !model->queue.empty())
                return IBRH_ERROR_INVALID_STATE;
            model->queue.push_back(state);
        }
        model->queue_condition.notify_one();
        *output = job.release();
        return IBRH_OK;
    } catch (...) {
        return IBRH_ERROR_INTERNAL;
    }
}

ibrh_result IBRH_CALL job_poll(
    const ibrh_job* job, size_t size, ibrh_job_status* output) {
    if (job == nullptr || output == nullptr) return IBRH_ERROR_INVALID_ARGUMENT;
    if (size < sizeof(*output)) return IBRH_ERROR_STRUCT_TOO_SMALL;
    *output = {};
    output->struct_size = sizeof(*output);
    output->state = job->state->state.load(std::memory_order_acquire);
    output->output_count = output->state == IBRH_JOB_COMPLETE ? 1u : 0u;
    output->source_frame_id = job->state->source_frame_id;
    return IBRH_OK;
}

ibrh_result IBRH_CALL job_cancel(ibrh_job* job) {
    if (job == nullptr) return IBRH_ERROR_INVALID_ARGUMENT;
    const uint32_t state = job->state->state.load(std::memory_order_acquire);
    if (state == IBRH_JOB_COMPLETE || state == IBRH_JOB_FAILED ||
        state == IBRH_JOB_CANCELLED) return IBRH_ERROR_INVALID_STATE;
    job->state->cancel.store(true, std::memory_order_release);
    if (state == IBRH_JOB_RUNNING && job->model != nullptr &&
        job->model->engine)
        job->model->engine->cancel();
    return IBRH_OK;
}

void IBRH_CALL job_release(ibrh_job* job) { delete job; }

ibrh_result IBRH_CALL get_last_error(
    const void*, char* destination, size_t destination_size,
    size_t* required_size) {
    const std::string message = copy_error();
    const size_t required = message.size() + 1u;
    if (required_size != nullptr) *required_size = required;
    if (destination == nullptr || destination_size < required)
        return IBRH_ERROR_STRUCT_TOO_SMALL;
    std::memcpy(destination, message.c_str(), required);
    return IBRH_OK;
}

} // namespace

extern "C" IBRH_API ibrh_result IBRH_CALL ibrh_get_api(
    uint32_t requested_version, size_t size, ibrh_api* output) {
    if (output == nullptr) return IBRH_ERROR_INVALID_ARGUMENT;
    if (size < sizeof(*output)) return IBRH_ERROR_STRUCT_TOO_SMALL;
    if ((requested_version >> 16u) != IBRH_API_VERSION_MAJOR)
        return IBRH_ERROR_UNSUPPORTED_API;
    *output = {};
    output->struct_size = sizeof(*output);
    output->api_version = IBRH_CURRENT_API_VERSION;
    output->query_capabilities = query_capabilities;
    output->runtime_create = runtime_create;
    output->runtime_destroy = runtime_destroy;
    output->model_load = model_load;
    output->model_unload = model_unload;
    output->model_describe_io = model_describe_io;
    output->model_get_port = model_get_port;
    output->model_plan_outputs = model_plan_outputs;
    output->submit = submit;
    output->job_poll = job_poll;
    output->job_cancel = job_cancel;
    output->job_release = job_release;
    output->get_last_error = get_last_error;
    return IBRH_OK;
}
