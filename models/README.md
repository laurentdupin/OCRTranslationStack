# External model assets

Model files are optional external assets. The application never downloads them
and does not need network access at runtime.

Run the setup script from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_models.ps1
```

The default layout is:

```text
models/
  ocr/
    ATH-MaaS_OvisOCR2-Q6_K.gguf       # or Q6_K_L
    mmproj-ATH-MaaS_OvisOCR2-bf16.gguf
  translation/
    Hy-MT2-1.8B-Q6_K.gguf             # or Q4_K_M
  ppocr/
    PP-OCRv6_medium_det.onnx
    PP-OCRv6_medium_det_inference.yml
    PP-OCRv6_medium_rec.onnx
    PP-OCRv6_medium_rec_inference.yml
  japanese/
    unidic/                             # compiled MeCab/UniDic directory
```

The official `ATH-MaaS/OvisOCR2` release is safetensors-only. The setup script
therefore obtains the pinned GGUF conversion from
`bartowski/ATH-MaaS_OvisOCR2-GGUF` and pairs it with that repository's BF16
projector. The exact revisions, sizes and SHA-256 values are recorded in the
script and in the root README. Do not mix a projector from another release.

The optional Q4_K_M configurations reduce memory use at a quality/performance
tradeoff. The application chooses Q6 first, then Q6_K_L (OCR) or Q4_K_M
(translation) when the preferred file is absent.

PP-OCRv6 is the native, no-Python furigana path. The detector and recognizer
are the official ONNX releases pinned by `scripts/setup_models.ps1`; keep each
ONNX file with its matching inference YAML. The recognizer output is CTC
aligned, so the DLL can return a token surface, reading and image quadrilateral
without exposing ONNX Runtime or MeCab types.

Run the following explicitly when Japanese readings are wanted:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_models.ps1 `
  -IncludeJapaneseDictionary
```

That downloads the pinned UniDic for Contemporary Written Japanese archive
(`unidic-cwj-202512`) and records its computed SHA-256 beside the extracted
dictionary. `readings.tsv` is a small checked-in fixture fallback only; it is
not a production Japanese lexicon.
