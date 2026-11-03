param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/time-protocol-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'time_protocol_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/app" (Join-Path $root 'tests/time_protocol_host_test.c') `
    (Join-Path $root 'app/time_protocol.c') (Join-Path $root 'app/clock_model.c') `
    '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'TIME2 protocol host build failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'TIME2 protocol host tests failed' }
