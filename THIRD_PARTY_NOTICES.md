# Third-party notices

This file is the starting inventory for the native prototype. The exact
versions below are pinned by source revision or by the setup script. Do a final
license audit against the generated binary and the specific Vulkan SDK package
used for a commercial release.

## llama.cpp, ggml and libmtmd

The build incorporates the static `llama`, `ggml` and `mtmd` libraries from
llama.cpp revision
`876a4321163249c43ca4e986818fab5ab081f282` (official repository
`ggml-org/llama.cpp`). The upstream repository is MIT licensed. The notice is
included in `licenses/llama.cpp-MIT.txt` and must remain with source and binary
redistributions. libmtmd is built from the same revision and is covered by the
upstream MIT notice.

The upstream mtmd helper includes stb_image and miniaudio headers for optional
image/audio helper code. stb_image is public-domain/MIT dual licensed and
miniaudio is public-domain/MIT dual licensed. Their upstream notices are part
of the pinned llama.cpp source tree; retain those files when distributing the
source checkout. This application uses Windows Imaging Component for its own
image decode path and does not expose those headers through the DLL ABI.

## Vulkan

The Vulkan backend is compiled against Khronos Vulkan-Headers from the Vulkan
SDK selected by CMake. See `licenses/Vulkan-Headers-Apache-2.0.txt` and the
Apache-2.0 text. The application does not ship `vulkan-1.dll`; it relies on the
Vulkan loader installed with the user's Windows graphics driver. A product that
redistributes a loader must include the license files from that exact loader
build.

## Windows system APIs

Windows Imaging Component, Direct2D, COM, common controls and the Windows file
dialogs are operating-system components. No Qt, Electron, .NET runtime, Python,
CUDA, or network client is included by this project.

## ONNX Runtime

The optional native PP-OCRv6 backend uses the official ONNX Runtime Windows
x64 package `1.24.4`, downloaded and verified by
`scripts/setup_dependencies.ps1`. The package is MIT licensed; retain
`licenses/ONNX-Runtime-MIT.txt` and ship only the runtime DLLs needed by the
chosen build. ONNX Runtime headers and symbols remain private to
`LocalAILib.dll`.

## PP-OCRv6

The detector and recognizer are official Apache-2.0 ONNX artifacts from
`PaddlePaddle/PP-OCRv6_medium_det_onnx` revision
`61323801669c338b7891481ec7bac61ce31b576a` and
`PaddlePaddle/PP-OCRv6_medium_rec_onnx` revision
`50c7eacafc52fa7bcf4194e8cd08e46f8558504b`. Preserve the model cards and
Apache notice when those external assets are redistributed. The native code
implements the required DB thresholding, contour/convex-hull recovery, box
scoring, minimum-area quadrilateral fitting, unclip expansion, CTC greedy
decoding/alignment and image-coordinate mapping without copying Python,
OpenCV or other reciprocal code.

## MeCab and UniDic

The optional Japanese reading stage statically incorporates MeCab `0.996`
source from the Debian source archive, using its permissive BSD licensing
option. The only source change is the MSVC compatibility removal of the
obsolete `std::binary_function` base from `dictionary.cpp`. Keep
`licenses/MeCab-BSD-3-Clause.txt` with binary/source distributions.

UniDic is an external Japanese dictionary asset, not linked into the DLL. The
setup script pins the `unidic-cwj-202512` archive from the National Institute
for Japanese Language and Linguistics. UniDic is distributed under the GPL
v2.0/LGPL v2.1/New BSD triple license; this project uses the BSD option, but a
commercial product must preserve the exact license and attribution files from
the selected archive. See `licenses/UniDic-README.txt`.

## Models

The model assets are optional and are not embedded in the executable. Both
model releases are Apache-2.0:

* `ATH-MaaS/OvisOCR2` official revision
  `65c619d374b55d4152e85150fc1b003700bc1f0c`; attribution is in
  `licenses/OvisOCR2-LICENSE.txt`.
* `bartowski/ATH-MaaS_OvisOCR2-GGUF` conversion revision
  `ab22420f3d44201d3aa5a62ca49a665a46b507e9`; it supplies the pinned Q6/Q4
  GGUF files and matching BF16 projector. Its README is saved by the setup
  script as conversion metadata.
* `tencent/Hy-MT2-1.8B-GGUF` revision
  `1cd5208700acedef4ef93019b6cfc148b8522d45`; attribution is in
  `licenses/Hy-MT2-LICENSE.txt`.

Keep the exact upstream LICENSE files copied by `setup_models.ps1` when model
files are redistributed. No GPL or other reciprocal code is intentionally
incorporated.
