#requires -Version 5.1
<#
v-simple-editor: GPU 検出スクリプト
出力 (stdout 1行):
  modern   : AV1 ハードウェアデコード対応 GPU を検出
  classic  : 非対応 / 不明
判定基準 (2026 年時点):
  - NVIDIA RTX 30 / 40 / 50 シリーズ (Ampere+)
  - Intel Arc A/B シリーズ + Iris Xe (Tiger Lake / 11th gen 以降)
  - AMD Radeon RX 6000 / 7000 / 8000 / 9000 シリーズ
#>

$ErrorActionPreference = 'Stop'

function Test-Av1Capable {
    param([string]$Name)
    if ([string]::IsNullOrWhiteSpace($Name)) { return $false }

    # NVIDIA: RTX 30xx / 40xx / 50xx
    if ($Name -match 'RTX\s*(30|40|50)\d{2}') { return $true }

    # Intel Arc (discrete) + Iris Xe (integrated 11th gen+)
    if ($Name -match 'Arc\s*[AB]\d{3}') { return $true }
    if ($Name -match 'Iris\s*Xe') { return $true }

    # AMD Radeon RX 6/7/8/9 thousand
    if ($Name -match 'RX\s*(6|7|8|9)\d{3}') { return $true }

    return $false
}

try {
    $gpus = Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop |
        Where-Object { $_.Name -and $_.Name -notmatch 'Microsoft Basic|Remote Display' }

    foreach ($gpu in $gpus) {
        if (Test-Av1Capable -Name $gpu.Name) {
            Write-Output 'modern'
            exit 0
        }
    }
    Write-Output 'classic'
    exit 0
} catch {
    # 判定失敗時は安全側 (classic) に倒す
    Write-Output 'classic'
    exit 0
}
