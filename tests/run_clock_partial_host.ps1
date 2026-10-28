param(
    [string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'build\clock-partial-host'
}
$testOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
$testExecutable = Join-Path $testOutput 'clock_partial_host_test.exe'
$testSources = @(
    'tests/clock_partial_host_test.c', 'app/epd_bringup.c',
    'app/epd_clock_view.c', 'app/epd_weather_view.c', 'app/display.c',
    'app/weather.c', 'app/clock_schedule.c', 'app/clock_model.c',
    'app/clock_faults.c', 'device/epd_gdey029t94.c',
    'resources/clock_gray_assets.c', 'resources/weather_glyphs.c',
    'gui/GUI_Paint.c', 'fonts/font8.c', 'fonts/font12.c', 'fonts/font16.c',
    'fonts/font20.c', 'fonts/font24.c'
) | ForEach-Object { Join-Path $projectRoot $_ }
$stubDirectory = Join-Path $PSScriptRoot 'stubs\display'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    '-DEPD_VENDOR_DEMO_MODE=0' '-DEPD_CLOCK_USE_GRAY4=0' `
    '-I' $stubDirectory @testSources '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Real raster/controller partial integration compilation failed' }
$scenarios = @('clock-minute', 'weather-minute', 'clock-ampm', 'weather-ampm',
    'narrow-digits', 'date-full', 'format-full', 'weather-full', 'page-full',
    'fault-full', 'outside-full', 'duplicate', 'second-window-failure', 'second-window-timeout',
    'busy-coalesce', 'cleanup-120')
foreach ($scenario in $scenarios) {
    & $testExecutable $scenario
    if ($LASTEXITCODE -ne 0) { throw "Real raster/controller partial integration failed: $scenario" }
}
Write-Host "PASS: real raster/controller partial integration ($($scenarios.Count) scenarios)."
