[CmdletBinding()]
param(
    [string] $WorkspaceRoot = (Split-Path -Parent $PSScriptRoot),
    [switch] $Update
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Pinned after the OvisOCR2/libmtmd and Hy-MT2 compatibility audit on 2026-08-01.
$LlamaRevision = '876a4321163249c43ca4e986818fab5ab081f282'
$LlamaRepository = 'https://github.com/ggml-org/llama.cpp.git'
$Destination = Join-Path $WorkspaceRoot 'third_party\llama.cpp'
$OnnxRuntimeVersion = '1.24.4'
$OnnxRuntimeUrl = "https://github.com/microsoft/onnxruntime/releases/download/v$OnnxRuntimeVersion/onnxruntime-win-x64-$OnnxRuntimeVersion.zip"
$OnnxRuntimeSha256 = 'd2319fddfb6ea4db99ccc4b60c85c517bcd855721f5daa6a06d40d7cb2ee2357'
$OnnxRuntimeRoot = Join-Path $WorkspaceRoot 'third_party\onnxruntime'
$MecabVersion = '0.996'
$MecabUrl = 'https://deb.debian.org/debian/pool/main/m/mecab/mecab_0.996.orig.tar.gz'
$MecabSha256 = 'e073325783135b72e666145c781bb48fada583d5224fb2490fb6c1403ba69c59'
$MecabRoot = Join-Path $WorkspaceRoot 'third_party\mecab'
$DependencyDownloadRoot = Join-Path $WorkspaceRoot 'build\dependency-downloads'

function Invoke-Git {
    param([Parameter(Mandatory = $true)][string[]] $Arguments)
    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][string] $Uri,
        [Parameter(Mandatory = $true)][string] $DestinationPath,
        [Parameter(Mandatory = $true)][string] $Sha256
    )
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $DestinationPath) | Out-Null
    if (Test-Path -LiteralPath $DestinationPath) {
        $existing = (Get-FileHash -Algorithm SHA256 -LiteralPath $DestinationPath).Hash.ToLowerInvariant()
        if ($existing -ne $Sha256) {
            throw "Checksum mismatch for existing dependency archive ${DestinationPath}: $existing"
        }
        return
    }
    $temporary = "$DestinationPath.download"
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }
    Write-Host "Downloading $Uri"
    Invoke-WebRequest -Uri $Uri -OutFile $temporary -UseBasicParsing
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) {
        Remove-Item -LiteralPath $temporary -Force
        throw "Checksum mismatch for ${Uri}: $actual"
    }
    Move-Item -LiteralPath $temporary -Destination $DestinationPath
}

function Prepare-OnnxRuntime {
    $archive = Join-Path $DependencyDownloadRoot "onnxruntime-win-x64-$OnnxRuntimeVersion.zip"
    Get-VerifiedDownload $OnnxRuntimeUrl $archive $OnnxRuntimeSha256
    $required = @(
        (Join-Path $OnnxRuntimeRoot 'include\onnxruntime_cxx_api.h'),
        (Join-Path $OnnxRuntimeRoot 'lib\onnxruntime.lib'),
        (Join-Path $OnnxRuntimeRoot 'lib\onnxruntime.dll'))
    $complete = $true
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path)) {
            $complete = $false
            break
        }
    }
    if (-not $complete) {
        $extract = Join-Path $DependencyDownloadRoot "onnxruntime-$OnnxRuntimeVersion"
        if (Test-Path -LiteralPath $extract) {
            Remove-Item -LiteralPath $extract -Recurse -Force
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $extract -Force
        $package = Get-ChildItem -LiteralPath $extract -Directory | Select-Object -First 1
        if ($package -eq $null) {
            throw "ONNX Runtime archive did not contain a package directory"
        }
        New-Item -ItemType Directory -Force -Path $OnnxRuntimeRoot | Out-Null
        Copy-Item -LiteralPath (Join-Path $package.FullName 'include') -Destination $OnnxRuntimeRoot -Recurse -Force
        Copy-Item -LiteralPath (Join-Path $package.FullName 'lib') -Destination $OnnxRuntimeRoot -Recurse -Force
    }
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "ONNX Runtime package is incomplete: $path"
        }
    }
    Write-Host "Prepared ONNX Runtime $OnnxRuntimeVersion"
}

function Prepare-Mecab {
    $archive = Join-Path $DependencyDownloadRoot "mecab_$MecabVersion.orig.tar.gz"
    Get-VerifiedDownload $MecabUrl $archive $MecabSha256
    $header = Join-Path $MecabRoot 'src\mecab.h'
    if (-not (Test-Path -LiteralPath $header)) {
        $extract = Join-Path $DependencyDownloadRoot "mecab-$MecabVersion"
        if (Test-Path -LiteralPath $extract) {
            Remove-Item -LiteralPath $extract -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $extract | Out-Null
        tar -xzf $archive -C $extract
        if ($LASTEXITCODE -ne 0) {
            throw "tar could not extract MeCab $MecabVersion"
        }
        $package = Get-ChildItem -LiteralPath $extract -Directory | Select-Object -First 1
        if ($package -eq $null) {
            throw "MeCab archive did not contain a package directory"
        }
        New-Item -ItemType Directory -Force -Path $MecabRoot | Out-Null
        Copy-Item -Path (Join-Path $package.FullName '*') -Destination $MecabRoot -Recurse -Force
    }
    $dictionaryCpp = Join-Path $MecabRoot 'src\dictionary.cpp'
    if (-not (Test-Path -LiteralPath $dictionaryCpp)) {
        throw "MeCab source is incomplete: $dictionaryCpp"
    }
    $contents = [System.IO.File]::ReadAllText($dictionaryCpp)
    $old = 'struct pair_1st_cmp : public std::binary_function<T1, T2, bool> {'
    $new = 'struct pair_1st_cmp {'
    if ($contents.Contains($old)) {
        $contents = $contents.Replace($old, $new)
        [System.IO.File]::WriteAllText(
            $dictionaryCpp,
            $contents,
            [System.Text.UTF8Encoding]::new($false))
    }
    Write-Host "Prepared MeCab $MecabVersion with the reviewed MSVC compatibility patch"
}

if (Test-Path (Join-Path $Destination '.git')) {
    $top = (& git -C $Destination rev-parse --show-toplevel).Trim()
    if ([System.IO.Path]::GetFullPath($top) -ne [System.IO.Path]::GetFullPath($Destination)) {
        throw "Existing dependency path is not the expected repository: $Destination"
    }
    $dirty = (& git -C $Destination status --porcelain).Trim()
    if ($dirty) {
        throw "The pinned llama.cpp checkout has local changes. Review them before using -Update."
    }
    $current = (& git -C $Destination rev-parse HEAD).Trim()
    if ($current -ne $LlamaRevision) {
        if (-not $Update) {
            throw "llama.cpp is at $current, expected $LlamaRevision. Re-run with -Update to fetch the pinned revision."
        }
        Invoke-Git @('-C', $Destination, 'fetch', '--depth', '1', 'origin', $LlamaRevision)
        Invoke-Git @('-C', $Destination, 'checkout', '--detach', $LlamaRevision)
    }
} elseif (Test-Path $Destination) {
    throw "Dependency destination exists but is not a git checkout: $Destination"
} else {
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Invoke-Git @('clone', '--filter=blob:none', '--no-checkout', $LlamaRepository, $Destination)
    Invoke-Git @('-C', $Destination, 'checkout', '--detach', $LlamaRevision)
}

$verified = (& git -C $Destination rev-parse HEAD).Trim()
if ($verified -ne $LlamaRevision) {
    throw "Pinned dependency verification failed: got $verified"
}

Prepare-OnnxRuntime
Prepare-Mecab

Write-Host "Prepared llama.cpp at $verified"
Write-Host "No model files were downloaded. Run setup_models.ps1 explicitly when model assets are wanted."
