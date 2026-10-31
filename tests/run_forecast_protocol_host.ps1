param([string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/forecast-protocol-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'forecast_protocol_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/debug" "-I$root/app" "-I$root/device" "-I$root/middleware" `
    (Join-Path $root 'tests/forecast_protocol_host_test.c') `
    (Join-Path $root 'app/forecast_protocol.c') (Join-Path $root 'app/forecast.c') `
    (Join-Path $root 'app/clock_model.c') '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Forecast protocol host compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Forecast protocol host tests failed' }
Write-Host "PASS: forecast protocol tests. Evidence: $OutputDirectory"
