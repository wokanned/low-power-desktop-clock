param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/weather-pages-integration-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'weather_pages_integration_host_test.exe'
$sources = @('tests/weather_pages_integration_host_test.c', 'app/weather_feed.c',
    'app/display.c', 'app/weather.c', 'app/forecast.c', 'app/clock_model.c',
    'app/epd_weather_pages.c', 'resources/weather_pages_assets.c') |
    ForEach-Object { Join-Path $root $_ }
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/display" "-I$root/app" @sources '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Weather pages integration host build failed' }
& $executable $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Weather pages integration host tests failed' }
