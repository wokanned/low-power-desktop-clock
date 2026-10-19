param(
    [string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'build\clock-display-host'
}
$testOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
$testExecutable = Join-Path $testOutput 'clock_display_host_test.exe'
$testSources = @(
    (Join-Path $projectRoot 'tests\clock_display_host_test.c'),
    (Join-Path $projectRoot 'app\epd_bringup.c'),
    (Join-Path $projectRoot 'app\display.c'),
    (Join-Path $projectRoot 'app\clock_schedule.c'),
    (Join-Path $projectRoot 'app\clock_model.c')
)
$stubDirectory = Join-Path $PSScriptRoot 'stubs\display'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    '-DEPD_VENDOR_DEMO_MODE=0' '-I' $stubDirectory @testSources '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Production clock display host compilation failed.' }
$scenarios = @('startup', 'minute', 'rtc-error', 'retry-limit', 'retry-success',
    'init-retry', 'retry-budget', 'sleep-failure', 'coalesce',
    'failed-fault-coalesce', 'failed-recovery-coalesce')
foreach ($scenario in $scenarios) {
    & $testExecutable $scenario
    if ($LASTEXITCODE -ne 0) { throw "Production clock display scenario failed: $scenario" }
}
Write-Host 'PASS: production bringup/display/schedule integration (11 independent scenarios).'
