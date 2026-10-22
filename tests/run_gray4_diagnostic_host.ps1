param(
    [string]$Compiler = 'C:/Users/a1/x86_64-8.1.0-release-posix-seh-rt_v6-rev0/mingw64/bin/gcc.exe',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'build/gray4-diagnostic-host'
}
$testOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
$renderSources = @('app/epd_clock_view.c', 'app/clock_model.c',
    'resources/clock_gray_assets.c', 'gui/GUI_Paint.c',
    'fonts/font8.c', 'fonts/font12.c', 'fonts/font16.c', 'fonts/font20.c', 'fonts/font24.c') |
    ForEach-Object { Join-Path $projectRoot $_ }
$pixelExecutable = Join-Path $testOutput 'pixel_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    (Join-Path $PSScriptRoot 'gray4_diagnostic_pixels_test.c') @renderSources '-o' $pixelExecutable
if ($LASTEXITCODE -ne 0) { throw 'Gray4 diagnostic pixel compilation failed.' }
& $pixelExecutable $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Gray4 diagnostic pixel tests failed.' }

$testExecutable = Join-Path $testOutput 'gray4_diagnostic_host_test.exe'
$testSources = @('tests/gray4_diagnostic_host_test.c', 'app/display.c',
    'app/epd_bringup.c', 'app/epd_vendor_demo.c', 'app/clock_schedule.c',
    'device/epd_gdey029t94.c', 'resources/epd_vendor_assets.c',
    'fonts/font12CN.c', 'fonts/font24CN.c') | ForEach-Object { Join-Path $projectRoot $_ }
$stubDirectory = Join-Path $PSScriptRoot 'stubs/display'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    '-DEPD_VENDOR_DEMO_MODE=7' '-I' $stubDirectory @testSources @renderSources '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Gray4 diagnostic display integration compilation failed.' }
$scenarios = @('success', 'run-once', 'bind-once', 'bind-always',
    'prepare-once', 'prepare-always', 'init-once', 'init-always',
    'refresh-once', 'refresh-always', 'busy-io-once', 'busy-io-always',
    'busy-timeout-once', 'busy-timeout-always', 'sleep-once', 'sleep-always')
foreach ($scenario in $scenarios) {
    & $testExecutable $scenario
    if ($LASTEXITCODE -ne 0) { throw "Gray4 diagnostic display scenario failed: $scenario" }
}
Write-Host 'PASS: gray4 diagnostic pixels and display integration (16 independent scenarios).'
