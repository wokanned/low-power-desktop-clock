param([string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = Join-Path $root 'build/w25q128-host'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$executable = Join-Path $output 'w25q128_host_test.exe'
$arguments = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
    ('-I' + (Join-Path $root 'device')), (Join-Path $root 'device/flash_w25q128.c'),
    (Join-Path $root 'tests/w25q128_host_test.c'), '-o', $executable)
& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Host compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Host tests failed' }
