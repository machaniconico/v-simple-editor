# Repeated Windows selftest harness for the non-deterministic MainWindow teardown.
# ExitCode is read from the Windows Process object as a 32-bit value so an
# access violation remains distinguishable from a WSL-shell status truncation.
[CmdletBinding()]
param(
    [ValidateSet('mainwindow-lifecycle', 'light3d')]
    [string]$TestName = 'mainwindow-lifecycle',

    [ValidateRange(1, 1000)]
    [int]$Runs = 20,

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$exePath = Join-Path $repoRoot (Join-Path 'build' (Join-Path $Configuration 'v-simple-editor.exe'))

if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Executable not found: $exePath"
}

$failed = 0
for ($run = 1; $run -le $Runs; $run++) {
    $process = Start-Process -FilePath $exePath `
        -ArgumentList "--selftest=$TestName" `
        -WorkingDirectory $repoRoot `
        -NoNewWindow `
        -Wait `
        -PassThru
    [int]$exitCode = $process.ExitCode
    if ($exitCode -eq 0) {
        Write-Host ("[{0}] PASS {1}/{2} EXIT=0" -f $TestName, $run, $Runs)
    } else {
        $failed++
        Write-Host ("[{0}] FAIL {1}/{2} EXIT={3} (0x{4:X8})" -f `
            $TestName, $run, $Runs, $exitCode, ([uint32]$exitCode))
    }
}

$passed = $Runs - $failed
Write-Host ("[{0}] SUMMARY PASS={1} FAIL={2} RUNS={3}" -f $TestName, $passed, $failed, $Runs)
if ($failed -ne 0) {
    exit 1
}
exit 0
