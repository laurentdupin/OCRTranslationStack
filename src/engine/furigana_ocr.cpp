#include "furigana_ocr.h"

#include <onnxruntime_cxx_api.h>

#include "mecab.h"

#if defined(_WIN32)
#  include <windows.h>
#  include <wincodec.h>
#  include <wrl/client.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <string_view>
#include <utility>

namespace localai {

namespace {

struct Box {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float confidence = 0.0f;
    LocalAIQuad quad{};
    bool has_quad = false;
};

using GeometryPoint = LocalAIPoint;

struct RotatedBox {
    std::array<GeometryPoint, 4> points{};
    float short_side = 0.0f;
};

struct AlignedCharacter {
    char32_t character = U' ';
    float start = 0.0f;
    float end = 0.0f;
    float confidence = 0.0f;
};

struct RecognizedLine {
    std::string utf8;
    std::vector<AlignedCharacter> characters;
};

std::string trim_ascii(std::string value) {
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t first = 0;
    while (first < value.size() && is_space(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && is_space(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

bool trace_enabled() {
    const char * value = std::getenv("LOCALAI_PPOCR_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool path_is_file(const std::wstring & path) {
    return !path.empty() && std::filesystem::is_regular_file(std::filesystem::path(path));
}

bool path_is_directory(const std::wstring & path) {
    return !path.empty() && std::filesystem::is_directory(std::filesystem::path(path));
}

bool decode_utf8(std::string_view input, std::vector<char32_t> & output) {
    output.clear();
    for (size_t i = 0; i < input.size();) {
        const unsigned char first = static_cast<unsigned char>(input[i]);
        char32_t value = 0;
        size_t length = 0;
        if (first < 0x80u) {
            value = first;
            length = 1;
        } else if ((first & 0xe0u) == 0xc0u) {
            value = first & 0x1fu;
            length = 2;
        } else if ((first & 0xf0u) == 0xe0u) {
            value = first & 0x0fu;
            length = 3;
        } else if ((first & 0xf8u) == 0xf0u) {
            value = first & 0x07u;
            length = 4;
        } else {
            return false;
        }
        if (i + length > input.size()) {
            return false;
        }
        for (size_t j = 1; j < length; ++j) {
            const unsigned char next = static_cast<unsigned char>(input[i + j]);
            if ((next & 0xc0u) != 0x80u) {
                return false;
            }
            value = (value << 6u) | (next & 0x3fu);
        }
        if ((length == 2 && value < 0x80u) ||
            (length == 3 && value < 0x800u) ||
            (length == 4 && value < 0x10000u) ||
            value > 0x10ffffu ||
            (value >= 0xd800u && value <= 0xdfffu)) {
            return false;
        }
        output.push_back(value);
        i += length;
    }
    return true;
}

void append_utf8(char32_t value, std::string & output) {
    if (value <= 0x7fu) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (value >> 6u)));
        output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    } else if (value <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (value >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (value >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((value >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    }
}

std::string utf32_to_utf8(const std::vector<char32_t> & input) {
    std::string output;
    for (const char32_t value : input) {
        append_utf8(value, output);
    }
    return output;
}

bool is_kanji(char32_t value) {
    return (value >= 0x3400 && value <= 0x4dbf) ||
           (value >= 0x4e00 && value <= 0x9fff) ||
           (value >= 0xf900 && value <= 0xfaff) ||
           (value >= 0x20000 && value <= 0x323af);
}

bool is_kana(char32_t value) {
    return (value >= 0x3040 && value <= 0x30ff) ||
           (value >= 0x31f0 && value <= 0x31ff) ||
           (value >= 0xff66 && value <= 0xff9f);
}

bool contains_kanji(std::string_view text) {
    std::vector<char32_t> codepoints;
    if (!decode_utf8(text, codepoints)) {
        return false;
    }
    return std::any_of(codepoints.begin(), codepoints.end(), is_kanji);
}

std::string katakana_to_hiragana(std::string value) {
    std::vector<char32_t> codepoints;
    if (!decode_utf8(value, codepoints)) {
        return {};
    }
    for (char32_t & codepoint : codepoints) {
        if (codepoint >= 0x30a1 && codepoint <= 0x30f6) {
            codepoint -= 0x60;
        }
    }
    return utf32_to_utf8(codepoints);
}

std::vector<std::string> split_feature(const char * feature) {
    std::vector<std::string> fields;
    if (feature == nullptr) {
        return fields;
    }
    std::string current;
    for (const char * cursor = feature;; ++cursor) {
        if (*cursor == ',' || *cursor == '\0') {
            fields.push_back(current);
            current.clear();
            if (*cursor == '\0') {
                break;
            }
        } else {
            current.push_back(*cursor);
        }
    }
    return fields;
}

std::string find_reading(const char * feature) {
    const std::vector<std::string> fields = split_feature(feature);
    const auto usable = [](const std::string & candidate) {
        if (candidate.empty() || candidate == "*") {
            return false;
        }
        std::vector<char32_t> codepoints;
        if (!decode_utf8(candidate, codepoints) || codepoints.empty()) {
            return false;
        }
        return std::any_of(codepoints.begin(), codepoints.end(), is_kana);
    };
    for (const size_t index : {size_t(7), size_t(6), size_t(5)}) {
        if (index < fields.size() && usable(fields[index])) {
            return katakana_to_hiragana(fields[index]);
        }
    }
    for (const std::string & field : fields) {
        if (usable(field)) {
            return katakana_to_hiragana(field);
        }
    }
    return {};
}

float geometry_cross(
    const GeometryPoint & origin,
    const GeometryPoint & first,
    const GeometryPoint & second) {
    return (first.x - origin.x) * (second.y - origin.y) -
           (first.y - origin.y) * (second.x - origin.x);
}

float polygon_signed_area(const std::vector<GeometryPoint> & polygon) {
    if (polygon.size() < 3u) {
        return 0.0f;
    }
    double area = 0.0;
    for (size_t index = 0; index < polygon.size(); ++index) {
        const GeometryPoint & first = polygon[index];
        const GeometryPoint & second = polygon[(index + 1u) % polygon.size()];
        area += static_cast<double>(first.x) * second.y -
                static_cast<double>(second.x) * first.y;
    }
    return static_cast<float>(area * 0.5);
}

float polygon_perimeter(const std::vector<GeometryPoint> & polygon) {
    float perimeter = 0.0f;
    for (size_t index = 0; index < polygon.size(); ++index) {
        const GeometryPoint & first = polygon[index];
        const GeometryPoint & second = polygon[(index + 1u) % polygon.size()];
        perimeter += std::hypot(second.x - first.x, second.y - first.y);
    }
    return perimeter;
}

std::vector<GeometryPoint> convex_hull(std::vector<GeometryPoint> points) {
    if (points.size() <= 1u) {
        return points;
    }
    std::sort(points.begin(), points.end(), [](const GeometryPoint & first, const GeometryPoint & second) {
        if (first.x != second.x) {
            return first.x < second.x;
        }
        return first.y < second.y;
    });
    points.erase(
        std::unique(points.begin(), points.end(), [](const GeometryPoint & first, const GeometryPoint & second) {
            return first.x == second.x && first.y == second.y;
        }),
        points.end());
    if (points.size() <= 2u) {
        return points;
    }

    std::vector<GeometryPoint> hull;
    hull.reserve(points.size() * 2u);
    for (const GeometryPoint & point : points) {
        while (hull.size() >= 2u &&
               geometry_cross(hull[hull.size() - 2u], hull.back(), point) <= 0.0f) {
            hull.pop_back();
        }
        hull.push_back(point);
    }
    const size_t lower_size = hull.size();
    for (size_t index = points.size(); index-- > 0u;) {
        const GeometryPoint & point = points[index];
        while (hull.size() > lower_size &&
               geometry_cross(hull[hull.size() - 2u], hull.back(), point) <= 0.0f) {
            hull.pop_back();
        }
        hull.push_back(point);
    }
    if (!hull.empty()) {
        hull.pop_back();
    }
    return hull;
}

std::array<GeometryPoint, 4> order_quad(const std::vector<GeometryPoint> & points) {
    std::array<GeometryPoint, 4> ordered{};
    if (points.empty()) {
        return ordered;
    }
    const float infinity = std::numeric_limits<float>::infinity();
    float minimum_sum = infinity;
    float maximum_sum = -infinity;
    float minimum_difference = infinity;
    float maximum_difference = -infinity;
    for (const GeometryPoint & point : points) {
        const float sum = point.x + point.y;
        const float difference = point.x - point.y;
        if (sum < minimum_sum) {
            minimum_sum = sum;
            ordered[0] = point;
        }
        if (difference > maximum_difference) {
            maximum_difference = difference;
            ordered[1] = point;
        }
        if (sum > maximum_sum) {
            maximum_sum = sum;
            ordered[2] = point;
        }
        if (difference < minimum_difference) {
            minimum_difference = difference;
            ordered[3] = point;
        }
    }
    return ordered;
}

RotatedBox minimum_area_box(const std::vector<GeometryPoint> & polygon) {
    RotatedBox result;
    if (polygon.size() < 3u) {
        return result;
    }
    float best_area = std::numeric_limits<float>::infinity();
    for (size_t index = 0; index < polygon.size(); ++index) {
        const GeometryPoint & first = polygon[index];
        const GeometryPoint & second = polygon[(index + 1u) % polygon.size()];
        const float dx = second.x - first.x;
        const float dy = second.y - first.y;
        const float length = std::hypot(dx, dy);
        if (length <= 1.0e-4f) {
            continue;
        }
        const float cosine = dx / length;
        const float sine = dy / length;
        float minimum_x = std::numeric_limits<float>::infinity();
        float maximum_x = -std::numeric_limits<float>::infinity();
        float minimum_y = std::numeric_limits<float>::infinity();
        float maximum_y = -std::numeric_limits<float>::infinity();
        for (const GeometryPoint & point : polygon) {
            const float rotated_x = point.x * cosine + point.y * sine;
            const float rotated_y = -point.x * sine + point.y * cosine;
            minimum_x = std::min(minimum_x, rotated_x);
            maximum_x = std::max(maximum_x, rotated_x);
            minimum_y = std::min(minimum_y, rotated_y);
            maximum_y = std::max(maximum_y, rotated_y);
        }
        const float box_width = maximum_x - minimum_x;
        const float box_height = maximum_y - minimum_y;
        const float area = box_width * box_height;
        if (!(area > 0.0f) || area >= best_area) {
            continue;
        }
        const std::vector<GeometryPoint> corners = {
            {minimum_x * cosine - minimum_y * sine, minimum_x * sine + minimum_y * cosine},
            {maximum_x * cosine - minimum_y * sine, maximum_x * sine + minimum_y * cosine},
            {maximum_x * cosine - maximum_y * sine, maximum_x * sine + maximum_y * cosine},
            {minimum_x * cosine - maximum_y * sine, minimum_x * sine + maximum_y * cosine}};
        result.points = order_quad(corners);
        result.short_side = std::min(box_width, box_height);
        best_area = area;
    }
    return result;
}

bool point_in_polygon(const GeometryPoint & point, const std::vector<GeometryPoint> & polygon) {
    bool inside = false;
    for (size_t index = 0, previous = polygon.size() - 1u;
         index < polygon.size();
         previous = index++) {
        const GeometryPoint & current_point = polygon[index];
        const GeometryPoint & previous_point = polygon[previous];
        const bool crosses = (current_point.y > point.y) != (previous_point.y > point.y);
        if (crosses) {
            const float x_at_y = (previous_point.x - current_point.x) *
                                     (point.y - current_point.y) /
                                     (previous_point.y - current_point.y) +
                                 current_point.x;
            if (point.x < x_at_y) {
                inside = !inside;
            }
        }
    }
    return inside;
}

float polygon_score_fast(
    const std::vector<float> & probabilities,
    size_t width,
    size_t height,
    const std::vector<GeometryPoint> & polygon) {
    if (polygon.size() < 3u || width == 0u || height == 0u) {
        return 0.0f;
    }
    float minimum_x = std::numeric_limits<float>::infinity();
    float maximum_x = -std::numeric_limits<float>::infinity();
    float minimum_y = std::numeric_limits<float>::infinity();
    float maximum_y = -std::numeric_limits<float>::infinity();
    for (const GeometryPoint & point : polygon) {
        minimum_x = std::min(minimum_x, point.x);
        maximum_x = std::max(maximum_x, point.x);
        minimum_y = std::min(minimum_y, point.y);
        maximum_y = std::max(maximum_y, point.y);
    }
    const size_t left = std::min(width - 1u, static_cast<size_t>(std::max(0.0f, std::floor(minimum_x))));
    const size_t right = std::min(width - 1u, static_cast<size_t>(std::max(0.0f, std::ceil(maximum_x))));
    const size_t top = std::min(height - 1u, static_cast<size_t>(std::max(0.0f, std::floor(minimum_y))));
    const size_t bottom = std::min(height - 1u, static_cast<size_t>(std::max(0.0f, std::ceil(maximum_y))));
    if (right < left || bottom < top) {
        return 0.0f;
    }
    double sum = 0.0;
    size_t count = 0;
    for (size_t y = top; y <= bottom; ++y) {
        for (size_t x = left; x <= right; ++x) {
            if (point_in_polygon(
                    GeometryPoint{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f},
                    polygon)) {
                sum += probabilities[y * width + x];
                ++count;
            }
        }
    }
    return count == 0u ? 0.0f : static_cast<float>(sum / count);
}

bool line_intersection(
    const GeometryPoint & first,
    const GeometryPoint & first_direction,
    const GeometryPoint & second,
    const GeometryPoint & second_direction,
    GeometryPoint & result) {
    const float denominator = first_direction.x * second_direction.y -
                              first_direction.y * second_direction.x;
    if (std::abs(denominator) <= 1.0e-5f) {
        return false;
    }
    const GeometryPoint offset{second.x - first.x, second.y - first.y};
    const float amount = (offset.x * second_direction.y - offset.y * second_direction.x) /
                         denominator;
    result = {
        first.x + first_direction.x * amount,
        first.y + first_direction.y * amount};
    return true;
}

std::vector<GeometryPoint> offset_convex_polygon(
    std::vector<GeometryPoint> polygon,
    float distance) {
    if (polygon.size() < 3u || !(distance > 0.0f)) {
        return polygon;
    }
    if (polygon_signed_area(polygon) < 0.0f) {
        std::reverse(polygon.begin(), polygon.end());
    }
    std::vector<GeometryPoint> shifted(polygon.size());
    std::vector<GeometryPoint> directions(polygon.size());
    for (size_t index = 0; index < polygon.size(); ++index) {
        const GeometryPoint & first = polygon[index];
        const GeometryPoint & second = polygon[(index + 1u) % polygon.size()];
        const float dx = second.x - first.x;
        const float dy = second.y - first.y;
        const float length = std::hypot(dx, dy);
        if (length <= 1.0e-5f) {
            return {};
        }
        directions[index] = {dx, dy};
        shifted[index] = {
            first.x + dy / length * distance,
            first.y - dx / length * distance};
    }
    std::vector<GeometryPoint> expanded(polygon.size());
    for (size_t index = 0; index < polygon.size(); ++index) {
        const size_t previous = (index + polygon.size() - 1u) % polygon.size();
        if (!line_intersection(
                shifted[previous],
                directions[previous],
                shifted[index],
                directions[index],
                expanded[index])) {
            return {};
        }
    }
    return expanded;
}

LocalAIQuad make_quad(const Box & box) {
    if (box.has_quad) {
        return box.quad;
    }
    LocalAIQuad quad{};
    quad.points[0] = {box.left, box.top};
    quad.points[1] = {box.right, box.top};
    quad.points[2] = {box.right, box.bottom};
    quad.points[3] = {box.left, box.bottom};
    return quad;
}

LocalAIQuad interpolate_quad(const LocalAIQuad & quad, float start, float end) {
    const auto interpolate = [](LocalAIPoint left, LocalAIPoint right, float amount) {
        return LocalAIPoint{
            left.x + (right.x - left.x) * amount,
            left.y + (right.y - left.y) * amount};
    };
    LocalAIQuad result{};
    result.points[0] = interpolate(quad.points[0], quad.points[1], start);
    result.points[1] = interpolate(quad.points[0], quad.points[1], end);
    result.points[2] = interpolate(quad.points[3], quad.points[2], end);
    result.points[3] = interpolate(quad.points[3], quad.points[2], start);
    return result;
}

Box union_box(const Box & first, const Box & second) {
    return Box{
        std::min(first.left, second.left),
        std::min(first.top, second.top),
        std::max(first.right, second.right),
        std::max(first.bottom, second.bottom),
        std::min(first.confidence, second.confidence)};
}

std::vector<Box> group_boxes(std::vector<Box> boxes) {
    std::sort(boxes.begin(), boxes.end(), [](const Box & left, const Box & right) {
        if (left.top != right.top) {
            return left.top < right.top;
        }
        return left.left < right.left;
    });
    std::vector<Box> lines;
    for (const Box & candidate : boxes) {
        bool merged = false;
        for (Box & line : lines) {
            const float line_height = line.bottom - line.top;
            const float candidate_height = candidate.bottom - candidate.top;
            const float height = std::max(line_height, candidate_height);
            const float overlap = std::max(
                0.0f,
                std::min(line.bottom, candidate.bottom) - std::max(line.top, candidate.top));
            const float vertical_distance = std::abs(
                (line.top + line.bottom) * 0.5f - (candidate.top + candidate.bottom) * 0.5f);
            const float horizontal_gap = candidate.left > line.right
                                             ? candidate.left - line.right
                                             : line.left > candidate.right
                                                   ? line.left - candidate.right
                                                   : 0.0f;
            if ((overlap >= 0.15f * std::min(line_height, candidate_height) ||
                 vertical_distance <= 0.65f * height) &&
                horizontal_gap <= 2.5f * height) {
                line = union_box(line, candidate);
                merged = true;
                break;
            }
        }
        if (!merged) {
            lines.push_back(candidate);
        }
    }
    std::sort(lines.begin(), lines.end(), [](const Box & left, const Box & right) {
        const float left_center = (left.top + left.bottom) * 0.5f;
        const float right_center = (right.top + right.bottom) * 0.5f;
        if (left_center != right_center) {
            return left_center < right_center;
        }
        return left.left < right.left;
    });
    return lines;
}

class OrtEnvironment final {
public:
    OrtEnvironment() : environment_(ORT_LOGGING_LEVEL_WARNING, "LocalAI") {}
    Ort::Env & get() { return environment_; }

private:
    Ort::Env environment_;
};

Ort::Env & ort_environment() {
    static OrtEnvironment environment;
    return environment.get();
}

bool load_dictionary(const std::wstring & path, std::vector<std::string> & dictionary, std::string & error) {
    if (!path_is_file(path)) {
        error = "PP-OCRv6 character dictionary was not found: ";
        error += std::filesystem::path(path).string();
        return false;
    }
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) {
        error = "PP-OCRv6 character dictionary could not be opened";
        return false;
    }
    std::string contents{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    const bool yaml = std::filesystem::path(path).extension() == L".yml" ||
                      std::filesystem::path(path).extension() == L".yaml";
    dictionary.clear();
    std::istringstream lines(contents);
    std::string line;
    bool in_character_dict = !yaml;
    while (std::getline(lines, line)) {
        line = trim_ascii(line);
        if (yaml) {
            if (line == "character_dict:") {
                in_character_dict = true;
                continue;
            }
            if (!in_character_dict) {
                continue;
            }
            if (line.empty() || line.front() != '-') {
                if (!line.empty() && line.back() == ':') {
                    break;
                }
                continue;
            }
            line = trim_ascii(line.substr(1));
            if (line.size() >= 2 && line.front() == '\'' && line.back() == '\'') {
                line = line.substr(1, line.size() - 2);
                std::string unescaped;
                for (size_t i = 0; i < line.size(); ++i) {
                    if (line[i] == '\'' && i + 1 < line.size() && line[i + 1] == '\'') {
                        unescaped.push_back('\'');
                        ++i;
                    } else {
                        unescaped.push_back(line[i]);
                    }
                }
                line = std::move(unescaped);
            }
        }
        if (line.empty()) {
            continue;
        }
        std::vector<char32_t> codepoints;
        if (!decode_utf8(line, codepoints) || codepoints.size() != 1) {
            continue;
        }
        dictionary.push_back(line);
    }
    if (dictionary.size() < 128) {
        error = "PP-OCRv6 character dictionary is incomplete";
        return false;
    }
    return true;
}

bool tensor_shape(
    const Ort::Value & value,
    std::vector<int64_t> & shape,
    const float *& data,
    size_t & count) {
    if (!value.IsTensor()) {
        return false;
    }
    const Ort::TensorTypeAndShapeInfo info = value.GetTensorTypeAndShapeInfo();
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        return false;
    }
    shape = info.GetShape();
    data = value.GetTensorData<float>();
    count = info.GetElementCount();
    return data != nullptr && !shape.empty() && count != 0;
}

std::vector<Box> extract_detection_boxes(
    const Ort::Value & value,
    uint32_t image_width,
    uint32_t image_height,
    const std::atomic_bool & cancel_requested,
    std::string & error) {
    std::vector<int64_t> shape;
    const float * data = nullptr;
    size_t count = 0;
    if (!tensor_shape(value, shape, data, count) || shape.size() < 3) {
        error = "PP-OCRv6 detector returned an unsupported tensor";
        return {};
    }
    const int64_t map_height = shape[shape.size() - 2];
    const int64_t map_width = shape[shape.size() - 1];
    if (map_height <= 0 || map_width <= 0 ||
        static_cast<uint64_t>(map_height) * static_cast<uint64_t>(map_width) > count) {
        error = "PP-OCRv6 detector returned invalid dimensions";
        return {};
    }
    const size_t width = static_cast<size_t>(map_width);
    const size_t height = static_cast<size_t>(map_height);
    std::vector<unsigned char> active(width * height, 0);
    std::vector<float> probabilities(width * height, 0.0f);
    for (size_t y = 0; y < height; ++y) {
        if (cancel_requested.load(std::memory_order_relaxed)) {
            return {};
        }
        for (size_t x = 0; x < width; ++x) {
            float probability = data[y * width + x];
            if (probability < 0.0f || probability > 1.0f) {
                probability = 1.0f / (1.0f + std::exp(-probability));
            }
            probabilities[y * width + x] = probability;
            active[y * width + x] = probability >= 0.2f ? 1u : 0u;
        }
    }

    constexpr float box_threshold = 0.45f;
    constexpr float unclip_ratio = 1.4f;
    constexpr size_t max_candidates = 3000u;
    constexpr float minimum_size = 3.0f;
    std::vector<Box> boxes;
    std::vector<size_t> stack;
    std::vector<size_t> component;
    std::vector<GeometryPoint> contour_points;
    for (size_t start = 0; start < active.size(); ++start) {
        if (active[start] == 0) {
            continue;
        }
        active[start] = 0;
        stack.clear();
        component.clear();
        stack.push_back(start);
        while (!stack.empty()) {
            const size_t current = stack.back();
            stack.pop_back();
            component.push_back(current);
            const size_t x = current % width;
            const size_t y = current / width;
            for (int offset_y = -1; offset_y <= 1; ++offset_y) {
                for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                    if (offset_x == 0 && offset_y == 0) {
                        continue;
                    }
                    const int neighbor_x = static_cast<int>(x) + offset_x;
                    const int neighbor_y = static_cast<int>(y) + offset_y;
                    if (neighbor_x < 0 || neighbor_y < 0 ||
                        neighbor_x >= static_cast<int>(width) ||
                        neighbor_y >= static_cast<int>(height)) {
                        continue;
                    }
                    const size_t neighbor = static_cast<size_t>(neighbor_y) * width +
                                            static_cast<size_t>(neighbor_x);
                    if (active[neighbor] != 0) {
                        active[neighbor] = 0;
                        stack.push_back(neighbor);
                    }
                }
            }
        }
        if (cancel_requested.load(std::memory_order_relaxed)) {
            return {};
        }
        if (component.size() < 3u) {
            continue;
        }

        // A contour tracer is not available in the small native dependency
        // set, so recover the equivalent outer contour from the active pixel
        // cells and reduce it to a convex polygon. DB text regions are
        // normally long, nearly convex lines; the subsequent minimum-area
        // rectangle and unclip steps match DBPostProcess's quad path.
        contour_points.clear();
        contour_points.reserve(component.size() * 4u);
        for (const size_t current : component) {
            const float x = static_cast<float>(current % width);
            const float y = static_cast<float>(current / width);
            contour_points.push_back({x, y});
            contour_points.push_back({x + 1.0f, y});
            contour_points.push_back({x + 1.0f, y + 1.0f});
            contour_points.push_back({x, y + 1.0f});
        }
        const std::vector<GeometryPoint> hull = convex_hull(std::move(contour_points));
        const RotatedBox initial_box = minimum_area_box(hull);
        if (initial_box.short_side < minimum_size) {
            continue;
        }
        const std::vector<GeometryPoint> initial_polygon(
            initial_box.points.begin(),
            initial_box.points.end());
        const float score = polygon_score_fast(
            probabilities,
            width,
            height,
            initial_polygon);
        if (score < box_threshold) {
            continue;
        }

        const float area = std::abs(polygon_signed_area(initial_polygon));
        const float perimeter = polygon_perimeter(initial_polygon);
        if (!(area > 0.0f) || !(perimeter > 0.0f)) {
            continue;
        }
        const float offset_distance = area * unclip_ratio / perimeter;
        std::vector<GeometryPoint> expanded_polygon = offset_convex_polygon(
            initial_polygon,
            offset_distance);
        if (expanded_polygon.size() < 3u) {
            continue;
        }
        const RotatedBox expanded_box = minimum_area_box(expanded_polygon);
        if (expanded_box.short_side < minimum_size + 2.0f) {
            continue;
        }
        Box box;
        box.confidence = score;
        box.has_quad = true;
        for (size_t index = 0; index < std::size(box.quad.points); ++index) {
            box.quad.points[index].x = std::clamp(
                std::round(expanded_box.points[index].x /
                           static_cast<float>(width) * image_width),
                0.0f,
                static_cast<float>(image_width));
            box.quad.points[index].y = std::clamp(
                std::round(expanded_box.points[index].y /
                           static_cast<float>(height) * image_height),
                0.0f,
                static_cast<float>(image_height));
        }
        box.left = box.right = box.quad.points[0].x;
        box.top = box.bottom = box.quad.points[0].y;
        for (size_t index = 1; index < std::size(box.quad.points); ++index) {
            box.left = std::min(box.left, box.quad.points[index].x);
            box.right = std::max(box.right, box.quad.points[index].x);
            box.top = std::min(box.top, box.quad.points[index].y);
            box.bottom = std::max(box.bottom, box.quad.points[index].y);
        }
        boxes.push_back(box);
        if (boxes.size() >= max_candidates) {
            break;
        }
    }
    std::sort(boxes.begin(), boxes.end(), [](const Box & left, const Box & right) {
        const float left_center = (left.top + left.bottom) * 0.5f;
        const float right_center = (right.top + right.bottom) * 0.5f;
        if (left_center != right_center) {
            return left_center < right_center;
        }
        return left.left < right.left;
    });
    if (boxes.empty()) {
        error = "PP-OCRv6 found no text regions";
    }
    return boxes;
}

void resize_normalized_bgr_interleaved(
    const ImageBuffer & image,
    const Box & crop,
    uint32_t output_width,
    uint32_t output_height,
    std::vector<float> & output) {
    output.resize(static_cast<size_t>(output_width) * output_height * 3u);
    const float crop_width = std::max(1.0f, crop.right - crop.left);
    const float crop_height = std::max(1.0f, crop.bottom - crop.top);
    for (uint32_t y = 0; y < output_height; ++y) {
        const float source_y = crop.top + (static_cast<float>(y) + 0.5f) / output_height * crop_height;
        const uint32_t iy = std::min<uint32_t>(
            image.height - 1,
            static_cast<uint32_t>(std::clamp(source_y, 0.0f, static_cast<float>(image.height - 1))));
        for (uint32_t x = 0; x < output_width; ++x) {
            const float source_x = crop.left + (static_cast<float>(x) + 0.5f) / output_width * crop_width;
            const uint32_t ix = std::min<uint32_t>(
                image.width - 1,
                static_cast<uint32_t>(std::clamp(source_x, 0.0f, static_cast<float>(image.width - 1))));
            const unsigned char * pixel = image.rgb.data() +
                                           (static_cast<size_t>(iy) * image.width + ix) * 3u;
            const size_t base = (static_cast<size_t>(y) * output_width + x) * 3u;
            const float red = static_cast<float>(pixel[0]) / 255.0f;
            const float green = static_cast<float>(pixel[1]) / 255.0f;
            const float blue = static_cast<float>(pixel[2]) / 255.0f;
            output[base + 0] = (blue - 0.485f) / 0.229f;
            output[base + 1] = (green - 0.456f) / 0.224f;
            output[base + 2] = (red - 0.406f) / 0.225f;
        }
    }
}

void resize_rec_bgr_interleaved(
    const ImageBuffer & image,
    const Box & crop,
    uint32_t output_width,
    uint32_t output_height,
    std::vector<float> & output) {
    // PP-OCRv6's RecResizeImg path uses BGR / 255, followed by
    // (value - 0.5) / 0.5. It does not use the detector's ImageNet
    // normalization. Bilinear sampling matches Paddle's cv2.resize path.
    output.resize(static_cast<size_t>(output_width) * output_height * 3u);
    const float crop_width = std::max(1.0f, crop.right - crop.left);
    const float crop_height = std::max(1.0f, crop.bottom - crop.top);
    for (uint32_t y = 0; y < output_height; ++y) {
        const float source_y = crop.top +
                               ((static_cast<float>(y) + 0.5f) * crop_height /
                                    static_cast<float>(output_height)) -
                               0.5f;
        const float clamped_y = std::clamp(
            source_y,
            0.0f,
            static_cast<float>(image.height - 1));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(clamped_y));
        const uint32_t y1 = std::min<uint32_t>(image.height - 1, y0 + 1u);
        const float y_fraction = clamped_y - static_cast<float>(y0);
        for (uint32_t x = 0; x < output_width; ++x) {
            const float source_x = crop.left +
                                   ((static_cast<float>(x) + 0.5f) * crop_width /
                                        static_cast<float>(output_width)) -
                                   0.5f;
            const float clamped_x = std::clamp(
                source_x,
                0.0f,
                static_cast<float>(image.width - 1));
            const uint32_t x0 = static_cast<uint32_t>(std::floor(clamped_x));
            const uint32_t x1 = std::min<uint32_t>(image.width - 1, x0 + 1u);
            const float x_fraction = clamped_x - static_cast<float>(x0);
            const unsigned char * top_left = image.rgb.data() +
                (static_cast<size_t>(y0) * image.width + x0) * 3u;
            const unsigned char * top_right = image.rgb.data() +
                (static_cast<size_t>(y0) * image.width + x1) * 3u;
            const unsigned char * bottom_left = image.rgb.data() +
                (static_cast<size_t>(y1) * image.width + x0) * 3u;
            const unsigned char * bottom_right = image.rgb.data() +
                (static_cast<size_t>(y1) * image.width + x1) * 3u;
            const size_t base = (static_cast<size_t>(y) * output_width + x) * 3u;
            for (uint32_t channel = 0; channel < 3u; ++channel) {
                const float top = static_cast<float>(top_left[2u - channel]) * (1.0f - x_fraction) +
                                  static_cast<float>(top_right[2u - channel]) * x_fraction;
                const float bottom = static_cast<float>(bottom_left[2u - channel]) * (1.0f - x_fraction) +
                                     static_cast<float>(bottom_right[2u - channel]) * x_fraction;
                const float value = (top * (1.0f - y_fraction) + bottom * y_fraction) / 255.0f;
                output[base + channel] = value * 2.0f - 1.0f;
            }
        }
    }
}

void interleaved_to_chw(
    const std::vector<float> & interleaved,
    uint32_t width,
    uint32_t height,
    std::vector<float> & output) {
    output.resize(interleaved.size());
    for (uint32_t channel = 0; channel < 3u; ++channel) {
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                output[(static_cast<size_t>(channel) * height + y) * width + x] =
                    interleaved[(static_cast<size_t>(y) * width + x) * 3u + channel];
            }
        }
    }
}

void resize_normalized_bgr_image(
    const ImageBuffer & image,
    uint32_t output_width,
    uint32_t output_height,
    std::vector<float> & output) {
    std::vector<float> interleaved;
    resize_normalized_bgr_interleaved(
        image,
        Box{0.0f, 0.0f, static_cast<float>(image.width), static_cast<float>(image.height), 1.0f},
        output_width,
        output_height,
        interleaved);
    interleaved_to_chw(interleaved, output_width, output_height, output);
}

class ReadingResolver final {
public:
    explicit ReadingResolver(const std::wstring & path) {
        if (path_is_file(path)) {
            load_fallback(path);
        } else if (path_is_directory(path)) {
            const std::filesystem::path directory(path);
            const std::filesystem::path dicrc = directory / L"dicrc";
            if (path_is_file(dicrc.wstring())) {
                const std::string option = "-d \"" + directory.string() + "\"";
                tagger_.reset(MeCab::Tagger::create(option.c_str()));
                if (tagger_ == nullptr) {
                    error_ = MeCab::getLastError();
                }
            }
        }
    }

    bool valid() const {
        return tagger_ != nullptr || !fallback_.empty();
    }

    const std::string & error() const { return error_; }

    struct Token {
        std::string surface;
        std::string reading;
    };

    std::vector<Token> tokenize(const std::string & text) const {
        if (tagger_ != nullptr) {
            std::vector<Token> result;
            const MeCab::Node * node = tagger_->parseToNode(text.c_str());
            for (; node != nullptr; node = node->next) {
                if (node->surface == nullptr || node->length == 0) {
                    continue;
                }
                Token token;
                token.surface.assign(node->surface, node->length);
                token.reading = find_reading(node->feature);
                result.push_back(std::move(token));
            }
            return result;
        }
        std::vector<Token> result;
        std::vector<char32_t> codepoints;
        if (!decode_utf8(text, codepoints)) {
            return result;
        }
        size_t cursor = 0;
        while (cursor < codepoints.size()) {
            size_t best_length = 0;
            std::string best_reading;
            for (const auto & entry : fallback_) {
                std::vector<char32_t> surface;
                if (!decode_utf8(entry.first, surface) || surface.empty() ||
                    cursor + surface.size() > codepoints.size() ||
                    !std::equal(surface.begin(), surface.end(), codepoints.begin() + cursor)) {
                    continue;
                }
                if (surface.size() > best_length) {
                    best_length = surface.size();
                    best_reading = entry.second;
                }
            }
            if (best_length == 0) {
                best_length = 1;
            }
            std::vector<char32_t> surface(
                codepoints.begin() + cursor,
                codepoints.begin() + cursor + best_length);
            result.push_back(Token{utf32_to_utf8(surface), best_reading});
            cursor += best_length;
        }
        return result;
    }

private:
    void load_fallback(const std::wstring & path) {
        std::ifstream stream(std::filesystem::path(path), std::ios::binary);
        std::string line;
        while (std::getline(stream, line)) {
            line = trim_ascii(line);
            const size_t separator = line.find('\t');
            if (separator == std::string::npos) {
                continue;
            }
            const std::string surface = line.substr(0, separator);
            const std::string reading = katakana_to_hiragana(line.substr(separator + 1));
            if (!surface.empty() && !reading.empty()) {
                fallback_.emplace_back(surface, reading);
            }
        }
    }

    struct TaggerDeleter {
        void operator()(MeCab::Tagger * tagger) const {
            if (tagger != nullptr) {
                MeCab::deleteTagger(tagger);
            }
        }
    };

    std::unique_ptr<MeCab::Tagger, TaggerDeleter> tagger_;
    std::vector<std::pair<std::string, std::string>> fallback_;
    std::string error_;
};

} // namespace

#if defined(_WIN32)

bool load_image_rgb(const std::wstring & path, ImageBuffer & image, std::string & error) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
    }
    if (FAILED(hr)) {
        error = "Windows Imaging Component is unavailable";
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder))) {
        error = "Windows has no decoder for this image format";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        error = "The image has no readable frame";
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat24bppRGB,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom))) {
        error = "The image could not be converted to RGB";
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
        error = "The image has invalid dimensions";
        return false;
    }
    const uint64_t byte_count = static_cast<uint64_t>(width) * height * 3u;
    if (byte_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        byte_count > static_cast<uint64_t>(std::numeric_limits<UINT>::max())) {
        error = "The image is too large to process";
        return false;
    }
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<size_t>(byte_count));
    if (FAILED(converter->CopyPixels(
            nullptr,
            width * 3u,
            static_cast<UINT>(image.rgb.size()),
            image.rgb.data()))) {
        image = {};
        error = "The image pixels could not be read";
        return false;
    }
    return true;
}

#else

bool load_image_rgb(const std::wstring &, ImageBuffer &, std::string & error) {
    error = "Windows image loading is unavailable in this build";
    return false;
}

#endif

struct NativeFuriganaOcr::Impl {
    std::wstring detection_model_path;
    std::wstring recognition_model_path;
    std::wstring dictionary_path;
    std::wstring japanese_dictionary_path;
    uint32_t maximum_image_side = 1280;
    uint32_t threads = 0;
    std::unique_ptr<Ort::Session> detector;
    std::unique_ptr<Ort::Session> recognizer;
    std::vector<std::string> characters;
    std::string detector_input_name;
    std::string recognizer_input_name;
    std::vector<std::string> detector_output_names;
    std::vector<std::string> recognizer_output_names;
    bool loaded = false;
    std::string load_error;

    bool load() {
        if (loaded) {
            return true;
        }
        if (!path_is_file(detection_model_path) || !path_is_file(recognition_model_path)) {
            load_error = "PP-OCRv6 detector and recognizer model files are required";
            return false;
        }
        if (!load_dictionary(dictionary_path, characters, load_error)) {
            return false;
        }
        try {
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            if (threads != 0) {
                options.SetIntraOpNumThreads(static_cast<int>(threads));
                options.SetInterOpNumThreads(1);
            }
            detector = std::make_unique<Ort::Session>(
                ort_environment(), detection_model_path.c_str(), options);
            recognizer = std::make_unique<Ort::Session>(
                ort_environment(), recognition_model_path.c_str(), options);
            Ort::AllocatorWithDefaultOptions allocator;
            if (detector->GetInputCount() == 0 || recognizer->GetInputCount() == 0) {
                load_error = "PP-OCRv6 model has no input tensor";
                return false;
            }
            detector_input_name = detector->GetInputNameAllocated(0, allocator).get();
            recognizer_input_name = recognizer->GetInputNameAllocated(0, allocator).get();
            for (size_t i = 0; i < detector->GetOutputCount(); ++i) {
                detector_output_names.emplace_back(
                    detector->GetOutputNameAllocated(i, allocator).get());
            }
            for (size_t i = 0; i < recognizer->GetOutputCount(); ++i) {
                recognizer_output_names.emplace_back(
                    recognizer->GetOutputNameAllocated(i, allocator).get());
            }
            if (detector_output_names.empty() || recognizer_output_names.empty()) {
                load_error = "PP-OCRv6 model has no output tensor";
                return false;
            }
            if (trace_enabled()) {
                std::cerr << "PP-OCR detector inputs=" << detector->GetInputCount()
                          << " outputs=" << detector->GetOutputCount() << "\n";
                for (const std::string & name : detector_output_names) {
                    std::cerr << "  det output " << name << "\n";
                }
                std::cerr << "PP-OCR recognizer inputs=" << recognizer->GetInputCount()
                          << " outputs=" << recognizer->GetOutputCount() << "\n";
                for (const std::string & name : recognizer_output_names) {
                    std::cerr << "  rec output " << name << "\n";
                }
                std::cerr << "PP-OCR dictionary entries=" << characters.size() << "\n";
            }
            ReadingResolver resolver(japanese_dictionary_path);
            if (!resolver.valid()) {
                load_error = "Japanese reading dictionary could not be loaded";
                if (!resolver.error().empty()) {
                    load_error += ": ";
                    load_error += resolver.error();
                }
                return false;
            }
            loaded = true;
            return true;
        } catch (const Ort::Exception & exception) {
            load_error = "ONNX Runtime could not load PP-OCRv6: ";
            load_error += exception.what();
            return false;
        }
    }

    LocalAIStatus run(
        const ImageBuffer & image,
        const std::atomic_bool & cancel_requested,
        FuriganaOutput & output,
        std::string & error) {
        if (image.width == 0 || image.height == 0 || image.rgb.size() !=
                static_cast<size_t>(image.width) * image.height * 3u) {
            error = "the decoded image is invalid";
            return LOCAL_AI_INVALID_ARGUMENT;
        }
        if (!load()) {
            error = load_error;
            return LOCAL_AI_MODEL_LOAD_FAILED;
        }
        const float original_side = static_cast<float>(std::max(image.width, image.height));
        const float requested_side = static_cast<float>(maximum_image_side == 0 ? 1280 : maximum_image_side);
        const float resize_scale = std::min(1.0f, requested_side / original_side);
        uint32_t resized_width = std::max<uint32_t>(32u, static_cast<uint32_t>(std::round(image.width * resize_scale)));
        uint32_t resized_height = std::max<uint32_t>(32u, static_cast<uint32_t>(std::round(image.height * resize_scale)));
        resized_width = (resized_width + 31u) / 32u * 32u;
        resized_height = (resized_height + 31u) / 32u * 32u;
        const float scale_x = static_cast<float>(image.width) / resized_width;
        const float scale_y = static_cast<float>(image.height) / resized_height;

        std::vector<float> detector_input;
        resize_normalized_bgr_image(image, resized_width, resized_height, detector_input);
        const int64_t detector_shape[] = {
            1,
            3,
            static_cast<int64_t>(resized_height),
            static_cast<int64_t>(resized_width)};
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator,
            OrtMemTypeDefault);
        Ort::Value detector_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            detector_input.data(),
            detector_input.size(),
            detector_shape,
            std::size(detector_shape));
        std::vector<const char *> detector_outputs;
        detector_outputs.reserve(detector_output_names.size());
        for (const std::string & name : detector_output_names) {
            detector_outputs.push_back(name.c_str());
        }
        std::vector<Ort::Value> detector_values;
        try {
            Ort::RunOptions run_options;
            const char * detector_inputs[] = {detector_input_name.c_str()};
            detector_values = detector->Run(
                run_options,
                detector_inputs,
                &detector_tensor,
                1,
                detector_outputs.data(),
                detector_outputs.size());
        } catch (const Ort::Exception & exception) {
            error = "PP-OCRv6 detection failed: ";
            error += exception.what();
            return LOCAL_AI_INFERENCE_FAILED;
        }
        if (cancel_requested.load(std::memory_order_relaxed)) {
            error = "furigana OCR was cancelled";
            return LOCAL_AI_CANCELLED;
        }
        std::vector<Box> boxes;
        for (const Ort::Value & value : detector_values) {
            std::string detection_error;
            boxes = extract_detection_boxes(
                value,
                resized_width,
                resized_height,
                cancel_requested,
                detection_error);
            if (!boxes.empty()) {
                break;
            }
        }
        if (trace_enabled()) {
            std::cerr << "PP-OCR detector boxes=" << boxes.size() << "\n";
            for (const Box & box : boxes) {
                std::cerr << "  box " << box.left << "," << box.top << " - "
                          << box.right << "," << box.bottom << " conf=" << box.confidence << "\n";
            }
        }
        if (boxes.empty()) {
            if (error.empty()) {
                error = "PP-OCRv6 found no text regions";
            }
            return cancel_requested.load(std::memory_order_relaxed)
                       ? LOCAL_AI_CANCELLED
                       : LOCAL_AI_INFERENCE_FAILED;
        }

        ReadingResolver resolver(japanese_dictionary_path);
        if (!resolver.valid()) {
            error = "Japanese reading dictionary could not be loaded";
            if (!resolver.error().empty()) {
                error += ": ";
                error += resolver.error();
            }
            return LOCAL_AI_MODEL_LOAD_FAILED;
        }

        output = {};
        bool first_line = true;
        for (Box & box : boxes) {
            if (cancel_requested.load(std::memory_order_relaxed)) {
                error = "furigana OCR was cancelled";
                return LOCAL_AI_CANCELLED;
            }
            const float scaled_left = box.left * scale_x;
            const float scaled_right = box.right * scale_x;
            const float scaled_top = box.top * scale_y;
            const float scaled_bottom = box.bottom * scale_y;
            if (box.has_quad) {
                for (LocalAIPoint & point : box.quad.points) {
                    point.x *= scale_x;
                    point.y *= scale_y;
                }
            }
            // A small crop margin keeps anti-aliased edge pixels available to
            // the recognizer. DBPostProcess's unclip expansion above owns the
            // actual text-region geometry.
            constexpr float crop_padding = 2.0f;
            box.left = std::clamp(
                scaled_left - crop_padding,
                0.0f,
                static_cast<float>(image.width - 1));
            box.right = std::clamp(
                scaled_right + crop_padding,
                box.left + 1.0f,
                static_cast<float>(image.width));
            box.top = std::clamp(
                scaled_top - crop_padding,
                0.0f,
                static_cast<float>(image.height - 1));
            box.bottom = std::clamp(
                scaled_bottom + crop_padding,
                box.top + 1.0f,
                static_cast<float>(image.height));

            const float crop_width = box.right - box.left;
            const float crop_height = box.bottom - box.top;
            const uint32_t resized_recognition_width = std::clamp<uint32_t>(
                static_cast<uint32_t>(std::ceil(crop_width / crop_height * 48.0f)),
                16u,
                3200u);
            const uint32_t recognition_width = std::max<uint32_t>(
                320u,
                resized_recognition_width);
            std::vector<float> recognition_input;
            resize_rec_bgr_interleaved(
                image,
                box,
                resized_recognition_width,
                48u,
                recognition_input);
            std::vector<float> padded(
                static_cast<size_t>(3u) * 48u * recognition_width,
                0.0f);
            for (uint32_t channel = 0; channel < 3u; ++channel) {
                for (uint32_t y = 0; y < 48u; ++y) {
                    for (uint32_t x = 0; x < resized_recognition_width; ++x) {
                        padded[(static_cast<size_t>(channel) * 48u + y) * recognition_width + x] =
                            recognition_input[(static_cast<size_t>(y) * resized_recognition_width + x) * 3u + channel];
                    }
                }
            }
            const int64_t recognition_shape[] = {
                1,
                3,
                48,
                static_cast<int64_t>(recognition_width)};
            Ort::Value recognition_tensor = Ort::Value::CreateTensor<float>(
                memory_info,
                padded.data(),
                padded.size(),
                recognition_shape,
                std::size(recognition_shape));
            std::vector<const char *> recognition_outputs;
            recognition_outputs.reserve(recognizer_output_names.size());
            for (const std::string & name : recognizer_output_names) {
                recognition_outputs.push_back(name.c_str());
            }
            std::vector<Ort::Value> recognition_values;
            try {
                Ort::RunOptions run_options;
                const char * recognizer_inputs[] = {recognizer_input_name.c_str()};
                recognition_values = recognizer->Run(
                    run_options,
                    recognizer_inputs,
                    &recognition_tensor,
                    1,
                    recognition_outputs.data(),
                    recognition_outputs.size());
            } catch (const Ort::Exception & exception) {
                error = "PP-OCRv6 recognition failed: ";
                error += exception.what();
                return LOCAL_AI_INFERENCE_FAILED;
            }
            RecognizedLine line;
            std::vector<int64_t> output_shape;
            const float * output_data = nullptr;
            size_t output_count = 0;
            bool found_ctc = false;
            for (const Ort::Value & value : recognition_values) {
                std::vector<int64_t> candidate_shape;
                const float * candidate_data = nullptr;
                size_t candidate_count = 0;
                if (!tensor_shape(value, candidate_shape, candidate_data, candidate_count) ||
                    candidate_shape.size() != 3) {
                    continue;
                }
                if (trace_enabled()) {
                    std::cerr << "  rec tensor shape=";
                    for (const int64_t dimension : candidate_shape) {
                        std::cerr << dimension << " ";
                    }
                    std::cerr << " count=" << candidate_count << "\n";
                }
                const int64_t dimension_one = candidate_shape[1];
                const int64_t dimension_two = candidate_shape[2];
                if (dimension_one <= 0 || dimension_two <= 0 ||
                    std::max(dimension_one, dimension_two) < 128) {
                    continue;
                }
                output_shape = std::move(candidate_shape);
                output_data = candidate_data;
                output_count = candidate_count;
                found_ctc = true;
                break;
            }
            if (!found_ctc) {
                error = "PP-OCRv6 recognizer did not expose CTC logits";
                return LOCAL_AI_UNSUPPORTED;
            }
            const bool channel_last = output_shape[2] >= output_shape[1];
            const size_t timesteps = static_cast<size_t>(channel_last ? output_shape[1] : output_shape[2]);
            const size_t classes = static_cast<size_t>(channel_last ? output_shape[2] : output_shape[1]);
            if (classes < characters.size() + 1u || timesteps == 0 ||
                timesteps * classes > output_count) {
                error = "PP-OCRv6 recognizer CTC dimensions do not match its dictionary";
                return LOCAL_AI_UNSUPPORTED;
            }
            int previous_class = -1;
            std::vector<char32_t> line_codepoints;
            std::vector<AlignedCharacter> aligned;
            for (size_t timestep = 0; timestep < timesteps; ++timestep) {
                if (cancel_requested.load(std::memory_order_relaxed)) {
                    error = "furigana OCR was cancelled";
                    return LOCAL_AI_CANCELLED;
                }
                size_t best_class = 0;
                float best_logit = -std::numeric_limits<float>::infinity();
                for (size_t class_index = 0; class_index < classes; ++class_index) {
                    const size_t index = channel_last
                                             ? timestep * classes + class_index
                                             : class_index * timesteps + timestep;
                    const float logit = output_data[index];
                    if (logit > best_logit) {
                        best_logit = logit;
                        best_class = class_index;
                    }
                }
                const int class_index = static_cast<int>(best_class);
                // PaddleOCR's CTCLabelDecode prepends the blank token at index 0.
                // Some exported PP-OCRv6 graphs retain one additional terminal
                // class, so treat every class outside the character range as blank.
                if (best_class == 0 || best_class > characters.size()) {
                    previous_class = -1;
                    continue;
                }
                if (class_index == previous_class && !aligned.empty()) {
                    aligned.back().end = static_cast<float>(timestep + 1u) / timesteps;
                    aligned.back().confidence = std::max(
                        aligned.back().confidence,
                        1.0f / (1.0f + std::exp(-best_logit)));
                    continue;
                }
                std::vector<char32_t> decoded;
                const size_t character_index = best_class - 1u;
                if (character_index >= characters.size() ||
                    !decode_utf8(characters[character_index], decoded) || decoded.size() != 1) {
                    previous_class = -1;
                    continue;
                }
                aligned.push_back(AlignedCharacter{
                    decoded.front(),
                    static_cast<float>(timestep) / timesteps,
                    static_cast<float>(timestep + 1u) / timesteps,
                    1.0f / (1.0f + std::exp(-best_logit))});
                line_codepoints.push_back(decoded.front());
                previous_class = class_index;
            }
            line.utf8 = utf32_to_utf8(line_codepoints);
            line.characters = std::move(aligned);
            if (trace_enabled()) {
                std::cerr << "  rec line bytes=" << line.utf8.size() << " text=" << line.utf8 << "\n";
            }
            if (line.utf8.empty()) {
                continue;
            }
            output.regions.push_back(OcrRegion{
                line.utf8,
                make_quad(box),
                box.confidence,
                LOCAL_AI_TEXT_REGION_DETECTED});
            if (!first_line) {
                output.text.push_back('\n');
            }
            first_line = false;
            output.text += line.utf8;
            const std::vector<ReadingResolver::Token> tokens = resolver.tokenize(line.utf8);
            std::vector<char32_t> recognized_codepoints;
            if (!decode_utf8(line.utf8, recognized_codepoints)) {
                continue;
            }
            size_t codepoint_cursor = 0;
            for (const ReadingResolver::Token & token : tokens) {
                std::vector<char32_t> surface;
                if (!decode_utf8(token.surface, surface) || surface.empty() ||
                    codepoint_cursor + surface.size() > recognized_codepoints.size() ||
                    !std::equal(surface.begin(), surface.end(), recognized_codepoints.begin() + codepoint_cursor)) {
                    continue;
                }
                const size_t start_index = codepoint_cursor;
                codepoint_cursor += surface.size();
                if (!contains_kanji(token.surface) || token.reading.empty() ||
                    start_index >= line.characters.size()) {
                    continue;
                }
                const size_t end_index = std::min(codepoint_cursor, line.characters.size());
                if (end_index <= start_index) {
                    continue;
                }
                float start = line.characters[start_index].start;
                float end = line.characters[end_index - 1].end;
                float confidence = 0.0f;
                for (size_t index = start_index; index < end_index; ++index) {
                    confidence += line.characters[index].confidence;
                }
                confidence /= static_cast<float>(end_index - start_index);
                const LocalAIQuad resized_quad = make_quad(box);
                LocalAIQuad image_quad = resized_quad;
                for (LocalAIPoint & point : image_quad.points) {
                    point.x *= 1.0f;
                    point.y *= 1.0f;
                }
                output.tokens.push_back(FuriganaToken{
                    token.surface,
                    token.reading,
                    interpolate_quad(image_quad, start, end),
                    confidence,
                    1u});
            }
        }
        if (output.text.empty()) {
            error = "PP-OCRv6 recognized no text";
            return LOCAL_AI_INFERENCE_FAILED;
        }
        return LOCAL_AI_OK;
    }
};

NativeFuriganaOcr::NativeFuriganaOcr(
    std::wstring detection_model_path,
    std::wstring recognition_model_path,
    std::wstring dictionary_path,
    std::wstring japanese_dictionary_path,
    uint32_t maximum_image_side,
    uint32_t threads)
    : impl_(std::make_unique<Impl>()) {
    impl_->detection_model_path = std::move(detection_model_path);
    impl_->recognition_model_path = std::move(recognition_model_path);
    impl_->dictionary_path = std::move(dictionary_path);
    impl_->japanese_dictionary_path = std::move(japanese_dictionary_path);
    impl_->maximum_image_side = maximum_image_side;
    impl_->threads = threads;
}

NativeFuriganaOcr::~NativeFuriganaOcr() = default;

LocalAIStatus NativeFuriganaOcr::run(
    const ImageBuffer & image,
    const char *,
    const std::atomic_bool & cancel_requested,
    FuriganaOutput & output,
    std::string & error) {
    if (!impl_) {
        error = "native furigana backend is unavailable";
        return LOCAL_AI_INVALID_STATE;
    }
    return impl_->run(image, cancel_requested, output, error);
}

} // namespace localai
