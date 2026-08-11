[CmdletBinding()]
param(
    [string] $WorkspaceRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('Q6_K', 'Q6_K_L', 'Q4_K_M')]
    [string] $OcrQuantization = 'Q6_K',
    [ValidateSet('Q6_K', 'Q4_K_M')]
    [string] $TranslationQuantization = 'Q6_K',
    [switch] $IncludeJapaneseDictionary
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# These are immutable Hugging Face revisions. The OvisOCR2 official release is
# safetensors-only; the GGUF files below are the pinned bartowski conversion.
$OvisOfficialRevision = '65c619d374b55d4152e85150fc1b003700bc1f0c'
$OvisGgufRevision = 'ab22420f3d44201d3aa5a62ca49a665a46b507e9'
$HyRevision = '1cd5208700acedef4ef93019b6cfc148b8522d45'
$PpOcrDetectionRevision = '61323801669c338b7891481ec7bac61ce31b576a'
$PpOcrRecognitionRevision = '50c7eacafc52fa7bcf4194e8cd08e46f8558504b'
$UniDicVersion = '202512'
$UniDicFile = "unidic-cwj-$UniDicVersion.zip"
$UniDicUrl = "https://clrd.ninjal.ac.jp/unidic_archive/2512/$UniDicFile"
$OvisGgufRepository = 'bartowski/ATH-MaaS_OvisOCR2-GGUF'
$HyRepository = 'tencent/Hy-MT2-1.8B-GGUF'

$Artifacts = @{
    'Ovis-Q6_K' = @{
        Repository = $OvisGgufRepository
        Revision = $OvisGgufRevision
        File = 'ATH-MaaS_OvisOCR2-Q6_K.gguf'
        Sha256 = '7f86d22d9e3e359a4156c1a2f01bbd35f07bcc33dc33ddd92aee2812db101705'
        Bytes = 669712512
    }
    'Ovis-Q6_K_L' = @{
        Repository = $OvisGgufRepository
        Revision = $OvisGgufRevision
        File = 'ATH-MaaS_OvisOCR2-Q6_K_L.gguf'
        Sha256 = 'd9c0d84216e60c41e643d4f6a3e9e2e33d681b84445ab0ad0df522f43ff55869'
        Bytes = 731295872
    }
    'Ovis-Q4_K_M' = @{
        Repository = $OvisGgufRepository
        Revision = $OvisGgufRevision
        File = 'ATH-MaaS_OvisOCR2-Q4_K_M.gguf'
        Sha256 = '3786d230ceb8f217abdfb8ea8adba975827595053ad5087cb5502898d6a8a68e'
        Bytes = 557867136
    }
    'Ovis-projector-bf16' = @{
        Repository = $OvisGgufRepository
        Revision = $OvisGgufRevision
        File = 'mmproj-ATH-MaaS_OvisOCR2-bf16.gguf'
        Sha256 = '842015fc8bb09d8b953a04b1ba329510e7259081422a9dab50f41ce4fac430a1'
        Bytes = 207346336
    }
    'Hy-Q6_K' = @{
        Repository = $HyRepository
        Revision = $HyRevision
        File = 'Hy-MT2-1.8B-Q6_K.gguf'
        Sha256 = 'd98fe604dec1f28f58f80d7d560f7177e584d3b8e5835862687660e5ff97cb40'
        Bytes = 1474785120
    }
    'Hy-Q4_K_M' = @{
        Repository = $HyRepository
        Revision = $HyRevision
        File = 'Hy-MT2-1.8B-Q4_K_M.gguf'
        Sha256 = 'dc5f44fcf1fa496ee7ad725982c0c8c553a4de00259b53af84c4b89fb0c06699'
        Bytes = 1133080448
    }
    'PP-OCRv6-det' = @{
        Repository = 'PaddlePaddle/PP-OCRv6_medium_det_onnx'
        Revision = $PpOcrDetectionRevision
        File = 'PP-OCRv6_medium_det.onnx'
        Sha256 = 'eb13b44b25bb36f89528b68720af8a61d9cf381176107f465db1757b65d086e1'
        Bytes = 62032837
    }
    'PP-OCRv6-det-config' = @{
        Repository = 'PaddlePaddle/PP-OCRv6_medium_det_onnx'
        Revision = $PpOcrDetectionRevision
        File = 'PP-OCRv6_medium_det_inference.yml'
        Sha256 = '7298d5ead546584af2504d03355f881ac7a7bc0eb1e282d3e159277c1d0af871'
        Bytes = 886
    }
    'PP-OCRv6-rec' = @{
        Repository = 'PaddlePaddle/PP-OCRv6_medium_rec_onnx'
        Revision = $PpOcrRecognitionRevision
        File = 'PP-OCRv6_medium_rec.onnx'
        Sha256 = '9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba'
        Bytes = 76554979
    }
    'PP-OCRv6-rec-config' = @{
        Repository = 'PaddlePaddle/PP-OCRv6_medium_rec_onnx'
        Revision = $PpOcrRecognitionRevision
        File = 'PP-OCRv6_medium_rec_inference.yml'
        Sha256 = '991b700fac5b50a7de193468207d5f4255b538dde0d312ae3b7c7a9b6873129'
        Bytes = 150580
    }
}

function Get-ArtifactUrl {
    param([hashtable] $Artifact)
    return "https://huggingface.co/$($Artifact.Repository)/resolve/$($Artifact.Revision)/$($Artifact.File)?download=true"
}

function Install-VerifiedArtifact {
    param(
        [Parameter(Mandatory = $true)][hashtable] $Artifact,
        [Parameter(Mandatory = $true)][string] $DestinationDirectory
    )
    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
    $destination = Join-Path $DestinationDirectory $Artifact.File
    if (Test-Path $destination) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash.ToLowerInvariant()
        if ($hash -eq $Artifact.Sha256) {
            if ((Get-Item -LiteralPath $destination).Length -ne $Artifact.Bytes) {
                throw "Verified hash but unexpected size for $destination"
            }
            Write-Host "Already verified: $destination"
            return
        }
        throw "Refusing to overwrite an existing file with the wrong checksum: $destination"
    }
    $temporary = "$destination.download"
    if (Test-Path $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }
    Write-Host "Downloading $($Artifact.File) from revision $($Artifact.Revision)"
    Invoke-WebRequest -Uri (Get-ArtifactUrl $Artifact) -OutFile $temporary -UseBasicParsing
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $temporary).Length
    if ($hash -ne $Artifact.Sha256 -or $size -ne $Artifact.Bytes) {
        Remove-Item -LiteralPath $temporary -Force
        throw "Checksum or size mismatch for $($Artifact.File): got $hash / $size"
    }
    Move-Item -LiteralPath $temporary -Destination $destination
    Write-Host "Installed and verified: $destination"
}

function Install-TextAtRevision {
    param(
        [Parameter(Mandatory = $true)][string] $Repository,
        [Parameter(Mandatory = $true)][string] $Revision,
        [Parameter(Mandatory = $true)][string] $File,
        [Parameter(Mandatory = $true)][string] $Destination
    )
    if (Test-Path $Destination) {
        Write-Host "Already present: $Destination"
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Invoke-WebRequest -Uri "https://huggingface.co/$Repository/resolve/$Revision/$File" -OutFile $Destination -UseBasicParsing
}

$modelsRoot = Join-Path $WorkspaceRoot 'models'
$ocrRoot = Join-Path $modelsRoot 'ocr'
$translationRoot = Join-Path $modelsRoot 'translation'
$ppocrRoot = Join-Path $modelsRoot 'ppocr'
$ocrArtifact = $Artifacts["Ovis-$OcrQuantization"]
$translationArtifact = $Artifacts["Hy-$TranslationQuantization"]

Install-VerifiedArtifact $ocrArtifact $ocrRoot
Install-VerifiedArtifact $Artifacts['Ovis-projector-bf16'] $ocrRoot
Install-VerifiedArtifact $translationArtifact $translationRoot
Install-VerifiedArtifact $Artifacts['PP-OCRv6-det'] $ppocrRoot
Install-VerifiedArtifact $Artifacts['PP-OCRv6-det-config'] $ppocrRoot
Install-VerifiedArtifact $Artifacts['PP-OCRv6-rec'] $ppocrRoot
Install-VerifiedArtifact $Artifacts['PP-OCRv6-rec-config'] $ppocrRoot

if ($IncludeJapaneseDictionary) {
    $japaneseRoot = Join-Path $modelsRoot 'japanese'
    $downloadRoot = Join-Path $WorkspaceRoot 'build\model-downloads'
    $archive = Join-Path $downloadRoot $UniDicFile
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host "Downloading pinned UniDic $UniDicVersion from $UniDicUrl"
        Invoke-WebRequest -Uri $UniDicUrl -OutFile $archive -UseBasicParsing
    }
    $archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    New-Item -ItemType Directory -Force -Path $japaneseRoot | Out-Null
    Set-Content -LiteralPath (Join-Path $japaneseRoot "$UniDicFile.sha256") -Value $archiveHash -Encoding ascii
    $extract = Join-Path $downloadRoot "unidic-cwj-$UniDicVersion"
    if (-not (Test-Path -LiteralPath (Join-Path $japaneseRoot 'unidic\dicrc'))) {
        if (Test-Path -LiteralPath $extract) {
            Remove-Item -LiteralPath $extract -Recurse -Force
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $extract -Force
        $dicrc = Get-ChildItem -LiteralPath $extract -Recurse -Filter 'dicrc' -File | Select-Object -First 1
        if ($dicrc -eq $null) {
            throw "UniDic archive did not contain a compiled MeCab dicrc file"
        }
        $dictionarySource = $dicrc.Directory.FullName
        $dictionaryDestination = Join-Path $japaneseRoot 'unidic'
        New-Item -ItemType Directory -Force -Path $dictionaryDestination | Out-Null
        Copy-Item -Path (Join-Path $dictionarySource '*') -Destination $dictionaryDestination -Recurse -Force
    }
    Write-Host "Installed UniDic $UniDicVersion under $japaneseRoot\unidic"
} else {
    Write-Host "UniDic was not downloaded. Re-run with -IncludeJapaneseDictionary for furigana readings."
}

# Preserve the upstream model notices alongside the external assets. The same
# notices are also committed under licenses/ for redistribution review.
Install-TextAtRevision 'ATH-MaaS/OvisOCR2' $OvisOfficialRevision 'LICENSE' (Join-Path $ocrRoot 'LICENSE-OvisOCR2.txt')
Install-TextAtRevision $OvisGgufRepository $OvisGgufRevision 'README.md' (Join-Path $ocrRoot 'README-OvisOCR2-GGUF.txt')
Install-TextAtRevision $HyRepository $HyRevision 'LICENSE.txt' (Join-Path $translationRoot 'LICENSE-Hy-MT2.txt')

Write-Host ""
Write-Host "Model setup complete. No model is downloaded by the application itself."
Write-Host "OvisOCR2 source: ATH-MaaS/OvisOCR2@$OvisOfficialRevision"
Write-Host "OvisOCR2 GGUF conversion: $OvisGgufRepository@$OvisGgufRevision"
Write-Host "Hy-MT2: $HyRepository@$HyRevision"
Write-Host "PP-OCRv6 detector: PaddlePaddle/PP-OCRv6_medium_det_onnx@$PpOcrDetectionRevision"
Write-Host "PP-OCRv6 recognizer: PaddlePaddle/PP-OCRv6_medium_rec_onnx@$PpOcrRecognitionRevision"
