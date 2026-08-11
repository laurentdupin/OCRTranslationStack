#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "local_ai.h"

#include <windows.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"LocalAIAppWindow";
constexpr wchar_t kPreviewClass[] = L"LocalAIAppPreview";
constexpr wchar_t kRegistryPath[] = L"Software\\LocalAIPrototype";

constexpr UINT kOpenImage = 100;
constexpr UINT kSourceLanguage = 101;
constexpr UINT kTargetLanguage = 102;
constexpr UINT kRunOcr = 103;
constexpr UINT kTranslate = 104;
constexpr UINT kCancel = 105;
constexpr UINT kStatus = 106;
constexpr UINT kPreview = 107;
constexpr UINT kExtractedLabel = 108;
constexpr UINT kTranslationLabel = 109;
constexpr UINT kExtractedEdit = 110;
constexpr UINT kTranslationEdit = 111;
constexpr UINT kCopyExtracted = 112;
constexpr UINT kSaveExtracted = 113;
constexpr UINT kCopyTranslation = 114;
constexpr UINT kSaveTranslation = 115;
constexpr UINT kFurigana = 116;

constexpr UINT kDoneMessage = WM_APP + 1;

struct Language {
    const wchar_t * display;
    const char * prompt_name;
};

constexpr Language kSourceLanguages[] = {
    {L"Auto detect", "Auto"},
    {L"English", "English"},
    {L"French", "French"},
    {L"German", "German"},
    {L"Spanish", "Spanish"},
    {L"Italian", "Italian"},
    {L"Portuguese", "Portuguese"},
    {L"Chinese", "Chinese"},
    {L"Japanese", "Japanese"},
    {L"Korean", "Korean"},
    {L"Russian", "Russian"},
    {L"Arabic", "Arabic"},
};

constexpr Language kTargetLanguages[] = {
    {L"English", "English"},
    {L"French", "French"},
    {L"German", "German"},
    {L"Spanish", "Spanish"},
    {L"Italian", "Italian"},
    {L"Portuguese", "Portuguese"},
    {L"Chinese", "Chinese"},
    {L"Japanese", "Japanese"},
    {L"Korean", "Korean"},
    {L"Russian", "Russian"},
    {L"Arabic", "Arabic"},
};

enum class Operation {
    Ocr,
    Furigana,
    Translation,
};

struct DisplayFuriganaToken {
    std::wstring surface;
    std::wstring reading;
    LocalAIQuad image_quad{};
    float confidence = 0.0f;
};

struct DisplayTextRegion {
    std::wstring text;
    LocalAIQuad image_quad{};
    float confidence = 0.0f;
    uint32_t flags = 0;
};

struct WorkerResult {
    Operation operation = Operation::Ocr;
    LocalAIStatus status = LOCAL_AI_INFERENCE_FAILED;
    std::string utf8_text;
    std::wstring error;
    LocalAIMemoryInfo memory{};
    std::vector<DisplayFuriganaToken> furigana_tokens;
    std::vector<DisplayTextRegion> text_regions;
};

struct PreviewState {
    ComPtr<IWICBitmapSource> source;
    ComPtr<ID2D1HwndRenderTarget> render_target;
    ComPtr<ID2D1Bitmap> bitmap;
    UINT width = 0;
    UINT height = 0;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND open_button = nullptr;
    HWND source_combo = nullptr;
    HWND target_combo = nullptr;
    HWND ocr_button = nullptr;
    HWND furigana_button = nullptr;
    HWND translate_button = nullptr;
    HWND cancel_button = nullptr;
    HWND status = nullptr;
    HWND preview_window = nullptr;
    HWND extracted_label = nullptr;
    HWND translation_label = nullptr;
    HWND extracted_edit = nullptr;
    HWND translation_edit = nullptr;
    HWND copy_extracted = nullptr;
    HWND save_extracted = nullptr;
    HWND copy_translation = nullptr;
    HWND save_translation = nullptr;
    HFONT font = nullptr;
    ComPtr<IWICImagingFactory> wic_factory;
    ComPtr<ID2D1Factory> d2d_factory;
    ComPtr<IDWriteFactory> write_factory;
    PreviewState preview;
    LocalAIEngine * engine = nullptr;
    std::wstring image_path;
    std::vector<DisplayFuriganaToken> furigana_tokens;
    std::vector<DisplayTextRegion> text_regions;
    std::thread worker;
    bool active = false;
    std::atomic_bool closing{false};
    int saved_width = 1200;
    int saved_height = 760;
};

void set_status(AppState & state, const std::wstring & text) {
    if (state.status != nullptr) {
        SetWindowTextW(state.status, text.c_str());
    }
}

std::wstring utf8_to_wide(const char * text, size_t byte_count) {
    if (text == nullptr || byte_count == 0) {
        return {};
    }
    if (byte_count > static_cast<size_t>(INT_MAX)) {
        return L"[text is too large to display]";
    }
    int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text,
        static_cast<int>(byte_count),
        nullptr,
        0);
    if (required <= 0) {
        required = MultiByteToWideChar(
            CP_UTF8,
            0,
            text,
            static_cast<int>(byte_count),
            nullptr,
            0);
    }
    if (required <= 0) {
        return L"[invalid UTF-8 output]";
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            0,
            text,
            static_cast<int>(byte_count),
            result.data(),
            required) != required) {
        return L"[invalid UTF-8 output]";
    }
    return result;
}

std::wstring utf8_to_wide(const std::string & text) {
    return utf8_to_wide(text.data(), text.size());
}

std::string wide_to_utf8(const std::wstring & text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<size_t>(INT_MAX)) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            required,
            nullptr,
            nullptr) != required) {
        return {};
    }
    return result;
}

std::wstring format_bytes(uint64_t bytes) {
    const double value = static_cast<double>(bytes);
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(value >= 1024.0 * 1024.0 * 1024.0 ? 2 : 1);
    if (value >= 1024.0 * 1024.0 * 1024.0) {
        stream << value / (1024.0 * 1024.0 * 1024.0) << L" GiB";
    } else if (value >= 1024.0 * 1024.0) {
        stream << value / (1024.0 * 1024.0) << L" MiB";
    } else {
        stream << value / 1024.0 << L" KiB";
    }
    return stream.str();
}

std::wstring get_executable_directory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
        return std::filesystem::current_path().wstring();
    }
    path.resize(length);
    return std::filesystem::path(path).parent_path().wstring();
}

std::filesystem::path choose_model_file(
    const std::filesystem::path & root,
    std::initializer_list<const wchar_t *> names) {
    for (const wchar_t * name : names) {
        const auto candidate = root / name;
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return root / *names.begin();
}

std::wstring environment_value(const wchar_t * name) {
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

std::vector<std::filesystem::path> model_roots() {
    std::vector<std::filesystem::path> roots;
    const auto add_unique = [&roots](const std::filesystem::path & root) {
        if (root.empty()) {
            return;
        }
        const auto normalized = root.lexically_normal();
        if (std::find(roots.begin(), roots.end(), normalized) == roots.end()) {
            roots.push_back(normalized);
        }
    };

    const std::wstring configured_root = environment_value(L"LOCALAI_MODELS_DIR");
    if (!configured_root.empty()) {
        add_unique(std::filesystem::path(configured_root));
    }

    std::filesystem::path directory(get_executable_directory());
    for (size_t level = 0; level < 8 && !directory.empty(); ++level) {
        add_unique(directory / L"models");
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    std::error_code error;
    add_unique(std::filesystem::current_path(error) / L"models");
    return roots;
}

bool read_dword(HKEY key, const wchar_t * name, DWORD & value) {
    DWORD type = 0;
    DWORD size = sizeof(value);
    return RegQueryValueExW(
               key,
               name,
               nullptr,
               &type,
               reinterpret_cast<BYTE *>(&value),
               &size) == ERROR_SUCCESS &&
           type == REG_DWORD && size == sizeof(value);
}

void load_settings(AppState & state) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }
    DWORD source = 0;
    DWORD target = 1;
    DWORD width = static_cast<DWORD>(state.saved_width);
    DWORD height = static_cast<DWORD>(state.saved_height);
    read_dword(key, L"SourceLanguage", source);
    read_dword(key, L"TargetLanguage", target);
    read_dword(key, L"WindowWidth", width);
    read_dword(key, L"WindowHeight", height);
    RegCloseKey(key);
    if (source < std::size(kSourceLanguages)) {
        SendMessageW(state.source_combo, CB_SETCURSEL, source, 0);
    }
    if (target < std::size(kTargetLanguages)) {
        SendMessageW(state.target_combo, CB_SETCURSEL, target, 0);
    }
    state.saved_width = std::clamp(static_cast<int>(width), 900, 2400);
    state.saved_height = std::clamp(static_cast<int>(height), 600, 1600);
}

void save_settings(const AppState & state) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRegistryPath,
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &key,
            &disposition) != ERROR_SUCCESS) {
        return;
    }
    const DWORD source = static_cast<DWORD>(std::max<LRESULT>(
        0, SendMessageW(state.source_combo, CB_GETCURSEL, 0, 0)));
    const DWORD target = static_cast<DWORD>(std::max<LRESULT>(
        0, SendMessageW(state.target_combo, CB_GETCURSEL, 0, 0)));
    RECT rect{};
    GetWindowRect(state.window, &rect);
    const DWORD width = static_cast<DWORD>(std::max<LONG>(900, rect.right - rect.left));
    const DWORD height = static_cast<DWORD>(std::max<LONG>(600, rect.bottom - rect.top));
    RegSetValueExW(key, L"SourceLanguage", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&source), sizeof(source));
    RegSetValueExW(key, L"TargetLanguage", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&target), sizeof(target));
    RegSetValueExW(key, L"WindowWidth", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&width), sizeof(width));
    RegSetValueExW(key, L"WindowHeight", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&height), sizeof(height));
    RegCloseKey(key);
}

std::wstring edit_text(HWND edit) {
    if (edit == nullptr) {
        return {};
    }
    const int length = GetWindowTextLengthW(edit);
    std::wstring result(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(edit, result.data(), length + 1);
    }
    result.resize(static_cast<size_t>(std::max(0, length)));
    return result;
}

void apply_font(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void callback_text(const char * utf8_text, size_t byte_count, void * user_data) {
    auto * result = static_cast<WorkerResult *>(user_data);
    if (result == nullptr) {
        return;
    }
    try {
        result->utf8_text.assign(utf8_text == nullptr ? "" : utf8_text, byte_count);
    } catch (...) {
        result->utf8_text.clear();
    }
}

void callback_furigana(
    const LocalAIFuriganaToken * tokens,
    size_t token_count,
    void * user_data) {
    auto * result = static_cast<WorkerResult *>(user_data);
    if (result == nullptr || tokens == nullptr) {
        return;
    }
    try {
        result->furigana_tokens.clear();
        result->furigana_tokens.reserve(token_count);
        for (size_t index = 0; index < token_count; ++index) {
            DisplayFuriganaToken token;
            token.surface = utf8_to_wide(tokens[index].surface_utf8, tokens[index].surface_byte_count);
            token.reading = utf8_to_wide(tokens[index].reading_utf8, tokens[index].reading_byte_count);
            token.image_quad = tokens[index].image_quad;
            token.confidence = tokens[index].confidence;
            result->furigana_tokens.push_back(std::move(token));
        }
    } catch (...) {
        result->furigana_tokens.clear();
    }
}

void callback_text_regions(
    const LocalAITextRegion * regions,
    size_t region_count,
    void * user_data) {
    auto * result = static_cast<WorkerResult *>(user_data);
    if (result == nullptr || regions == nullptr) {
        return;
    }
    try {
        result->text_regions.clear();
        result->text_regions.reserve(region_count);
        for (size_t index = 0; index < region_count; ++index) {
            DisplayTextRegion region;
            region.text = utf8_to_wide(regions[index].text_utf8, regions[index].text_byte_count);
            region.image_quad = regions[index].image_quad;
            region.confidence = regions[index].confidence;
            region.flags = regions[index].flags;
            result->text_regions.push_back(std::move(region));
        }
    } catch (...) {
        result->text_regions.clear();
    }
}

const wchar_t * operation_name(Operation operation) {
    switch (operation) {
    case Operation::Ocr:
        return L"OCR";
    case Operation::Furigana:
        return L"Furigana OCR";
    case Operation::Translation:
        return L"Translation";
    }
    return L"Operation";
}

std::wstring status_for_result(const WorkerResult & result) {
    if (result.status == LOCAL_AI_OK) {
        std::wstring text = operation_name(result.operation);
        text += L" complete";
        if (result.memory.reliable != 0) {
            text += L" — peak Vulkan memory " + format_bytes(result.memory.peak_allocated_bytes);
        }
        if (result.operation == Operation::Furigana) {
            text += L" — " + std::to_wstring(result.text_regions.size()) +
                    L" detected regions, " + std::to_wstring(result.furigana_tokens.size()) +
                    L" furigana readings";
        }
        return text;
    }
    std::wstring text = operation_name(result.operation);
    text += L" failed";
    if (result.status == LOCAL_AI_CANCELLED) {
        text = L"Operation cancelled";
    }
    if (!result.error.empty()) {
        text += L": " + result.error;
    }
    return text;
}

bool create_wic_factory(AppState & state) {
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&state.wic_factory));
    if (FAILED(hr)) {
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&state.wic_factory));
    }
    return SUCCEEDED(hr) && state.wic_factory != nullptr;
}

bool load_preview(AppState & state, const std::wstring & path, std::wstring & error) {
    if (!state.wic_factory && !create_wic_factory(state)) {
        error = L"Windows Imaging Component is unavailable.";
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = state.wic_factory->CreateDecoderFromFilename(
        path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder);
    if (FAILED(hr)) {
        error = L"Windows could not decode this image format.";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        error = L"The selected image has no readable frame.";
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(state.wic_factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom))) {
        error = L"Windows could not convert the image for display.";
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
        error = L"The selected image has invalid dimensions.";
        return false;
    }
    state.preview.source = converter;
    state.preview.width = width;
    state.preview.height = height;
    state.preview.bitmap.Reset();
    state.preview.render_target.Reset();
    InvalidateRect(state.preview_window, nullptr, TRUE);
    return true;
}

bool ensure_preview_target(AppState & state) {
    if (state.preview.render_target != nullptr) {
        return true;
    }
    if (!state.d2d_factory || state.preview_window == nullptr) {
        return false;
    }
    RECT rect{};
    GetClientRect(state.preview_window, &rect);
    const auto properties = D2D1::HwndRenderTargetProperties(
        state.preview_window,
        D2D1::SizeU(
            static_cast<UINT>(std::max<LONG>(1, rect.right - rect.left)),
            static_cast<UINT>(std::max<LONG>(1, rect.bottom - rect.top))));
    HRESULT hr = state.d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        properties,
        &state.preview.render_target);
    if (FAILED(hr)) {
        return false;
    }
    if (state.preview.source != nullptr) {
        hr = state.preview.render_target->CreateBitmapFromWicBitmap(
            state.preview.source.Get(),
            nullptr,
            &state.preview.bitmap);
    }
    return SUCCEEDED(hr);
}

bool ensure_write_factory(AppState & state) {
    if (state.write_factory != nullptr) {
        return true;
    }
    return SUCCEEDED(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(state.write_factory.GetAddressOf())));
}

bool rectangles_overlap(const D2D1_RECT_F & first, const D2D1_RECT_F & second) {
    return first.left < second.right && first.right > second.left &&
           first.top < second.bottom && first.bottom > second.top;
}

bool quad_is_valid(const LocalAIQuad & quad) {
    for (const LocalAIPoint & point : quad.points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            return false;
        }
    }
    return true;
}

D2D1_POINT_2F preview_point(
    const LocalAIPoint & point,
    const D2D1_RECT_F & destination,
    float scale) {
    return D2D1::Point2F(
        destination.left + point.x * scale,
        destination.top + point.y * scale);
}

void draw_quad_outline(
    ID2D1RenderTarget * render_target,
    const LocalAIQuad & quad,
    const D2D1_RECT_F & destination,
    float scale,
    ID2D1Brush * brush,
    float stroke_width) {
    if (render_target == nullptr || brush == nullptr || !quad_is_valid(quad)) {
        return;
    }
    for (size_t index = 0; index < std::size(quad.points); ++index) {
        const size_t next = (index + 1u) % std::size(quad.points);
        render_target->DrawLine(
            preview_point(quad.points[index], destination, scale),
            preview_point(quad.points[next], destination, scale),
            brush,
            stroke_width);
    }
}

void draw_detection_overlay(
    AppState & state,
    const D2D1_RECT_F & destination,
    float scale) {
    if (state.text_regions.empty() || state.preview.render_target == nullptr) {
        return;
    }
    ComPtr<ID2D1SolidColorBrush> outline_shadow;
    ComPtr<ID2D1SolidColorBrush> outline;
    if (FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(0x001018, 0.90f),
            &outline_shadow)) ||
        FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(0x00D7D7, 0.95f),
            &outline))) {
        return;
    }
    for (const DisplayTextRegion & region : state.text_regions) {
        draw_quad_outline(
            state.preview.render_target.Get(),
            region.image_quad,
            destination,
            scale,
            outline_shadow.Get(),
            3.0f);
        draw_quad_outline(
            state.preview.render_target.Get(),
            region.image_quad,
            destination,
            scale,
            outline.Get(),
            1.4f);
    }
}

void draw_furigana_overlay(
    AppState & state,
    const D2D1_RECT_F & destination,
    float scale) {
    if (state.furigana_tokens.empty() || !ensure_write_factory(state) ||
        state.preview.render_target == nullptr) {
        return;
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(0x8B0000, 0.95f),
            &brush))) {
        return;
    }
    ComPtr<ID2D1SolidColorBrush> reading_background;
    if (FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(0xFFFFFF, 0.82f),
            &reading_background))) {
        return;
    }
    ComPtr<ID2D1SolidColorBrush> token_outline;
    if (FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(0xFF2D55, 0.95f),
            &token_outline))) {
        return;
    }
    std::vector<D2D1_RECT_F> occupied;
    for (const DisplayFuriganaToken & token : state.furigana_tokens) {
        if (token.reading.empty() || !quad_is_valid(token.image_quad)) {
            continue;
        }
        draw_quad_outline(
            state.preview.render_target.Get(),
            token.image_quad,
            destination,
            scale,
            token_outline.Get(),
            2.0f);
        float left = token.image_quad.points[0].x;
        float right = left;
        float top = token.image_quad.points[0].y;
        float bottom = top;
        for (size_t index = 1; index < std::size(token.image_quad.points); ++index) {
            left = std::min(left, token.image_quad.points[index].x);
            right = std::max(right, token.image_quad.points[index].x);
            top = std::min(top, token.image_quad.points[index].y);
            bottom = std::max(bottom, token.image_quad.points[index].y);
        }
        if (!std::isfinite(left) || !std::isfinite(right) ||
            !std::isfinite(top) || !std::isfinite(bottom) || right <= left || bottom <= top) {
            continue;
        }
        const float screen_left = destination.left + left * scale;
        const float screen_right = destination.left + right * scale;
        const float screen_top = destination.top + top * scale;
        const float screen_bottom = destination.top + bottom * scale;
        const float base_height = std::max(1.0f, screen_bottom - screen_top);
        const float font_size = std::clamp(base_height * 0.55f, 10.0f, 42.0f);
        const float text_width = std::max(font_size * 1.8f, screen_right - screen_left + font_size);
        D2D1_RECT_F text_rect = D2D1::RectF(
            (screen_left + screen_right - text_width) * 0.5f,
            screen_top - font_size * 1.85f,
            (screen_left + screen_right + text_width) * 0.5f,
            screen_top - font_size * 0.15f);
        for (int attempt = 0; attempt < 4; ++attempt) {
            bool collision = false;
            for (const D2D1_RECT_F & other : occupied) {
                if (rectangles_overlap(text_rect, other)) {
                    collision = true;
                    break;
                }
            }
            if (!collision) {
                break;
            }
            text_rect.top -= font_size * 1.15f;
            text_rect.bottom -= font_size * 1.15f;
        }
        text_rect.left = std::max(destination.left + 1.0f, text_rect.left);
        text_rect.right = std::min(destination.right - 1.0f, text_rect.right);
        text_rect.top = std::max(destination.top + 1.0f, text_rect.top);
        text_rect.bottom = std::min(destination.bottom - 1.0f, text_rect.bottom);
        if (text_rect.right <= text_rect.left || text_rect.bottom <= text_rect.top) {
            continue;
        }
        state.preview.render_target->FillRectangle(text_rect, reading_background.Get());
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(state.write_factory->CreateTextFormat(
                L"Yu Gothic UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                font_size,
                L"ja-jp",
                &format))) {
            continue;
        }
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        state.preview.render_target->DrawTextW(
            token.reading.c_str(),
            static_cast<UINT32>(token.reading.size()),
            format.Get(),
            text_rect,
            brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
        occupied.push_back(text_rect);
    }
}

void draw_overlay_legend(AppState & state, const D2D1_RECT_F & destination) {
    if ((state.text_regions.empty() && state.furigana_tokens.empty()) ||
        state.preview.render_target == nullptr || !ensure_write_factory(state)) {
        return;
    }
    ComPtr<ID2D1SolidColorBrush> background;
    ComPtr<ID2D1SolidColorBrush> detected_brush;
    ComPtr<ID2D1SolidColorBrush> furigana_brush;
    ComPtr<ID2D1SolidColorBrush> text_brush;
    if (FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(0x101820, 0.78f),
            &background)) ||
        FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(0x00D7D7, 0.95f),
            &detected_brush)) ||
        FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(0xFF2D55, 0.95f),
            &furigana_brush)) ||
        FAILED(state.preview.render_target->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White, 0.95f),
            &text_brush))) {
        return;
    }
    constexpr float padding = 6.0f;
    constexpr float row_height = 18.0f;
    const size_t row_count = !state.text_regions.empty() && !state.furigana_tokens.empty() ? 2u : 1u;
    const D2D1_RECT_F panel = D2D1::RectF(
        destination.left + 6.0f,
        destination.top + 6.0f,
        destination.left + 150.0f,
        destination.top + 6.0f + padding * 2.0f + row_height * static_cast<float>(row_count));
    state.preview.render_target->FillRectangle(panel, background.Get());

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(state.write_factory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            11.0f,
            L"en-us",
            &format))) {
        return;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float y = panel.top + padding;
    if (!state.text_regions.empty()) {
        state.preview.render_target->DrawLine(
            D2D1::Point2F(panel.left + padding + 1.0f, y + row_height * 0.5f),
            D2D1::Point2F(panel.left + padding + 13.0f, y + row_height * 0.5f),
            detected_brush.Get(),
            2.0f);
        const wchar_t label[] = L"Detected text";
        state.preview.render_target->DrawTextW(
            label,
            static_cast<UINT32>(std::size(label) - 1u),
            format.Get(),
            D2D1::RectF(panel.left + padding + 18.0f, y, panel.right - padding, y + row_height),
            text_brush.Get());
        y += row_height;
    }
    if (!state.furigana_tokens.empty()) {
        state.preview.render_target->DrawLine(
            D2D1::Point2F(panel.left + padding + 1.0f, y + row_height * 0.5f),
            D2D1::Point2F(panel.left + padding + 13.0f, y + row_height * 0.5f),
            furigana_brush.Get(),
            2.0f);
        const wchar_t label[] = L"Furigana";
        state.preview.render_target->DrawTextW(
            label,
            static_cast<UINT32>(std::size(label) - 1u),
            format.Get(),
            D2D1::RectF(panel.left + padding + 18.0f, y, panel.right - padding, y + row_height),
            text_brush.Get());
    }
}

void paint_preview(AppState & state) {
    if (!ensure_preview_target(state)) {
        PAINTSTRUCT paint{};
        BeginPaint(state.preview_window, &paint);
        FillRect(paint.hdc, &paint.rcPaint, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        EndPaint(state.preview_window, &paint);
        return;
    }
    PAINTSTRUCT paint{};
    BeginPaint(state.preview_window, &paint);
    state.preview.render_target->BeginDraw();
    state.preview.render_target->Clear(D2D1::ColorF(D2D1::ColorF::White));
    D2D1_RECT_F destination{};
    float image_scale = 0.0f;
    if (state.preview.bitmap != nullptr && state.preview.width != 0 && state.preview.height != 0) {
        RECT rect{};
        GetClientRect(state.preview_window, &rect);
        const float client_width = static_cast<float>(rect.right - rect.left);
        const float client_height = static_cast<float>(rect.bottom - rect.top);
        const float image_width = static_cast<float>(state.preview.width);
        const float image_height = static_cast<float>(state.preview.height);
        const float scale = std::min(client_width / image_width, client_height / image_height);
        const float draw_width = image_width * std::max(0.0f, scale);
        const float draw_height = image_height * std::max(0.0f, scale);
        destination = D2D1::RectF(
            (client_width - draw_width) / 2.0f,
            (client_height - draw_height) / 2.0f,
            (client_width + draw_width) / 2.0f,
            (client_height + draw_height) / 2.0f);
        image_scale = std::max(0.0f, scale);
        state.preview.render_target->DrawBitmap(
            state.preview.bitmap.Get(),
            destination,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        draw_detection_overlay(state, destination, image_scale);
        draw_furigana_overlay(state, destination, image_scale);
        draw_overlay_legend(state, destination);
    }
    const HRESULT hr = state.preview.render_target->EndDraw();
    EndPaint(state.preview_window, &paint);
    if (hr == D2DERR_RECREATE_TARGET) {
        state.preview.bitmap.Reset();
        state.preview.render_target.Reset();
        InvalidateRect(state.preview_window, nullptr, FALSE);
    }
}

LRESULT CALLBACK preview_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto * state = reinterpret_cast<AppState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto * create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
        state = static_cast<AppState *>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->preview_window = window;
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state != nullptr) {
            paint_preview(*state);
            return 0;
        }
        break;
    case WM_SIZE:
        if (state != nullptr) {
            state->preview.render_target.Reset();
            state->preview.bitmap.Reset();
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void set_active(AppState & state, bool active) {
    state.active = active;
    EnableWindow(state.open_button, !active);
    EnableWindow(state.source_combo, !active);
    EnableWindow(state.target_combo, !active);
    EnableWindow(state.ocr_button, !active);
    EnableWindow(state.furigana_button, !active);
    EnableWindow(state.translate_button, !active);
    EnableWindow(state.extracted_edit, !active);
    EnableWindow(state.translation_edit, !active);
    EnableWindow(state.copy_extracted, !active);
    EnableWindow(state.save_extracted, !active);
    EnableWindow(state.copy_translation, !active);
    EnableWindow(state.save_translation, !active);
    EnableWindow(state.cancel_button, active);
}

const char * selected_language(HWND combo, const Language * languages, size_t count) {
    const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index < 0 || static_cast<size_t>(index) >= count) {
        return languages[0].prompt_name;
    }
    return languages[index].prompt_name;
}

void start_operation(AppState & state, Operation operation) {
    if (state.active || state.engine == nullptr) {
        return;
    }
    if ((operation == Operation::Ocr || operation == Operation::Furigana) && state.image_path.empty()) {
        set_status(state, L"Choose an image before running OCR.");
        return;
    }
    const std::wstring source_text = edit_text(state.extracted_edit);
    if (operation == Operation::Translation && source_text.empty()) {
        set_status(state, L"Enter or extract text before translating.");
        return;
    }
    if (state.worker.joinable()) {
        state.worker.join();
    }
    const std::wstring image_path = state.image_path;
    const std::string source_language = selected_language(
        state.source_combo,
        kSourceLanguages,
        std::size(kSourceLanguages));
    const std::string target_language = selected_language(
        state.target_combo,
        kTargetLanguages,
        std::size(kTargetLanguages));
    const std::string input_utf8 = wide_to_utf8(source_text);
    if (operation == Operation::Ocr || operation == Operation::Furigana) {
        state.furigana_tokens.clear();
        state.text_regions.clear();
        InvalidateRect(state.preview_window, nullptr, FALSE);
    }
    set_active(state, true);
    set_status(
        state,
        operation == Operation::Translation
            ? L"Translating locally..."
            : operation == Operation::Furigana
                  ? L"Running furigana OCR locally..."
                  : L"Running OCR locally...");
    state.worker = std::thread([&state, operation, image_path, source_language, target_language, input_utf8] {
        auto result = std::make_unique<WorkerResult>();
        result->operation = operation;
        if (operation == Operation::Ocr) {
            result->status = local_ai_ocr_file(
                state.engine,
                image_path.c_str(),
                source_language.c_str(),
                &callback_text,
                result.get());
        } else if (operation == Operation::Furigana) {
            result->status = local_ai_ocr_furigana_file_with_regions(
                state.engine,
                image_path.c_str(),
                source_language.c_str(),
                &callback_text,
                &callback_furigana,
                &callback_text_regions,
                result.get());
        } else {
            result->status = local_ai_translate(
                state.engine,
                input_utf8.c_str(),
                source_language.c_str(),
                target_language.c_str(),
                &callback_text,
                result.get());
        }
        const wchar_t * error = local_ai_get_last_error(state.engine);
        if (error != nullptr) {
            result->error = error;
        }
        result->memory.struct_size = sizeof(result->memory);
        local_ai_get_memory_info(state.engine, &result->memory);
        if (!state.closing && !PostMessageW(
                                state.window,
                                kDoneMessage,
                                0,
                                reinterpret_cast<LPARAM>(result.release()))) {
            result.reset();
        } else if (state.closing) {
            result.reset();
        }
    });
}

bool open_image_dialog(AppState & state) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        set_status(state, L"The Windows file picker is unavailable.");
        return false;
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.webp"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(L"Open image for local OCR");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    if (dialog->Show(state.window) != S_OK) {
        return false;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return false;
    }
    PWSTR path = nullptr;
    const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (FAILED(hr) || path == nullptr) {
        set_status(state, L"The selected image path could not be read.");
        return false;
    }
    std::wstring selected(path);
    CoTaskMemFree(path);
    std::wstring error;
    if (!load_preview(state, selected, error)) {
        set_status(state, error);
        MessageBoxW(state.window, error.c_str(), L"Open image", MB_ICONERROR | MB_OK);
        return false;
    }
    state.image_path = std::move(selected);
    state.furigana_tokens.clear();
    state.text_regions.clear();
    SetWindowTextW(state.extracted_edit, L"");
    SetWindowTextW(state.translation_edit, L"");
    set_status(state, L"Image selected. Ready for local OCR.");
    return true;
}

bool copy_edit(HWND edit) {
    const std::wstring full_text = edit_text(edit);
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    const size_t first = std::min<size_t>(start, full_text.size());
    const size_t last = std::min<size_t>(std::max<DWORD>(start, end), full_text.size());
    const std::wstring text = first != last ? full_text.substr(first, last - first) : full_text;
    if (!OpenClipboard(edit)) {
        return false;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }
    void * destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool save_text_dialog(AppState & state, HWND edit) {
    const std::wstring text = edit_text(edit);
    if (text.empty()) {
        set_status(state, L"There is no text to save.");
        return false;
    }
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileSaveDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        set_status(state, L"The Windows save dialog is unavailable.");
        return false;
    }
    const COMDLG_FILTERSPEC filter = {L"UTF-8 text", L"*.txt"};
    dialog->SetFileTypes(1, &filter);
    dialog->SetDefaultExtension(L"txt");
    dialog->SetTitle(L"Save text");
    if (dialog->Show(state.window) != S_OK) {
        return false;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return false;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) {
        set_status(state, L"The save path could not be read.");
        return false;
    }
    const std::wstring selected(path);
    CoTaskMemFree(path);
    const std::string utf8 = wide_to_utf8(text);
    HANDLE file = CreateFileW(
        selected.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_status(state, L"Windows could not create the text file.");
        return false;
    }
    DWORD written = 0;
    const bool success = utf8.size() <= MAXDWORD &&
                         (utf8.empty() || WriteFile(
                             file,
                             utf8.data(),
                             static_cast<DWORD>(utf8.size()),
                             &written,
                             nullptr) != FALSE) &&
                         written == utf8.size();
    CloseHandle(file);
    if (!success) {
        set_status(state, L"Windows could not write the complete text file.");
        return false;
    }
    set_status(state, L"Text saved as UTF-8.");
    return true;
}

bool create_engine(AppState & state) {
    const std::vector<std::filesystem::path> roots = model_roots();
    const std::filesystem::path default_root = roots.empty()
                                                   ? std::filesystem::path(get_executable_directory()) / L"models"
                                                   : roots.front();
    std::filesystem::path ocr_model = choose_model_file(
        default_root / L"ocr",
        {L"ATH-MaaS_OvisOCR2-Q6_K.gguf",
         L"ATH-MaaS_OvisOCR2-Q6_K_L.gguf",
         L"ATH-MaaS_OvisOCR2-Q4_K_M.gguf"});
    std::filesystem::path projector = default_root / L"ocr" / L"mmproj-ATH-MaaS_OvisOCR2-bf16.gguf";
    std::filesystem::path translation_model = choose_model_file(
        default_root / L"translation",
        {L"Hy-MT2-1.8B-Q6_K.gguf", L"Hy-MT2-1.8B-Q4_K_M.gguf"});
    std::filesystem::path ppocr_detection_model = default_root / L"ppocr" / L"PP-OCRv6_medium_det.onnx";
    std::filesystem::path ppocr_recognition_model = default_root / L"ppocr" / L"PP-OCRv6_medium_rec.onnx";
    std::filesystem::path ppocr_dictionary = default_root / L"ppocr" / L"PP-OCRv6_medium_rec_inference.yml";
    std::filesystem::path japanese_dictionary = default_root / L"japanese" / L"unidic";
    if (!std::filesystem::is_directory(japanese_dictionary) &&
        !std::filesystem::is_regular_file(japanese_dictionary)) {
        japanese_dictionary = default_root / L"japanese" / L"readings.tsv";
    }
    for (const auto & root : roots) {
        const auto candidate_ocr = choose_model_file(
            root / L"ocr",
            {L"ATH-MaaS_OvisOCR2-Q6_K.gguf",
             L"ATH-MaaS_OvisOCR2-Q6_K_L.gguf",
             L"ATH-MaaS_OvisOCR2-Q4_K_M.gguf"});
        const auto candidate_projector = root / L"ocr" / L"mmproj-ATH-MaaS_OvisOCR2-bf16.gguf";
        const auto candidate_translation = choose_model_file(
            root / L"translation",
            {L"Hy-MT2-1.8B-Q6_K.gguf", L"Hy-MT2-1.8B-Q4_K_M.gguf"});
        const auto candidate_ppocr_detection = root / L"ppocr" / L"PP-OCRv6_medium_det.onnx";
        const auto candidate_ppocr_recognition = root / L"ppocr" / L"PP-OCRv6_medium_rec.onnx";
        const auto candidate_ppocr_dictionary = root / L"ppocr" / L"PP-OCRv6_medium_rec_inference.yml";
        const auto candidate_japanese_directory = root / L"japanese" / L"unidic";
        const auto candidate_japanese_fallback = root / L"japanese" / L"readings.tsv";
        if (std::filesystem::is_regular_file(candidate_ocr) &&
            std::filesystem::is_regular_file(candidate_projector)) {
            ocr_model = candidate_ocr;
            projector = candidate_projector;
        }
        if (std::filesystem::is_regular_file(candidate_translation)) {
            translation_model = candidate_translation;
        }
        if (std::filesystem::is_regular_file(candidate_ppocr_detection)) {
            ppocr_detection_model = candidate_ppocr_detection;
        }
        if (std::filesystem::is_regular_file(candidate_ppocr_recognition)) {
            ppocr_recognition_model = candidate_ppocr_recognition;
        }
        if (std::filesystem::is_regular_file(candidate_ppocr_dictionary)) {
            ppocr_dictionary = candidate_ppocr_dictionary;
        }
        if (std::filesystem::is_directory(candidate_japanese_directory)) {
            japanese_dictionary = candidate_japanese_directory;
        } else if (std::filesystem::is_regular_file(candidate_japanese_fallback)) {
            japanese_dictionary = candidate_japanese_fallback;
        }
        if (std::filesystem::is_regular_file(candidate_ocr) &&
            std::filesystem::is_regular_file(candidate_projector) &&
            std::filesystem::is_regular_file(candidate_translation) &&
            std::filesystem::is_regular_file(candidate_ppocr_detection) &&
            std::filesystem::is_regular_file(candidate_ppocr_recognition) &&
            std::filesystem::is_regular_file(candidate_ppocr_dictionary) &&
            (std::filesystem::is_directory(candidate_japanese_directory) ||
             std::filesystem::is_regular_file(candidate_japanese_fallback))) {
            break;
        }
    }
    LocalAIConfig config{};
    config.struct_size = sizeof(config);
    const std::wstring ocr_model_string = ocr_model.wstring();
    const std::wstring projector_string = projector.wstring();
    const std::wstring translation_string = translation_model.wstring();
    const std::wstring ppocr_detection_string = ppocr_detection_model.wstring();
    const std::wstring ppocr_recognition_string = ppocr_recognition_model.wstring();
    const std::wstring ppocr_dictionary_string = ppocr_dictionary.wstring();
    const std::wstring japanese_dictionary_string = japanese_dictionary.wstring();
    config.ocr_model_path = ocr_model_string.c_str();
    config.vision_projector_path = projector_string.c_str();
    config.translation_model_path = translation_string.c_str();
    config.ppocr_detection_model_path = ppocr_detection_string.c_str();
    config.ppocr_recognition_model_path = ppocr_recognition_string.c_str();
    config.ppocr_dictionary_path = ppocr_dictionary_string.c_str();
    config.japanese_dictionary_path = japanese_dictionary_string.c_str();
    config.ppocr_max_image_side = 1280;
    config.ppocr_threads = 0;
    config.translation_context_size = 2048;
    config.ocr_context_size = 8192;
    config.gpu_layers = -1;
    config.maximum_vram_bytes = 6ull * 1024ull * 1024ull * 1024ull;
    config.flags = LOCAL_AI_CONFIG_SEQUENTIAL_MODELS | LOCAL_AI_CONFIG_ENABLE_FURIGANA;
    const LocalAIStatus status = local_ai_create(&config, &state.engine);
    if (status != LOCAL_AI_OK || state.engine == nullptr) {
        set_status(state, L"The native AI engine could not be created.");
        return false;
    }
    const bool have_ocr = std::filesystem::is_regular_file(ocr_model) &&
                          std::filesystem::is_regular_file(projector);
    const bool have_translation = std::filesystem::is_regular_file(translation_model);
    const bool have_furigana = std::filesystem::is_regular_file(ppocr_detection_model) &&
                               std::filesystem::is_regular_file(ppocr_recognition_model) &&
                               std::filesystem::is_regular_file(ppocr_dictionary) &&
                               (std::filesystem::is_directory(japanese_dictionary) ||
                                std::filesystem::is_regular_file(japanese_dictionary));
    if (!have_ocr || !have_translation || !have_furigana) {
        std::wstring message = L"Ready. Model files are not installed; run scripts\\setup_models.ps1.";
        if (!have_ocr) {
            message += L" Missing OvisOCR2 GGUF/projector.";
        }
        if (!have_translation) {
            message += L" Missing Hy-MT2 GGUF.";
        }
        if (!have_furigana) {
            message += L" Missing PP-OCRv6 or Japanese reading assets; Furigana is unavailable.";
        }
        set_status(state, message);
    } else {
        set_status(state, L"Ready. All inference stays local to this computer.");
    }
    return true;
}

HWND create_control(
    AppState & state,
    const wchar_t * class_name,
    const wchar_t * text,
    DWORD style,
    DWORD ex_style,
    UINT id) {
    HWND control = CreateWindowExW(
        ex_style,
        class_name,
        text,
        style,
        0,
        0,
        0,
        0,
        state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        state.instance,
        nullptr);
    apply_font(control, state.font);
    return control;
}

void create_controls(AppState & state) {
    state.font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    state.open_button = create_control(
        state,
        L"BUTTON",
        L"Open image…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kOpenImage);
    state.source_combo = create_control(
        state,
        L"COMBOBOX",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0,
        kSourceLanguage);
    state.target_combo = create_control(
        state,
        L"COMBOBOX",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0,
        kTargetLanguage);
    for (const Language & language : kSourceLanguages) {
        SendMessageW(state.source_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(language.display));
    }
    for (const Language & language : kTargetLanguages) {
        SendMessageW(state.target_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(language.display));
    }
    SendMessageW(state.source_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(state.target_combo, CB_SETCURSEL, 1, 0);
    state.ocr_button = create_control(
        state,
        L"BUTTON",
        L"Run OCR",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kRunOcr);
    state.furigana_button = create_control(
        state,
        L"BUTTON",
        L"Furigana",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kFurigana);
    state.translate_button = create_control(
        state,
        L"BUTTON",
        L"Translate",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kTranslate);
    state.cancel_button = create_control(
        state,
        L"BUTTON",
        L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kCancel);
    state.status = create_control(
        state,
        L"STATIC",
        L"Starting…",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0,
        kStatus);
    state.extracted_label = create_control(
        state,
        L"STATIC",
        L"Extracted text",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0,
        kExtractedLabel);
    state.translation_label = create_control(
        state,
        L"STATIC",
        L"Translation",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0,
        kTranslationLabel);
    state.extracted_edit = create_control(
        state,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_WANTRETURN | WS_VSCROLL | ES_NOHIDESEL,
        WS_EX_CLIENTEDGE,
        kExtractedEdit);
    state.translation_edit = create_control(
        state,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_WANTRETURN | WS_VSCROLL | ES_READONLY | ES_NOHIDESEL,
        WS_EX_CLIENTEDGE,
        kTranslationEdit);
    state.copy_extracted = create_control(
        state,
        L"BUTTON",
        L"Copy",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kCopyExtracted);
    state.save_extracted = create_control(
        state,
        L"BUTTON",
        L"Save text…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kSaveExtracted);
    state.copy_translation = create_control(
        state,
        L"BUTTON",
        L"Copy",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kCopyTranslation);
    state.save_translation = create_control(
        state,
        L"BUTTON",
        L"Save text…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0,
        kSaveTranslation);
    SendMessageW(state.extracted_edit, EM_SETLIMITTEXT, 8u * 1024u * 1024u, 0);
    SendMessageW(state.translation_edit, EM_SETLIMITTEXT, 8u * 1024u * 1024u, 0);
    load_settings(state);
}

void layout_controls(AppState & state, int width, int height) {
    const int toolbar = 44;
    MoveWindow(state.open_button, 8, 8, 116, 28, TRUE);
    MoveWindow(state.source_combo, 132, 8, 120, 300, TRUE);
    MoveWindow(state.target_combo, 260, 8, 120, 300, TRUE);
    MoveWindow(state.ocr_button, 388, 8, 90, 28, TRUE);
    MoveWindow(state.furigana_button, 484, 8, 82, 28, TRUE);
    MoveWindow(state.translate_button, 572, 8, 92, 28, TRUE);
    MoveWindow(state.cancel_button, 670, 8, 78, 28, TRUE);
    MoveWindow(state.status, 758, 8, std::max(160, width - 766), 28, TRUE);

    const int main_height = std::max(100, height - toolbar - 8);
    const int left_width = std::clamp(width * 42 / 100, 280, std::max(280, width - 520));
    MoveWindow(state.preview_window, 8, toolbar, left_width - 16, main_height, TRUE);

    const int right_x = left_width + 4;
    const int right_width = std::max(300, width - right_x - 8);
    const int pane_gap = 12;
    const int button_height = 28;
    const int label_height = 20;
    const int pane_height = std::max(80, (main_height - pane_gap) / 2);
    const int first_y = toolbar;
    const int second_y = toolbar + pane_height + pane_gap;
    MoveWindow(state.extracted_label, right_x, first_y, right_width, label_height, TRUE);
    MoveWindow(
        state.extracted_edit,
        right_x,
        first_y + label_height,
        right_width,
        std::max(40, pane_height - label_height - button_height - 4),
        TRUE);
    const int first_button_y = first_y + pane_height - button_height;
    MoveWindow(state.copy_extracted, right_x, first_button_y, 76, button_height, TRUE);
    MoveWindow(state.save_extracted, right_x + 84, first_button_y, 104, button_height, TRUE);
    MoveWindow(state.translation_label, right_x, second_y, right_width, label_height, TRUE);
    MoveWindow(
        state.translation_edit,
        right_x,
        second_y + label_height,
        right_width,
        std::max(40, pane_height - label_height - button_height - 4),
        TRUE);
    const int second_button_y = second_y + pane_height - button_height;
    MoveWindow(state.copy_translation, right_x, second_button_y, 76, button_height, TRUE);
    MoveWindow(state.save_translation, right_x + 84, second_button_y, 104, button_height, TRUE);
}

void handle_drop(AppState & state, HDROP drop) {
    wchar_t path[MAX_PATH]{};
    if (DragQueryFileW(drop, 0, path, std::size(path)) == 0) {
        DragFinish(drop);
        return;
    }
    std::wstring selected(path);
    DragFinish(drop);
    std::wstring error;
    if (!load_preview(state, selected, error)) {
        set_status(state, error);
        return;
    }
    state.image_path = std::move(selected);
    state.furigana_tokens.clear();
    state.text_regions.clear();
    SetWindowTextW(state.extracted_edit, L"");
    SetWindowTextW(state.translation_edit, L"");
    set_status(state, L"Image selected. Ready for local OCR.");
}

LRESULT CALLBACK main_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto * state = reinterpret_cast<AppState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto * create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
        try {
            state = new AppState();
        } catch (...) {
            return FALSE;
        }
        state->instance = static_cast<HINSTANCE>(create->hInstance);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }
    switch (message) {
    case WM_CREATE: {
        create_controls(*state);
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                state->d2d_factory.GetAddressOf()))) {
            set_status(*state, L"Direct2D preview is unavailable; OCR can still run.");
        }
        CreateWindowExW(
            0,
            kPreviewClass,
            L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_CLIPCHILDREN,
            0,
            0,
            0,
            0,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreview)),
            state->instance,
            state);
        SetWindowPos(
            window,
            nullptr,
            0,
            0,
            state->saved_width,
            state->saved_height,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        DragAcceptFiles(window, TRUE);
        create_engine(*state);
        return 0;
    }
    case WM_SIZE:
        layout_controls(*state, LOWORD(l_param), HIWORD(l_param));
        return 0;
    case WM_COMMAND: {
        const UINT id = LOWORD(w_param);
        const UINT notification = HIWORD(w_param);
        if (notification == BN_CLICKED) {
            if (id == kOpenImage) {
                open_image_dialog(*state);
            } else if (id == kRunOcr) {
                start_operation(*state, Operation::Ocr);
            } else if (id == kFurigana) {
                start_operation(*state, Operation::Furigana);
            } else if (id == kTranslate) {
                start_operation(*state, Operation::Translation);
            } else if (id == kCancel) {
                local_ai_cancel(state->engine);
                set_status(*state, L"Cancellation requested…");
            } else if (id == kCopyExtracted) {
                set_status(*state, copy_edit(state->extracted_edit) ? L"Extracted text copied." : L"Could not copy extracted text.");
            } else if (id == kSaveExtracted) {
                save_text_dialog(*state, state->extracted_edit);
            } else if (id == kCopyTranslation) {
                set_status(*state, copy_edit(state->translation_edit) ? L"Translation copied." : L"Could not copy translation.");
            } else if (id == kSaveTranslation) {
                save_text_dialog(*state, state->translation_edit);
            }
        }
        return 0;
    }
    case WM_DROPFILES:
        handle_drop(*state, reinterpret_cast<HDROP>(w_param));
        return 0;
    case kDoneMessage: {
        auto result = std::unique_ptr<WorkerResult>(reinterpret_cast<WorkerResult *>(l_param));
        if (state->worker.joinable()) {
            state->worker.join();
        }
        if (result->status == LOCAL_AI_OK) {
            const std::wstring text = utf8_to_wide(result->utf8_text);
            if (result->operation == Operation::Ocr || result->operation == Operation::Furigana) {
                SetWindowTextW(state->extracted_edit, text.c_str());
                SetWindowTextW(state->translation_edit, L"");
                state->furigana_tokens = std::move(result->furigana_tokens);
                state->text_regions = std::move(result->text_regions);
                InvalidateRect(state->preview_window, nullptr, FALSE);
            } else {
                SetWindowTextW(state->translation_edit, text.c_str());
            }
        }
        set_active(*state, false);
        set_status(*state, status_for_result(*result));
        if (result->status != LOCAL_AI_OK && result->status != LOCAL_AI_CANCELLED) {
            MessageBoxW(state->window, status_for_result(*result).c_str(), L"Local AI", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    case WM_CLOSE:
        state->closing = true;
        local_ai_cancel(state->engine);
        if (state->worker.joinable()) {
            state->worker.join();
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->closing = true;
        local_ai_cancel(state->engine);
        if (state->worker.joinable()) {
            state->worker.join();
        }
        save_settings(*state);
        local_ai_destroy(state->engine);
        state->engine = nullptr;
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

bool register_window_classes(HINSTANCE instance) {
    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.hInstance = instance;
    main_class.lpfnWndProc = &main_window_proc;
    main_class.lpszClassName = kWindowClass;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassExW(&main_class) == 0) {
        return false;
    }
    WNDCLASSEXW preview_class{};
    preview_class.cbSize = sizeof(preview_class);
    preview_class.hInstance = instance;
    preview_class.lpfnWndProc = &preview_window_proc;
    preview_class.lpszClassName = kPreviewClass;
    preview_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    preview_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassExW(&preview_class) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_initialized = SUCCEEDED(com_result);
    INITCOMMONCONTROLSEX common_controls{sizeof(common_controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common_controls);
    if (!register_window_classes(instance)) {
        if (com_initialized) {
            CoUninitialize();
        }
        MessageBoxW(nullptr, L"The Local AI window classes could not be registered.", L"Local AI", MB_ICONERROR | MB_OK);
        return 1;
    }
    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        L"Local AI OCR Translator",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1200,
        760,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        if (com_initialized) {
            CoUninitialize();
        }
        MessageBoxW(nullptr, L"The Local AI window could not be created.", L"Local AI", MB_ICONERROR | MB_OK);
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (com_initialized) {
        CoUninitialize();
    }
    return static_cast<int>(message.wParam);
}
