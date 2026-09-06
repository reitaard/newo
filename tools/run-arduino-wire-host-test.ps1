$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$compiler = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $compiler) { $compiler = 'C:\msys64\mingw64\bin\g++.exe' }
if (-not (Test-Path -LiteralPath $compiler)) { throw 'g++ is required for this host test' }
$env:PATH = (Split-Path $compiler -Parent) + [IO.Path]::PathSeparator + $env:PATH
$output = Join-Path $repo 'build/arduino-wire-host-test.exe'
New-Item -ItemType Directory -Force (Split-Path $output -Parent) | Out-Null
& $compiler -std=c++17 -Wall -Wextra -Werror '-static' (Join-Path $repo 'tools/arduino-wire-host-test.cpp') -o $output
if ($LASTEXITCODE -ne 0) { throw 'Host test compilation failed' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Host tests failed' }
