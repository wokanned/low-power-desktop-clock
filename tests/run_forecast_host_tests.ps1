param([string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe',
      [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'build/forecast-host-tests' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$testExecutable = Join-Path $OutputDirectory 'forecast_host_test.exe'
$sources = @('tests/stubs/power_stub.c', 'tests/forecast_host_test.c', 'app/forecast.c', 'app/weather.c',
    'app/display.c', 'app/clock_model.c', 'app/epd_weather_view.c',
    'resources/weather_glyphs.c', 'app/weather_feed.c', 'app/epd_weather_pages.c',
    'resources/weather_pages_assets.c') | ForEach-Object { Join-Path $projectRoot $_ }
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$projectRoot/tests/stubs/display" @sources '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Forecast model/display build failed' }
& $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Forecast model/display tests failed' }
