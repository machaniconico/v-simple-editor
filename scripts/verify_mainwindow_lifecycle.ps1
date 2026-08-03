# Repeated Windows selftest harness for the non-deterministic MainWindow teardown.
# ExitCode is read from the Windows Process object as a 32-bit value so an
# access violation remains distinguishable from a WSL-shell status truncation.
[CmdletBinding()]
param(
    # precompose-e2e also builds a MainWindow, so it hits the same teardown path.
    [ValidateSet('mainwindow-lifecycle', 'light3d', 'precompose-e2e')]
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
        # A crash exit code such as 0xC0000005 arrives as a negative Int32.
        # Casting it straight to UInt32 throws, which used to abort the whole
        # loop on the first crash; mask through Int64 instead.
        $unsigned = [uint32]([int64]$exitCode -band 0xFFFFFFFF)
        Write-Host ("[{0}] FAIL {1}/{2} EXIT={3} (0x{4:X8})" -f `
            $TestName, $run, $Runs, $exitCode, $unsigned)
    }
}

$passed = $Runs - $failed
Write-Host ("[{0}] SUMMARY PASS={1} FAIL={2} RUNS={3}" -f $TestName, $passed, $failed, $Runs)
if ($failed -ne 0) {
    exit 1
}
exit 0
