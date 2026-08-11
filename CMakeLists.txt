cmake_minimum_required(VERSION 3.24)

project(LocalOCRTranslationStack VERSION 0.1.0 LANGUAGES C CXX)

if (NOT WIN32)
    message(FATAL_ERROR "LocalOCRTranslationStack currently targets Windows only")
endif()

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
        "Use the static MSVC runtime for a self-contained redistribution" FORCE)
endif()

set(LOCALAI_LLAMA_CPP_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llama.cpp"
    CACHE PATH "Pinned llama.cpp source tree prepared by scripts/setup_dependencies.ps1")
if (NOT EXISTS "${LOCALAI_LLAMA_CPP_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Pinned llama.cpp is missing. Run scripts/setup_dependencies.ps1 first, "
        "or set LOCALAI_LLAMA_CPP_SOURCE_DIR to a checked-out compatible tree.")
endif()

option(LOCALAI_ENABLE_FURIGANA "Build the native PP-OCRv6/furigana backend" ON)
set(LOCALAI_ONNXRUNTIME_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime"
    CACHE PATH "Pinned ONNX Runtime package prepared by scripts/setup_dependencies.ps1")
set(LOCALAI_MECAB_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mecab"
    CACHE PATH "Pinned MeCab source tree prepared by scripts/setup_dependencies.ps1")

if (LOCALAI_ENABLE_FURIGANA)
    if (NOT EXISTS "${LOCALAI_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h" OR
        NOT EXISTS "${LOCALAI_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib" OR
        NOT EXISTS "${LOCALAI_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
        message(FATAL_ERROR
            "ONNX Runtime 1.24.4 is missing. Run scripts/setup_dependencies.ps1 "
            "or set LOCALAI_ENABLE_FURIGANA=OFF.")
    endif()
    if (NOT EXISTS "${LOCALAI_MECAB_SOURCE_DIR}/src/mecab.h")
        message(FATAL_ERROR
            "Pinned MeCab source is missing. Run scripts/setup_dependencies.ps1 "
            "or set LOCALAI_ENABLE_FURIGANA=OFF.")
    endif()

    add_library(localai_onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(localai_onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${LOCALAI_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        IMPORTED_LOCATION "${LOCALAI_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${LOCALAI_ONNXRUNTIME_ROOT}/include")

    set(LOCALAI_MECAB_SOURCES
        char_property.cpp
        connector.cpp
        context_id.cpp
        dictionary.cpp
        dictionary_compiler.cpp
        dictionary_generator.cpp
        dictionary_rewriter.cpp
        eval.cpp
        feature_index.cpp
        iconv_utils.cpp
        lbfgs.cpp
        learner.cpp
        learner_tagger.cpp
        libmecab.cpp
        nbest_generator.cpp
        param.cpp
        string_buffer.cpp
        tagger.cpp
        tokenizer.cpp
        utils.cpp
        viterbi.cpp
        writer.cpp)
    list(TRANSFORM LOCALAI_MECAB_SOURCES
        PREPEND "${LOCALAI_MECAB_SOURCE_DIR}/src/")
    add_library(localai_mecab STATIC ${LOCALAI_MECAB_SOURCES})
    # MeCab 0.996 is intentionally built in its original pre-C++17 dialect;
    # its headers still use the removed `register` storage specifier.
    target_compile_features(localai_mecab PRIVATE cxx_std_14)
    target_include_directories(localai_mecab PUBLIC "${LOCALAI_MECAB_SOURCE_DIR}/src")
    target_compile_definitions(localai_mecab PRIVATE
        DIC_VERSION=102
        HAVE_GETENV
        HAVE_WINDOWS_H
        HAVE_UNSIGNED_LONG_LONG_INT
        MECAB_USE_UTF8_ONLY
        MECAB_USE_THREAD
        PACKAGE=\"mecab\"
        VERSION=\"0.996\"
        _HAS_AUTO_PTR_ETC=1
        _HAS_DEPRECATED_ADAPTOR_TYPEDEFS=1
        "MECAB_DEFAULT_RC=\"\""
        _CRT_SECURE_NO_WARNINGS)
    if (MSVC)
        target_compile_options(localai_mecab PRIVATE /utf-8)
    endif()
endif()

# The application uses the public llama.cpp/libmtmd APIs only. Keep the upstream
# build focused on the static libraries needed by the DLL and the Vulkan backend.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(LLAMA_SUBPROCESS OFF CACHE BOOL "" FORCE)
set(MTMD_VIDEO OFF CACHE BOOL "" FORCE)
set(LLAMA_ALL_WARNINGS_3RD_PARTY OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(GGML_SYCL OFF CACHE BOOL "" FORCE)
set(GGML_RPC OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN ON CACHE BOOL "" FORCE)
set(GGML_VULKAN_CHECK_RESULTS OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN_VALIDATE OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${LOCALAI_LLAMA_CPP_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/llama.cpp"
    EXCLUDE_FROM_ALL)

add_library(LocalAILib SHARED
    src/dll/local_ai_dll.cpp
    src/engine/local_ai_engine.cpp)
if (LOCALAI_ENABLE_FURIGANA)
    target_sources(LocalAILib PRIVATE src/engine/furigana_ocr.cpp)
else()
    target_sources(LocalAILib PRIVATE src/engine/furigana_ocr_disabled.cpp)
endif()
target_compile_features(LocalAILib PRIVATE cxx_std_20)
target_compile_definitions(LocalAILib PRIVATE LOCALAI_BUILD_DLL NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(LocalAILib PRIVATE /utf-8)
endif()
target_include_directories(LocalAILib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/engine")
target_link_libraries(LocalAILib PRIVATE mtmd llama ggml)
target_link_libraries(LocalAILib PRIVATE windowscodecs ole32)
if (LOCALAI_ENABLE_FURIGANA)
    target_link_libraries(LocalAILib PRIVATE localai_onnxruntime localai_mecab)
    add_custom_command(TARGET LocalAILib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LOCALAI_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:LocalAILib>/onnxruntime.dll")
    install(FILES "${LOCALAI_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll" DESTINATION .)
endif()

add_executable(LocalAIApp WIN32 src/app/main.cpp)
target_compile_features(LocalAIApp PRIVATE cxx_std_20)
target_compile_definitions(LocalAIApp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(LocalAIApp PRIVATE /utf-8)
endif()
target_include_directories(LocalAIApp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(LocalAIApp PRIVATE LocalAILib)
target_link_libraries(LocalAIApp PRIVATE d2d1 dwrite windowscodecs comctl32 ole32 shell32 advapi32 user32 gdi32)

option(LOCALAI_BUILD_TESTS "Build LocalAITests" ON)
if (LOCALAI_BUILD_TESTS)
    enable_testing()
    add_executable(LocalAITests tests/test_main.cpp)
    target_compile_features(LocalAITests PRIVATE cxx_std_20)
    if (MSVC)
        target_compile_options(LocalAITests PRIVATE /utf-8)
    endif()
    target_include_directories(LocalAITests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(LocalAITests PRIVATE LocalAILib)
    add_test(NAME LocalAITests COMMAND LocalAITests)

    add_executable(LocalAICAbiSmoke tests/c_abi_smoke.c)
    target_include_directories(LocalAICAbiSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(LocalAICAbiSmoke PRIVATE LocalAILib)
    add_test(NAME LocalAICAbiSmoke COMMAND LocalAICAbiSmoke)
    set_tests_properties(LocalAITests PROPERTIES SKIP_RETURN_CODE 77)
endif()

set(_localai_output_targets LocalAILib LocalAIApp)
if (LOCALAI_BUILD_TESTS)
    list(APPEND _localai_output_targets LocalAITests LocalAICAbiSmoke)
endif()
foreach (_target IN LISTS _localai_output_targets)
    set_target_properties(${_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    foreach (_config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${_config}" _config_upper)
        set_target_properties(${_target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/bin/${_config}"
            LIBRARY_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/bin/${_config}"
            ARCHIVE_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/lib/${_config}")
    endforeach()
endforeach()

install(TARGETS LocalAILib LocalAIApp RUNTIME DESTINATION . LIBRARY DESTINATION . ARCHIVE DESTINATION .)
if (LOCALAI_BUILD_TESTS)
    # Tests are useful in the build tree but are intentionally not installed.
endif()
install(FILES include/local_ai.h DESTINATION include)
install(FILES README.md THIRD_PARTY_NOTICES.md DESTINATION .)
install(DIRECTORY licenses DESTINATION .)
install(FILES models/README.md DESTINATION models)
install(FILES models/japanese/README.md models/japanese/readings.tsv DESTINATION models/japanese)
