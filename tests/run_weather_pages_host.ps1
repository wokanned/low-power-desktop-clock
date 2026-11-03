param([string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe',
      [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'build\weather-pages-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'weather_pages_host_test.exe'
$sources = @('tests/weather_pages_host_test.c', 'app/epd_weather_pages.c',
    'app/clock_model.c', 'resources/weather_pages_assets.c') | ForEach-Object { Join-Path $projectRoot $_ }
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' @sources '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Weather pages compilation failed' }
& $executable $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Weather pages checks failed' }
