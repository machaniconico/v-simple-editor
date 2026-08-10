[CmdletBinding()]
param(
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
    $RepoRoot = Split-Path -Parent $scriptDirectory
}

function Assert-FfmpegFeature {
    param(
        [string]$RelativePath,
        [string]$Feature,
        [int]$MinimumMatches,
        [int]$MaximumMatches = [int]::MaxValue
    )

    $path = Join-Path $RepoRoot $RelativePath
    $content = Get-Content -Raw -LiteralPath $path
    $escapedFeature = [regex]::Escape($Feature)
    $pattern = "(?im)ffmpeg\[[^\]\r\n]*\b$escapedFeature\b[^\]\r\n]*\]"
    $matches = [regex]::Matches($content, $pattern)
    if ($matches.Count -lt $MinimumMatches -or $matches.Count -gt $MaximumMatches) {
        throw "$RelativePath must request the FFmpeg $Feature feature between $MinimumMatches and $MaximumMatches time(s); found $($matches.Count)"
    }
    Write-Output "PASS $RelativePath $Feature feature matches=$($matches.Count)"
}

function Assert-TextPattern {
    param(
        [string]$RelativePath,
        [string]$Pattern,
        [string]$Description
    )

    $path = Join-Path $RepoRoot $RelativePath
    $content = Get-Content -Raw -LiteralPath $path
    if ($content -notmatch $Pattern) {
        throw "$RelativePath must declare $Description"
    }
    Write-Output "PASS $RelativePath $Description"
}

Assert-FfmpegFeature -RelativePath 'build.bat' -Feature 'amf' -MinimumMatches 2
Assert-FfmpegFeature -RelativePath 'setup.bat' -Feature 'amf' -MinimumMatches 1
Assert-FfmpegFeature -RelativePath '.github/workflows/selftest.yml' -Feature 'amf' -MinimumMatches 1
# Classic intentionally remains H.264/HEVC-only; Modern is the sole build.bat
# edition that requests the AV1 software fallback.
Assert-FfmpegFeature -RelativePath 'build.bat' -Feature 'aom' -MinimumMatches 1 -MaximumMatches 1
Assert-FfmpegFeature -RelativePath 'setup.bat' -Feature 'aom' -MinimumMatches 1
Assert-FfmpegFeature -RelativePath '.github/workflows/selftest.yml' -Feature 'aom' -MinimumMatches 1
Assert-TextPattern -RelativePath 'setup.bat' -Pattern '(?i)-DVEDITOR_EDITION=modern' -Description 'Modern edition for the AOM dependency set'
Assert-TextPattern -RelativePath '.github/workflows/selftest.yml' -Pattern '(?i)-DVEDITOR_EDITION=modern' -Description 'Modern edition for the AOM dependency set'

Write-Output 'RADEON-BUILD-SUPPORT: PASS'
