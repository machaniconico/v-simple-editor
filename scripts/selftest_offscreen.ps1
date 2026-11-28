# Run every selftest suite alone under QT_QPA_PLATFORM=offscreen and print a table
# (suite, exit code, PASS / FAIL / SKIP line counts, seconds) (Wave 21, S0-14).
# --selftest=all is not used: with offscreen it aborts inside clip-lut (a GL texture
# without a context), so the suites are started one process each. The table is a
# reference for what offscreen can and cannot run; it is not an acceptance result
# (acceptance runs on the real windows platform, common rule 109).
[CmdletBinding()]
param(
    [string]$ExePath = 'build\Release\v-simple-editor.exe',

    # Directory for one log per suite and for results.tsv.
    [string]$OutDir = '.omc\state\offscreen',

    # Comma-separated suite names; empty runs every name of --selftest=list
    # except the entries that are not plain suites (all, fixtures, probes, ...).
    [string]$Filter = '',

    [ValidateRange(10, 86400)]
    [int]$TimeoutSec = 300,

    [ValidateRange(0, 65535)]
    [int]$McpPort = 0
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not [System.IO.Path]::IsPathRooted($ExePath)) { $ExePath = Join-Path $repoRoot $ExePath }
if (-not [System.IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $repoRoot $OutDir }
if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) { throw "Executable not found: $ExePath" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$env:QT_QPA_PLATFORM = 'offscreen'
if ($McpPort -gt 0) { $env:VEDITOR_MCP_PORT = "$McpPort" }

$skipNames = @('all', 'audio-fixture', 'credential-store', 'gate-count-probe',
               'gate-tally', 'render-fixture', 'watchdog', 'watchdog-probe')
if ($Filter -ne '') {
    $suites = @($Filter -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
} else {
    $listOut = Join-Path $OutDir 'list.txt'
    $p = Start-Process -FilePath $ExePath -ArgumentList '--selftest=list' -WorkingDirectory $repoRoot `
        -NoNewWindow -PassThru -RedirectStandardOutput $listOut -RedirectStandardError (Join-Path $OutDir 'list.err')
    $handle = $p.Handle
    if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force; throw 'selftest=list did not finish' }
    $suites = @(Get-Content -LiteralPath $listOut |
        ForEach-Object { if ($_ -match '^--selftest=(\S+)') { $Matches[1] } } |
        Where-Object { $skipNames -notcontains $_ })
    if ($p.ExitCode -ne 0 -or $suites.Count -eq 0) {
        throw "selftest=list failed (exit $($p.ExitCode), $($suites.Count) suites): see $listOut and $(Join-Path $OutDir 'list.err')"
    }
}

$tsv = Join-Path $OutDir 'results.tsv'
Set-Content -LiteralPath $tsv -Value "suite`trc`tPASS`tFAIL`tSKIP`tseconds"
$total = @{ rc0 = 0; rcNon0 = 0; timeout = 0 }
foreach ($suite in $suites) {
    $out = Join-Path $OutDir "$suite.log"
    $err = Join-Path $OutDir "$suite.err"
    $started = Get-Date
    $p = Start-Process -FilePath $ExePath -ArgumentList "--selftest=$suite" -WorkingDirectory $repoRoot `
        -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $handle = $p.Handle  # keep the handle so ExitCode stays readable
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $p.WaitForExit()
        $rc = 'TIMEOUT'
        $total.timeout++
    } else {
        $rc = [int]$p.ExitCode
        if ($rc -eq 0) { $total.rc0++ } else { $total.rcNon0++ }
    }
    $text = ''
    foreach ($f in @($out, $err)) {
        $t = Get-Content -LiteralPath $f -Raw -ErrorAction SilentlyContinue
        if ($null -ne $t) { $text += $t }
    }
    $pass = ([regex]::Matches($text, '(?m)^PASS\b')).Count
    $fail = ([regex]::Matches($text, '(?m)^FAIL\b')).Count
    $skip = ([regex]::Matches($text, '(?m)^SKIP\b')).Count
    $seconds = [int]((Get-Date) - $started).TotalSeconds
    $line = "{0}`t{1}`t{2}`t{3}`t{4}`t{5}" -f $suite, $rc, $pass, $fail, $skip, $seconds
    Add-Content -LiteralPath $tsv -Value $line
    Write-Host $line
}
Write-Host ("suites={0} rc0={1} rcNon0={2} timeout={3} table={4}" -f `
    $suites.Count, $total.rc0, $total.rcNon0, $total.timeout, $tsv)
