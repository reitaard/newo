param(
  [Parameter(Mandatory = $true)][string]$EspSrPath,
  [string]$ArduinoPackages = "$env:LOCALAPPDATA\Arduino15\packages\esp32",
  [string]$ArduinoVersion = "3.3.10",
  [string]$OutputDirectory = "$PSScriptRoot\build\esp32.esp32.esp32s3",
  [string]$ExpectedEspSrVersion = "2.4.6"
)

$ErrorActionPreference = "Stop"
$sdkConfig = Join-Path $ArduinoPackages "tools\esp32s3-libs\$ArduinoVersion\sdkconfig"
$moveModel = Join-Path $EspSrPath "model\movemodel.py"
$componentManifest = Join-Path $EspSrPath "idf_component.yml"
if (-not (Test-Path -LiteralPath $sdkConfig)) { throw "Arduino ESP32-S3 sdkconfig not found: $sdkConfig" }
if (-not (Test-Path -LiteralPath $moveModel)) { throw "ESP-SR movemodel.py not found: $moveModel" }
if (-not (Test-Path -LiteralPath $componentManifest)) { throw "ESP-SR component manifest not found: $componentManifest" }
$manifest = Get-Content -LiteralPath $componentManifest -Raw
$versionMatch = [regex]::Match($manifest, '(?m)^version:\s*([^\s#]+)')
$actualVersion = $versionMatch.Groups[1].Value.Trim('"', "'")
if ($actualVersion -ne $ExpectedEspSrVersion) {
  throw "ESP-SR must be component version $ExpectedEspSrVersion to match Arduino-ESP32 $ArduinoVersion"
}
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("newo-srmodels-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
try {
  $customConfig = Join-Path $work "sdkconfig"
  $content = Get-Content -LiteralPath $sdkConfig -Raw
  $content = $content -replace '(?m)^CONFIG_SR_WN_WN9_HIESP=y$', '# CONFIG_SR_WN_WN9_HIESP is not set'
  $content = $content -replace '(?m)^# CONFIG_SR_WN_WN9_HIWALLE_TTS2 is not set$', 'CONFIG_SR_WN_WN9_HIWALLE_TTS2=y'
  if ($content -notmatch '(?m)^CONFIG_SR_WN_WN9_HIWALLE_TTS2=y$') { throw "Arduino sdkconfig does not expose wn9_hiwalle_tts2" }
  Set-Content -LiteralPath $customConfig -Value $content -NoNewline
  python $moveModel -d1 $customConfig -d2 $EspSrPath -d3 $work
  if ($LASTEXITCODE -ne 0) { throw "movemodel.py failed with exit code $LASTEXITCODE" }
  $generated = Join-Path $work "srmodels\srmodels.bin"
  if (-not (Test-Path -LiteralPath $generated)) { throw "Generated srmodels.bin was not found" }
  New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
  Copy-Item -LiteralPath $generated -Destination (Join-Path $OutputDirectory "srmodels.bin") -Force
  Write-Host "Prepared wn9_hiwalle_tts2 model partition at $OutputDirectory\srmodels.bin"
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
