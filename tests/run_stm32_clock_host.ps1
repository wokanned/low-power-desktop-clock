param(
    [string]$Compiler = 'C:\Users\a1\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'build\stm32-clock-host'
}
$testOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
$testExecutable = Join-Path $testOutput 'stm32_clock_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    '-I' (Join-Path $PSScriptRoot 'stubs\rtc') '-I' (Join-Path $projectRoot 'app') `
    (Join-Path $PSScriptRoot 'stm32_clock_host_test.c') `
    (Join-Path $projectRoot 'platform\stm32_clock.c') `
    (Join-Path $projectRoot 'app\clock_model.c') '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'STM32 calendar adapter host build failed' }
& $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'STM32 calendar adapter host tests failed' }
