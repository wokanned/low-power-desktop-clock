param([string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe',
      [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\weather-host-tests'
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stub = Join-Path $PSScriptRoot 'stubs\debug'
$weatherExe = Join-Path $output 'weather_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$stub" "-I$root/app" `
    (Join-Path $PSScriptRoot 'weather_host_test.c') `
    (Join-Path $root 'app/weather.c') '-o' $weatherExe
if ($LASTEXITCODE -ne 0) { throw 'Weather protocol/state build failed' }
& $weatherExe
if ($LASTEXITCODE -ne 0) { throw 'Weather protocol/state tests failed' }
$viewExe = Join-Path $output 'weather_view_host_test.exe'
$preview = Join-Path $output 'weather-preview.pgm'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$stub" "-I$root/app" "-I$root/device" "-I$root/resources" `
    (Join-Path $PSScriptRoot 'weather_view_host_test.c') `
    (Join-Path $root 'app/epd_weather_view.c') `
    (Join-Path $root 'app/weather.c') (Join-Path $root 'app/clock_model.c') `
    (Join-Path $root 'resources/weather_glyphs.c') '-o' $viewExe
if ($LASTEXITCODE -ne 0) { throw 'Weather renderer build failed' }
& $viewExe $preview
if ($LASTEXITCODE -ne 0) { throw 'Weather renderer tests failed' }
Write-Host "PASS: weather host tests. C-rendered preview: $preview"
