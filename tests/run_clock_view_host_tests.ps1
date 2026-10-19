param(
    [string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'build\clock-view-host'
}
$testOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
$testExecutable = Join-Path $testOutput 'clock_view_host_test.exe'
$testSources = @(
    (Join-Path $projectRoot 'tests\clock_view_host_test.c'),
    (Join-Path $projectRoot 'app\epd_clock_view.c'),
    (Join-Path $projectRoot 'app\clock_model.c'),
    (Join-Path $projectRoot 'resources\clock_gray_assets.c'),
    (Join-Path $projectRoot 'gui\GUI_Paint.c'),
    (Join-Path $projectRoot 'fonts\font8.c'),
    (Join-Path $projectRoot 'fonts\font12.c'),
    (Join-Path $projectRoot 'fonts\font16.c'),
    (Join-Path $projectRoot 'fonts\font20.c'),
    (Join-Path $projectRoot 'fonts\font24.c')
)
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' @testSources '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Clock view host compilation failed.' }
& $testExecutable $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Clock view host checks failed.' }
$python = Join-Path $projectRoot 'build\adxl345-python\Scripts\python.exe'
if (Test-Path -LiteralPath $python) {
    & $python (Join-Path $projectRoot 'tools\check_clock_previews.py') $testOutput
    if ($LASTEXITCODE -ne 0) { throw 'Clock preview checks failed.' }
}
Write-Host "Clock view fixtures: $testOutput"
