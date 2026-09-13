param(
  [string]$EspSrPath = "$PSScriptRoot\.esp-sr-model-pack\esp-sr-2.4.6\esp-sr",
  [string]$ArduinoPackages = "$env:LOCALAPPDATA\Arduino15\packages\esp32",
  [string]$ArduinoVersion = "3.3.10",
  [string]$OutputDirectory = "$PSScriptRoot\build\esp32.esp32.esp32s3",
  [string]$ExpectedEspSrVersion = "2.4.6"
)

$ErrorActionPreference = "Stop"
$expectedFqbnParts = @(
  "esp32:esp32:esp32s3",
  "PartitionScheme=esp_sr_16",
  "EraseFlash=none"
)
$hardwarePath = Join-Path $ArduinoPackages "hardware\esp32\$ArduinoVersion"
$sdkConfig = Join-Path $ArduinoPackages "tools\esp32s3-libs\$ArduinoVersion\sdkconfig"
$versionsFile = Join-Path $ArduinoPackages "tools\esp32s3-libs\$ArduinoVersion\versions.txt"
$boardsFile = Join-Path $hardwarePath "boards.txt"
$moveModel = Join-Path $EspSrPath "model\movemodel.py"
$componentManifest = Join-Path $EspSrPath "idf_component.yml"
$buildOptionsFile = Join-Path $OutputDirectory "build.options.json"
$partitionsFile = Join-Path $OutputDirectory "partitions.csv"
if (-not (Test-Path -LiteralPath $hardwarePath)) { throw "Arduino-ESP32 $ArduinoVersion not found: $hardwarePath" }
if (-not (Test-Path -LiteralPath $sdkConfig)) { throw "Arduino ESP32-S3 sdkconfig not found: $sdkConfig" }
if (-not (Test-Path -LiteralPath $versionsFile)) { throw "Arduino ESP32-S3 versions file not found: $versionsFile" }
if (-not (Test-Path -LiteralPath $boardsFile)) { throw "Arduino boards file not found: $boardsFile" }
if (-not (Test-Path -LiteralPath $moveModel)) { throw "ESP-SR movemodel.py not found: $moveModel" }
if (-not (Test-Path -LiteralPath $componentManifest)) { throw "ESP-SR component manifest not found: $componentManifest" }
if (-not (Test-Path -LiteralPath $buildOptionsFile)) { throw "Compile the firmware first; build options not found: $buildOptionsFile" }
if (-not (Test-Path -LiteralPath $partitionsFile)) { throw "Compiled esp_sr_16 partitions.csv not found: $partitionsFile" }
$buildOptions = Get-Content -LiteralPath $buildOptionsFile -Raw | ConvertFrom-Json
foreach ($part in $expectedFqbnParts) {
  if ($buildOptions.fqbn -notlike "*$part*") { throw "Build directory does not use required FQBN option: $part" }
}
if ($buildOptions.hardwareFolders -notlike "*$hardwarePath*") {
  throw "Build directory was not compiled with Arduino-ESP32 $ArduinoVersion at $hardwarePath"
}
$partitions = Get-Content -LiteralPath $partitionsFile -Raw
if ($partitions -notmatch '(?im)^model\s*,\s*data\s*,\s*spiffs\s*,\s*0xC10000\s*,\s*0x3E0000\s*,?') {
  throw "Build directory does not contain the expected esp_sr_16 model partition at 0xC10000"
}
$boards = Get-Content -LiteralPath $boardsFile -Raw
if ($boards -notmatch '(?m)^esp32s3\.menu\.PartitionScheme\.esp_sr_16\.upload\.extra_flags=0xC10000 \{build\.path\}/srmodels\.bin$') {
  throw "Arduino-ESP32 $ArduinoVersion does not upload srmodels.bin for esp_sr_16"
}
$versions = Get-Content -LiteralPath $versionsFile -Raw
if ($versions -notmatch "(?m)^espressif__esp-sr:\s*$([regex]::Escape($ExpectedEspSrVersion))\s*$") {
  throw "Arduino-ESP32 $ArduinoVersion does not declare ESP-SR $ExpectedEspSrVersion"
}
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
  $modelBytes = [System.IO.File]::ReadAllBytes($generated)
  $modelText = [System.Text.Encoding]::ASCII.GetString($modelBytes)
  if (-not $modelText.Contains("wn9_hiwalle_tts2")) {
    throw "Generated srmodels.bin does not identify wn9_hiwalle_tts2"
  }
  if ($modelText.Contains("wn9_hiesp")) {
    throw "Generated srmodels.bin still contains wn9_hiesp"
  }
  Copy-Item -LiteralPath $generated -Destination (Join-Path $OutputDirectory "srmodels.bin") -Force
  $prepared = Join-Path $OutputDirectory "srmodels.bin"
  $hash = (Get-FileHash -LiteralPath $prepared -Algorithm SHA256).Hash
  Write-Host "Verified Arduino-ESP32 $ArduinoVersion, esp_sr_16, ESP-SR $ExpectedEspSrVersion"
  Write-Host "Verified wn9_hiwalle_tts2 model partition: $prepared"
  Write-Host "SHA256: $hash"
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
