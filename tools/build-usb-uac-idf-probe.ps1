[CmdletBinding()]
param(
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"

$idf = Get-Command idf.py -ErrorAction SilentlyContinue
if (-not $idf) {
    throw "idf.py was not found. Open an ESP-IDF 5.5.4 PowerShell/terminal first, then run this script again."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "experiments\usb-uac-idf-probe"

Push-Location $project
try {
    Write-Host "[probe] ESP-IDF: $(& idf.py --version)"
    Write-Host "[probe] project: $project"

    if (-not (Test-Path (Join-Path $project "sdkconfig"))) {
        & idf.py set-target esp32s3
        if ($LASTEXITCODE -ne 0) { throw "idf.py set-target failed" }
    }

    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed" }

    if ($Port) {
        Write-Host "[probe] flashing $Port; do not run erase-flash"
        & idf.py -p $Port flash monitor
        if ($LASTEXITCODE -ne 0) { throw "idf.py flash/monitor failed" }
    } else {
        Write-Host "[probe] build complete. Re-run with -Port COMx to flash and monitor."
    }
}
finally {
    Pop-Location
}
