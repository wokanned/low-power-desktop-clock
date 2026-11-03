param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/low-power/policy' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'power_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/clock" "-I$root/app" "-I$root/device" "-I$root/middleware" `
    (Join-Path $root 'app/power.c') (Join-Path $root 'tests/power_host_test.c') '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Power policy compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Power policy tests failed' }
$timeExecutable = Join-Path $OutputDirectory 'low_power_time_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' "-I$root/platform" `
    (Join-Path $root 'platform/low_power_time.c') (Join-Path $root 'tests/low_power_time_test.c') '-o' $timeExecutable
if ($LASTEXITCODE -ne 0) { throw 'Power time compilation failed' }
& $timeExecutable
if ($LASTEXITCODE -ne 0) { throw 'Power time tests failed' }
