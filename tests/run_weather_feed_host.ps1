param([string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/weather-feed-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'weather_feed_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/debug" "-I$root/app" `
    (Join-Path $root 'tests/weather_feed_host_test.c') `
    (Join-Path $root 'app/weather_feed.c') (Join-Path $root 'app/clock_model.c') `
    '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Weather feed host compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Weather feed host tests failed' }
