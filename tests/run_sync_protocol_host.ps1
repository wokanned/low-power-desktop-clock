param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/sync-protocol-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'sync_protocol_host_test.exe'
$sources = @('tests/stubs/power_stub.c', 'tests/sync_protocol_host_test.c', 'app/debug.c', 'app/debug_console.c',
    'app/vofa.c', 'app/file_transfer.c', 'app/weather.c', 'app/display.c',
    'app/clock_service.c', 'app/clock_model.c', 'app/clock_faults.c',
    'app/forecast.c', 'app/forecast_protocol.c', 'app/weather_feed.c', 'app/time_protocol.c',
    'app/epd_weather_pages.c', 'resources/weather_pages_assets.c', 'resources/weather_glyphs.c') |
    ForEach-Object { Join-Path $root $_ }
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/sync" "-I$root/tests/stubs/debug" "-I$root/tests/stubs/display" `
    "-I$root/app" "-I$root/platform" "-I$root/device" "-I$root/middleware" `
    @sources '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Sync protocol integration build failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Sync protocol integration tests failed' }
