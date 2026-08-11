# Local AI OCR Translator for Windows

This repository is a native Windows prototype with three components:

* `LocalAILib.dll` exposes a small versioned C ABI for local OCR and translation.
* `LocalAIApp.exe` is a Win32/WIC/Direct2D desktop front end.
* `LocalAITests` and `LocalAICAbiSmoke` exercise the ABI, Unicode/error paths,
  native PP-OCRv6 furigana calls and optional model-backed smoke calls.

The runtime has no Python or network dependency. Model files are optional
external assets and are never downloaded by the executable.

## Current implementation status

The implementation is intentionally conservative about model support. The
official OvisOCR2 release is safetensors-only, so the setup script uses the
following pinned community GGUF conversion and its matching BF16 projector:

* Official OvisOCR2: `ATH-MaaS/OvisOCR2@65c619d374b55d4152e85150fc1b003700bc1f0c`
* Ovis GGUF conversion: `bartowski/ATH-MaaS_OvisOCR2-GGUF@ab22420f3d44201d3aa5a62ca49a665a46b507e9`
* Translation: `tencent/Hy-MT2-1.8B-GGUF@1cd5208700acedef4ef93019b6cfc148b8522d45`
* PP-OCRv6 detector: `PaddlePaddle/PP-OCRv6_medium_det_onnx@61323801669c338b7891481ec7bac61ce31b576a`
* PP-OCRv6 recognizer: `PaddlePaddle/PP-OCRv6_medium_rec_onnx@50c7eacafc52fa7bcf4194e8cd08e46f8558504b`

The compatibility audit was performed against llama.cpp
`876a4321163249c43ca4e986818fab5ab081f282` (2026-08-01). That revision has
the `qwen35` model architecture, IMROPE handling, the `qwen3vl_merger`
projector path used by the verified Ovis BF16 projector, and Hunyuan Dense
support used by Hy-MT2. libmtmd's model list does not have a product-specific
`OvisOCR2` entry, but the GGUF metadata pair maps to its generic Qwen3-VL
merger implementation. No llama.cpp or libmtmd source patch is carried in this
repository. If a future revision rejects this pair, the backend returns an
explicit model-load error rather than substituting another model.

The native PP-OCRv6 path is implemented without Python: ONNX Runtime runs the
official detector and recognizer, the DLL performs CTC greedy decoding and
character/timestep alignment, and static MeCab provides Japanese word
readings. The GUI's `Furigana` action draws those readings above the matching
image regions with DirectWrite/Direct2D. A full UniDic install is optional for
the small smoke fixture; the checked-in TSV is deliberately not a production
lexicon.

## Requirements

* Windows 10/11 x64
* Visual Studio 2022 C++ workload
* CMake 3.24 or newer and Ninja or the Visual Studio generator
* Vulkan SDK with `glslc` on `PATH` during the build
* A Vulkan driver at runtime

The default CMake presets use the Visual Studio 17 2022 x64 generator and the
static MSVC runtime. The Vulkan loader is expected to come from the graphics
driver; the application does not install a driver or require administrator
privileges.

## Build

From a PowerShell prompt in the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_dependencies.ps1
cmake --preset release
cmake --build --preset build-release --config Release
ctest --preset test-release
cmake --install build\release --config Release
```

The dependency script checks out the exact llama.cpp commit above. It does not
download model files, but it explicitly prepares the pinned ONNX Runtime 1.24.4
package and MeCab 0.996 source needed by the native furigana backend. To
prepare the Debug configuration, use `debug`,
`build-debug` and `test-debug` in the same commands. The output directory for
the in-tree build is `build\release\bin\Release`; the install directory is
`dist\release`.

For a dependency checkout outside this repository, configure with:

```powershell
cmake -S . -B build\release -G "Visual Studio 17 2022" -A x64 `
  -DLOCALAI_LLAMA_CPP_SOURCE_DIR=C:\path\to\llama.cpp
cmake --build build\release --config Release
```

The checkout must be verified at the pinned revision and must not contain
unreviewed local changes.

## Model setup

Run model acquisition explicitly and inspect the license files before copying
assets into a product distribution:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_models.ps1
```

Optional low-memory choices are:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_models.ps1 `
  -OcrQuantization Q6_K_L -TranslationQuantization Q4_K_M
```

The script uses immutable Hugging Face revisions, verifies raw GGUF and
PP-OCRv6 SHA-256 values and byte sizes, refuses to overwrite an existing
mismatched file, and copies the upstream Ovis/Hy license text beside the
assets. It does not fetch
the original Ovis safetensors weights. The default files are:

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `ATH-MaaS_OvisOCR2-Q6_K.gguf` | 669,712,512 | `7f86d22d9e3e359a4156c1a2f01bbd35f07bcc33dc33ddd92aee2812db101705` |
| `ATH-MaaS_OvisOCR2-Q6_K_L.gguf` | 731,295,872 | `d9c0d84216e60c41e643d4f6a3e9e2e33d681b84445ab0ad0df522f43ff55869` |
| `ATH-MaaS_OvisOCR2-Q4_K_M.gguf` | 557,867,136 | `3786d230ceb8f217abdfb8ea8adba975827595053ad5087cb5502898d6a8a68e` |
| `mmproj-ATH-MaaS_OvisOCR2-bf16.gguf` | 207,346,336 | `842015fc8bb09d8b953a04b1ba329510e7259081422a9dab50f41ce4fac430a1` |
| `Hy-MT2-1.8B-Q6_K.gguf` | 1,474,785,120 | `d98fe604dec1f28f58f80d7d560f7177e584d3b8e5835862687660e5ff97cb40` |
| `Hy-MT2-1.8B-Q4_K_M.gguf` | 1,133,080,448 | `dc5f44fcf1fa496ee7ad725982c0c8c553a4de00259b53af84c4b89fb0c06699` |
| `PP-OCRv6_medium_det.onnx` | 62,032,837 | `eb13b44b25bb36f89528b68720AF8A61d9cf381176107f465db1757b65d086e1` |
| `PP-OCRv6_medium_rec.onnx` | 76,554,979 | `9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba` |

The application searches `models\ocr` and `models\translation` next to the
executable and in its parent directories, which covers both the Visual Studio
build tree and an installed distribution. Set `LOCALAI_MODELS_DIR` to an
explicit model root when desired. It prefers Q6, then the documented
alternatives. Keep the Ovis model and BF16 projector from the same pinned
conversion release. The model file sizes above are not a VRAM measurement.

The PP-OCRv6 YAML files are pinned with their ONNX revisions and checksums in
`scripts/setup_models.ps1`. To install the explicit Japanese dictionary asset,
run `.\scripts\setup_models.ps1 -IncludeJapaneseDictionary`; this downloads the
version-pinned UniDic `unidic-cwj-202512` archive and records its computed
checksum beside the extracted dictionary. The fallback
`models\japanese\readings.tsv` is only for smoke testing.

## DLL ABI and ownership

The public header is `include/local_ai.h`. It contains no STL, Vulkan or
llama.cpp types. `LocalAIConfig.struct_size` permits compatible extension and
unknown future fields are ignored. `local_ai_create` copies all path strings;
the caller may release its path buffers after the function returns. A caller
must keep the `LocalAIEngine` alive until every call using it has returned and
must serialize inference calls. `local_ai_cancel` is safe to call from another
thread and requests cancellation at backend/decode boundaries.

Text callbacks receive UTF-8 bytes that are valid only for the duration of the
callback. Copy them if they are needed later. Callback exceptions must not cross
the DLL boundary. The pointer returned by `local_ai_get_last_error` is a
thread-local UTF-16 copy, valid until the calling thread invokes that function
again; it is safe across threads while the engine remains alive. An empty
string means no recorded error. `local_ai_trim_memory` releases loaded model
contexts when no operation is active; otherwise it requests cancellation.

## Application behavior

The GUI uses `IFileOpenDialog`, WIC and Direct2D. It supports PNG, JPEG, BMP,
TIFF and WebP when a Windows decoder is installed, plus drag-and-drop. OCR runs
on a worker thread and leaves the extracted text editable. The source dropdown
is an OCR hint: it does not filter out names, numbers or foreign-language
fragments. Translation uses the Hy-MT2 chat-template path, only translates the
edited OCR text, preserves requested structural tokens in its prompt, and
splits large paragraph-oriented inputs before reassembling them.

The `Furigana` action runs the separate PP-OCRv6 line detector/recognizer. Its
output preserves detected line breaks in the extracted editor and returns
surface/reading tokens through the DLL. Character boxes are estimated from CTC
time ranges; the preview now draws cyan outlines for every detected text region,
magenta outlines for kanji tokens, and the generated readings above them. The
native detector path now performs DB thresholding, component-contour recovery,
box scoring, minimum-area quadrilateral fitting and the pinned `unclip_ratio`
expansion (`1.4`) before mapping regions to image coordinates. Recognition crops
remain axis-aligned, so vertical Japanese and highly stylized manga layouts
still need a later orientation-aware recognition pass.

The GUI stores source/target selections and window dimensions under
`HKCU\Software\LocalAIPrototype`. It does not require elevation and does not
send images or text outside the process. OCR output uses readable blank-line
separation for paragraphs and a wider internal block boundary when Ovis emits a
visual bounding-box marker; translation sends each recovered block separately
and rejoins the results with the same boundaries. Lines within a block remain
together. If Ovis returns a longer result with no paragraph boundary, the
engine performs one additional layout-only OCR pass that asks it to keep HUD,
dialogue, columns and other independent regions separate. A short one-line
result stays on the fast path because it may legitimately be a single block.

## Memory and VRAM measurement

The default configuration is Ovis Q6 plus BF16 projector, Hy-MT2 Q6,
translation context 2,048, OCR context 8,192, `gpu_layers=-1`, and sequential
model loading. When GPU offload is enabled, the engine selects the primary
Vulkan device instead of silently spreading this small model across every
installed GPU; this avoids cross-device transfer latency and makes the VRAM
measurement meaningful for one device. The engine samples
`ggml_backend_dev_memory` for that Vulkan GPU when the backend reports reliable
totals, tracks baseline-relative current and peak allocations, and enforces the
configured 6 GiB ceiling when reliable information is available. The GUI and
`LocalAIMemoryInfo` expose the peak.

Run the optional smoke test only on a machine with the models and a Vulkan
driver:

```powershell
$env:LOCALAI_RUN_SMOKE = '1'
$env:LOCALAI_TEST_IMAGE = 'C:\test-images\mixed-page.png'
$env:LOCALAI_OCR_MODEL = (Resolve-Path .\models\ocr\ATH-MaaS_OvisOCR2-Q6_K.gguf)
$env:LOCALAI_PROJECTOR = (Resolve-Path .\models\ocr\mmproj-ATH-MaaS_OvisOCR2-bf16.gguf)
$env:LOCALAI_TRANSLATION_MODEL = (Resolve-Path .\models\translation\Hy-MT2-1.8B-Q6_K.gguf)
ctest --preset test-release -R LocalAITests --output-on-failure
```

To benchmark a particular image without running the translation smoke calls:

```powershell
$env:LOCALAI_RUN_SMOKE = '1'
$env:LOCALAI_BENCHMARK_IMAGE = (Resolve-Path .\ii.png)
$env:LOCALAI_OCR_MODEL = (Resolve-Path .\models\ocr\ATH-MaaS_OvisOCR2-Q6_K.gguf)
$env:LOCALAI_PROJECTOR = (Resolve-Path .\models\ocr\mmproj-ATH-MaaS_OvisOCR2-bf16.gguf)
$env:LOCALAI_TRANSLATION_MODEL = (Resolve-Path .\models\translation\Hy-MT2-1.8B-Q6_K.gguf)
& .\build\release\bin\Release\LocalAITests.exe
```

On the development machine, `ii.png` measured approximately 1.17 s for the
first OCR model/projector load and 0.60 s for OCR inference on the primary AMD
Radeon RX 9070, with a reported peak allocation of 1,717,063,680 bytes
(1.60 GiB). These are warm local measurements, not a hardware guarantee.

The generated native fixture
`tests/fixtures/japanese_furigana_generated.png` ran successfully through the
PP-OCRv6 ONNX path on the same machine, producing two separated Japanese lines
and seven reading tokens. The fixture smoke command is:

```powershell
$env:LOCALAI_RUN_FURIGANA_SMOKE = '1'
$env:LOCALAI_FURIGANA_IMAGE = (Resolve-Path .\tests\fixtures\japanese_furigana_generated.png)
$env:LOCALAI_PPOCR_DET = (Resolve-Path .\models\ppocr\PP-OCRv6_medium_det.onnx)
$env:LOCALAI_PPOCR_REC = (Resolve-Path .\models\ppocr\PP-OCRv6_medium_rec.onnx)
$env:LOCALAI_PPOCR_DICT = (Resolve-Path .\models\ppocr\PP-OCRv6_medium_rec_inference.yml)
$env:LOCALAI_JAPANESE_DICT = (Resolve-Path .\models\japanese\readings.tsv)
& .\build\release\bin\Release\LocalAITests.exe
```

The smoke test runs OCR, UTF-8 output capture, both translation directions and
repeated translation operations, then prints the measured peak. It returns a
CTest skip code when model paths are unavailable. The repository does not
invent a VRAM result: the actual peak must be recorded from the target GPU.

## Known limitations

* The official Ovis model has no official GGUF release at the pinned revision;
  the exact community conversion and projector relationship is recorded above.
* There is no product-specific Ovis entry in upstream libmtmd. The adapter uses
  the generic metadata-compatible Qwen3.5/Qwen3-VL path and fails explicitly if
  the pair cannot be loaded.
* WIC format availability is OS/codec dependent. Animated images use their
  first frame. Large images are decoded into an RGB buffer before inference.
* Only one operation is active at a time. Sequential mode unloads the inactive
  model, which increases latency but is the default for a 6 GiB budget.
* Cancellation is cooperative. It is checked during model loading, image
  encoding and generation; a backend call can take time to reach its abort
  callback.
* Memory totals are reported as unavailable when the selected backend cannot
  report them. A successful load in that situation is not evidence of a 6 GiB
  peak.
* The OCR output currently follows the model's Markdown/HTML transcription
  instructions; a UI Markdown-rendering toggle is not included in this small
  prototype.
* PP-OCRv6 is a CPU ONNX Runtime backend in this prototype; it is separate from
  the Vulkan llama.cpp device and is not included in the 6 GiB Vulkan allocator
  measurement. The model files are small, but system RAM and ONNX peak memory
  still need measurement for a product build.
* The generated fixture validates Japanese text, CTC alignment, UTF-8 and
  overlay data. Release acceptance still needs independent English, accented
  French, mixed-language and table images on the target machine.

## Commercial redistribution checklist

Before shipping a product:

1. Include `LocalAILib.dll`, `LocalAIApp.exe` (if using the sample GUI), the
   public API documentation, `THIRD_PARTY_NOTICES.md`, MIT/Vulkan notices, and
   both model Apache-2.0 attributions/license text.
2. Re-run the license audit against the exact llama.cpp commit, Vulkan SDK and
   driver loader package shipped by the product. Do not ship build-only tools or
   multi-gigabyte models inside the executable bundle unless the product has a
   reviewed asset distribution plan.
3. Record model repository revisions, conversion metadata, hashes and the
   exact Ovis model/projector pair in the release manifest.
4. Test the final binaries with network access disabled, without Python, without
   administrator privileges, and with the supported Vulkan driver installed.
5. Record English, accented French, mixed-language, table, English↔French,
   cancellation, repeated-operation and peak-VRAM results on the release GPU.
6. Have counsel review model/commercial terms and all notices before release.
