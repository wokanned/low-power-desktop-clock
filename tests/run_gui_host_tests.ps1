param(
    [string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
    throw "Host C compiler not found: $Compiler"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $testOutput = Join-Path ([IO.Path]::GetTempPath()) ('epd-gui-tests-' + [Guid]::NewGuid().ToString('N'))
} else {
    $testOutput = [IO.Path]::GetFullPath($OutputDirectory)
}
New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
$testExecutable = Join-Path $testOutput 'gui_host_test.exe'
$testSources = @(
    (Join-Path $projectRoot 'tests\gui_host_test.c'),
    (Join-Path $projectRoot 'gui\GUI_Paint.c'),
    (Join-Path $projectRoot 'fonts\font8.c'),
    (Join-Path $projectRoot 'fonts\font12.c'),
    (Join-Path $projectRoot 'fonts\font16.c'),
    (Join-Path $projectRoot 'fonts\font20.c'),
    (Join-Path $projectRoot 'fonts\font24.c'),
    (Join-Path $projectRoot 'app\epd_clock_view.c'),
    (Join-Path $projectRoot 'app\clock_model.c'),
    (Join-Path $projectRoot 'resources\clock_gray_assets.c')
)

& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' @testSources '-o' $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Host GUI compilation failed with exit code $LASTEXITCODE"
}

& $testExecutable $testOutput
if ($LASTEXITCODE -ne 0) {
    throw "Host GUI tests failed with exit code $LASTEXITCODE"
}

$modeFixture = Join-Path $projectRoot 'tests\mode_config_test.c'
for ($mode = 0; $mode -lt 8; $mode++) {
    $demo = $mode -band 1
    $pattern = ($mode -shr 1) -band 1
    $forceFull = ($mode -shr 2) -band 1
    $expectedSuccess = ($demo + $pattern + $forceFull) -le 1
    $errorLog = Join-Path $testOutput "mode-$mode-compiler.log"
    & $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-fsyntax-only' `
        "-DEPD_GUI_DEMO=$demo" "-DEPD_CLOCK_DIAGNOSTIC_PATTERN=$pattern" `
        "-DEPD_CLOCK_DIAGNOSTIC_FORCE_FULL=$forceFull" '-DEPD_VENDOR_DEMO_MODE=0' $modeFixture 2> $errorLog
    $actualSuccess = $LASTEXITCODE -eq 0
    if ($actualSuccess -ne $expectedSuccess) {
        throw "Mode validation failed: demo=$demo, pattern=$pattern, forceFull=$forceFull"
    }
    if (-not $expectedSuccess) {
        if (-not (Select-String -LiteralPath $errorLog -SimpleMatch 'Select only one EPD diagnostic or GUI demo mode' -Quiet)) {
            throw "Invalid mode combination failed for the wrong reason: $mode"
        }
    }
}
Write-Host 'PASS: all 8 mode combinations, with 4 invalid combinations rejected by the intended diagnostic.'
Write-Host "Host executable: $testExecutable"
Write-Host 'These tests verify framebuffer behavior; hardware refresh and timing still require board tests.'
