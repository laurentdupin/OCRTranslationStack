#include "inferbridge/inferbridge_harness.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string slash_path(const char* value) {
    std::string result = std::filesystem::absolute(value).generic_string();
    return result;
}

std::string escape(std::string_view value) {
    std::string result;
    for (const char byte : value) {
        if (byte == '"' || byte == '\\') result.push_back('\\');
        result.push_back(byte);
    }
    return result;
}

std::string last_error(const ibrh_api& api, const void* object) {
    size_t required = 0u;
    api.get_last_error(object, nullptr, 0u, &required);
    std::string result(required == 0u ? 1u : required, '\0');
    if (api.get_last_error(object, result.data(), result.size(), &required) != IBRH_OK)
        return "could not read harness error";
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

void run_job(const ibrh_api& api, ibrh_model* model,
             ibrh_transfer_binding input, uint64_t frame_id,
             std::string& output_json) {
    constexpr uint32_t capacity = 1024u * 1024u;
    std::vector<uint8_t> output(capacity, 0u);
    ibrh_transfer_binding destination{};
    destination.struct_size = sizeof(destination);
    destination.api_version = IBRH_CURRENT_API_VERSION;
    destination.resource.struct_size = sizeof(destination.resource);
    destination.resource.api_version = IBRH_CURRENT_API_VERSION;
    destination.resource.domain = IBRH_RESOURCE_DOMAIN_HOST;
    destination.resource.kind = IBRH_RESOURCE_KIND_BUFFER;
    destination.resource.access = IBRH_RESOURCE_ACCESS_WRITE;
    destination.resource.pixel_format = IBRH_PAYLOAD_UTF8_JSON;
    destination.resource.width = capacity;
    destination.resource.height = 1u;
    destination.resource.depth = 1u;
    destination.resource.row_stride_bytes = capacity;
    destination.resource.native_handle_type = IBRH_NATIVE_HANDLE_HOST_POINTER;
    destination.resource.byte_size = capacity;
    destination.resource.native_handle = reinterpret_cast<uintptr_t>(output.data());
    destination.synchronization.struct_size = sizeof(destination.synchronization);
    destination.synchronization.api_version = IBRH_CURRENT_API_VERSION;

    ibrh_submit_request request{};
    request.struct_size = sizeof(request);
    request.api_version = IBRH_CURRENT_API_VERSION;
    request.inputs = &input;
    request.input_count = 1u;
    request.outputs = &destination;
    request.output_count = 1u;
    request.source_frame_id = frame_id;
    ibrh_job* job = nullptr;
    require(api.submit(model, sizeof(request), &request, &job) == IBRH_OK &&
                job != nullptr,
            "submit failed");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    ibrh_job_status status{};
    for (;;) {
        require(api.job_poll(job, sizeof(status), &status) == IBRH_OK,
                "job_poll failed");
        if (status.state == IBRH_JOB_COMPLETE) break;
        if (status.state == IBRH_JOB_FAILED || status.state == IBRH_JOB_CANCELLED) {
            const std::string message = last_error(api, model);
            api.job_release(job);
            throw std::runtime_error("job failed: " + message);
        }
        require(std::chrono::steady_clock::now() < deadline, "job timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(status.source_frame_id == frame_id && status.output_count == 1u,
            "job correlation failed");
    output_json.assign(reinterpret_cast<const char*>(output.data()));
    require(output_json.rfind("{\"text\":\"", 0u) == 0u,
            "output is not the expected UTF-8 JSON object");
    api.job_release(job);
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 5, "expected DLL, OCR model, projector, and translation model");
        const std::wstring dll = std::filesystem::absolute(argv[1]).wstring();
        HMODULE module = LoadLibraryW(dll.c_str());
        require(module != nullptr, "could not load harness DLL");
        auto get_api = reinterpret_cast<ibrh_get_api_fn>(
            GetProcAddress(module, "ibrh_get_api"));
        require(get_api != nullptr, "ibrh_get_api is not exported");
        ibrh_api api{};
        require(get_api(IBRH_CURRENT_API_VERSION, sizeof(api), &api) == IBRH_OK,
                "could not obtain ABI 2 API");
        ibrh_runtime_create_request runtime_request{};
        runtime_request.struct_size = sizeof(runtime_request);
        runtime_request.api_version = IBRH_CURRENT_API_VERSION;
        const std::string backend = "VULKAN";
        runtime_request.backend = {backend.data(), backend.size()};
        ibrh_runtime* runtime = nullptr;
        require(api.runtime_create(sizeof(runtime_request), &runtime_request,
                                   &runtime) == IBRH_OK && runtime != nullptr,
                "runtime creation failed");

        const auto load = [&](const std::string& path, const std::string& parameters) {
            ibrh_model_load_request request{};
            request.struct_size = sizeof(request);
            request.api_version = IBRH_CURRENT_API_VERSION;
            request.model_path = {path.data(), path.size()};
            request.parameters_json = {parameters.data(), parameters.size()};
            ibrh_model* model = nullptr;
            const ibrh_result result = api.model_load(
                runtime, sizeof(request), &request, &model);
            if (result != IBRH_OK)
                throw std::runtime_error("model load failed: " + last_error(api, runtime));
            return model;
        };

        const std::string ocr_path = slash_path(argv[2]);
        const std::string projector = slash_path(argv[3]);
        ibrh_model* ocr = load(
            ocr_path, "{\"task\":\"ocr\",\"vision_projector_path\":\"" +
                          escape(projector) + "\",\"source_language\":\"English\"}");
        constexpr uint32_t width = 192u;
        constexpr uint32_t height = 64u;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u, 255u);
        for (uint32_t y = 16u; y < 48u; ++y) {
            for (uint32_t x = 24u; x < 168u; x += 12u) {
                for (uint32_t stroke = 0u; stroke < 4u; ++stroke) {
                    uint8_t* pixel = pixels.data() +
                        (static_cast<size_t>(y) * width + x + stroke) * 4u;
                    pixel[0] = pixel[1] = pixel[2] = 0u;
                }
            }
        }
        ibrh_transfer_binding image{};
        image.struct_size = sizeof(image);
        image.api_version = IBRH_CURRENT_API_VERSION;
        image.resource.struct_size = sizeof(image.resource);
        image.resource.api_version = IBRH_CURRENT_API_VERSION;
        image.resource.domain = IBRH_RESOURCE_DOMAIN_HOST;
        image.resource.kind = IBRH_RESOURCE_KIND_IMAGE_2D;
        image.resource.access = IBRH_RESOURCE_ACCESS_READ;
        image.resource.pixel_format = IBRH_PIXEL_BGRA8;
        image.resource.width = width;
        image.resource.height = height;
        image.resource.depth = 1u;
        image.resource.row_stride_bytes = width * 4u;
        image.resource.native_handle_type = IBRH_NATIVE_HANDLE_HOST_POINTER;
        image.resource.byte_size = pixels.size();
        image.resource.native_handle = reinterpret_cast<uintptr_t>(pixels.data());
        image.synchronization.struct_size = sizeof(image.synchronization);
        image.synchronization.api_version = IBRH_CURRENT_API_VERSION;
        std::string ocr_json;
        run_job(api, ocr, image, 7001u, ocr_json);
        api.model_unload(ocr);

        const std::string translation_path = slash_path(argv[4]);
        ibrh_model* translation = load(
            translation_path,
            "{\"task\":\"translation\",\"source_language\":\"English\","
            "\"target_language\":\"French\"}");
        const std::string request_json =
            "{\"text\":\"Hello world.\",\"source_language\":\"English\","
            "\"target_language\":\"French\"}";
        ibrh_transfer_binding text{};
        text.struct_size = sizeof(text);
        text.api_version = IBRH_CURRENT_API_VERSION;
        text.resource.struct_size = sizeof(text.resource);
        text.resource.api_version = IBRH_CURRENT_API_VERSION;
        text.resource.domain = IBRH_RESOURCE_DOMAIN_HOST;
        text.resource.kind = IBRH_RESOURCE_KIND_BUFFER;
        text.resource.access = IBRH_RESOURCE_ACCESS_READ;
        text.resource.pixel_format = IBRH_PAYLOAD_UTF8_JSON;
        text.resource.width = static_cast<uint32_t>(request_json.size());
        text.resource.height = 1u;
        text.resource.depth = 1u;
        text.resource.row_stride_bytes = static_cast<uint32_t>(request_json.size());
        text.resource.native_handle_type = IBRH_NATIVE_HANDLE_HOST_POINTER;
        text.resource.byte_size = request_json.size();
        text.resource.native_handle = reinterpret_cast<uintptr_t>(request_json.data());
        text.synchronization.struct_size = sizeof(text.synchronization);
        text.synchronization.api_version = IBRH_CURRENT_API_VERSION;
        std::string translation_json;
        run_job(api, translation, text, 7002u, translation_json);
        api.model_unload(translation);
        api.runtime_destroy(runtime);
        FreeLibrary(module);
        std::cout << "INFERBRIDGE_OCR_TRANSLATION_OK\n"
                  << "OCR=" << ocr_json << "\n"
                  << "TRANSLATION=" << translation_json << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "InferBridge OCR/translation smoke failed: "
                  << error.what() << '\n';
        return 1;
    }
}
