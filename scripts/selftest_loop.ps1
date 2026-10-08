# Repeated selftest loop for intermittent hangs (US-912).
# Each run gets its own .out/.err, one line in results.txt, and, on TIMEOUT,
# a copy of the watchdog minidump and thread-stack text from
# .omc/artifacts/hang-dumps into the output directory.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z][a-z0-9-]*$')]
    [string]$TestName,

    [ValidateRange(1, 10000)]
    [int]$Runs = 100,

    # Executable to loop; pass a copy outside build/ so the loop does not hold
    # the shared build directory while other lanes rebuild it.
    [string]$ExePath = '',

    [Parameter(Mandatory = $true)]
    [string]$OutDir,

    # Per-entry watchdog (VEDITOR_SELFTEST_TIMEOUT_SEC); 0 keeps the exe default.
    [ValidateRange(0, 86400)]
    [int]$TimeoutSec = 0,

    [ValidateRange(0, 65535)]
    [int]$McpPort = 0,

    # Prefix for per-run log names and results.txt lines.
    [string]$Label = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($ExePath -eq '') {
    $ExePath = Join-Path $repoRoot 'build\Release\v-simple-editor.exe'
}
$ExePath = [System.IO.Path]::GetFullPath($ExePath)
if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    throw "Executable not found: $ExePath"
}
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $repoRoot $OutDir
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if ($Label -eq '') { $Label = $TestName }
$results = Join-Path $OutDir 'results.txt'
$dumpDir = Join-Path $repoRoot '.omc\artifacts\hang-dumps'

if ($TimeoutSec -gt 0) { $env:VEDITOR_SELFTEST_TIMEOUT_SEC = "$TimeoutSec" }
if ($McpPort -gt 0) { $env:VEDITOR_MCP_PORT = "$McpPort" }
# The watchdog ends a hung entry by itself; this outer bound only catches a
# process that hangs after the watchdog fired (for example while dumping).
$outerSec = $(if ($TimeoutSec -gt 0) { $TimeoutSec } else { 900 }) + 300

Add-Content -LiteralPath $results -Value ("# {0} start {1} exe={2} ({3}) runs={4} timeoutSec={5} mcpPort={6}" -f `
    $Label, (Get-Date -Format 's'), $ExePath, (Get-Item -LiteralPath $ExePath).LastWriteTime.ToString('s'),
    $Runs, $TimeoutSec, $McpPort)

$counts = @{ PASS = 0; FAIL = 0; TIMEOUT = 0; KILLED = 0 }
for ($run = 1; $run -le $Runs; $run++) {
    $out = Join-Path $OutDir ("{0}-{1}.out" -f $Label, $run)
    $err = Join-Path $OutDir ("{0}-{1}.err" -f $Label, $run)
    $started = Get-Date
    $process = Start-Process -FilePath $ExePath `
        -ArgumentList "--selftest=$TestName" `
        -WorkingDirectory $repoRoot `
        -NoNewWindow -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError $err
    $handle = $process.Handle  # keep the handle so ExitCode stays readable
    $status = ''
    if (-not $process.WaitForExit($outerSec * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit()
        $status = 'KILLED'
    }
    [int]$exitCode = $process.ExitCode
    $seconds = [int]((Get-Date) - $started).TotalSeconds
    $text = Get-Content -LiteralPath $err -Raw -ErrorAction SilentlyContinue
    if ($null -eq $text) { $text = '' }
    if ($status -eq '') {
        if ($text -match 'summary: TIMEOUT') { $status = 'TIMEOUT' }
        elseif ($exitCode -eq 0) { $status = 'PASS' }
        else { $status = 'FAIL' }
    }
    $counts[$status]++
    $dump = '-'
    $match = [regex]::Match($text, 'dump written to (\S+\.dmp)')
    if ($match.Success) {
        $dump = $match.Groups[1].Value
        $dumpPath = Join-Path $repoRoot ($dump -replace '/', '\')
        foreach ($file in @($dumpPath, [System.IO.Path]::ChangeExtension($dumpPath, '.txt'))) {
            if (Test-Path -LiteralPath $file -PathType Leaf) {
                Copy-Item -LiteralPath $file -Destination $OutDir -Force
            }
        }
    }
    $last = ([regex]::Matches($text, '(?m)^.*\b(PASS|FAIL) G\d+.*$') | Select-Object -Last 1)
    $lastGate = $(if ($null -ne $last) { $last.Value.Trim() } else { '-' })
    $line = "{0} {1} {2} rc={3} {4}s dump={5} last={6}" -f $Label, $run, $status, $exitCode, $seconds, $dump, $lastGate
    Add-Content -LiteralPath $results -Value $line
    Write-Host $line
    $null = $handle
}

$summary = "# {0} done {1} PASS={2} FAIL={3} TIMEOUT={4} KILLED={5} RUNS={6}" -f `
    $Label, (Get-Date -Format 's'), $counts.PASS, $counts.FAIL, $counts.TIMEOUT, $counts.KILLED, $Runs
Add-Content -LiteralPath $results -Value $summary
Write-Host $summary
if ($counts.PASS -ne $Runs) { exit 1 }
exit 0
