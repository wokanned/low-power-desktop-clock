param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/debug-host-tests' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$parserExe = Join-Path $OutputDirectory 'debug_console_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' "-I$root/app" `
    (Join-Path $root 'tests/debug_console_host_test.c') `
    (Join-Path $root 'app/debug_console.c') (Join-Path $root 'app/clock_model.c') '-o' $parserExe
if ($LASTEXITCODE -ne 0) { throw 'Console parser build failed' }
& $parserExe
if ($LASTEXITCODE -ne 0) { throw 'Console parser tests failed' }
$taskExe = Join-Path $OutputDirectory 'debug_task_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/debug" "-I$root/app" "-I$root/platform" "-I$root/device" "-I$root/middleware" `
    (Join-Path $root 'tests/debug_task_host_test.c') `
    (Join-Path $root 'app/debug.c') (Join-Path $root 'app/debug_console.c') `
    (Join-Path $root 'app/vofa.c') (Join-Path $root 'app/weather.c') `
    (Join-Path $root 'app/file_transfer.c') `
    (Join-Path $root 'app/clock_model.c') '-o' $taskExe
if ($LASTEXITCODE -ne 0) { throw 'Debug task build failed' }
& $taskExe
if ($LASTEXITCODE -ne 0) { throw 'Debug task tests failed' }
$uartExe = Join-Path $OutputDirectory 'debug_uart_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' '-DTEST_DEBUG_UART_CRITICAL' `
    "-I$root/tests/stubs/debug" "-I$root/platform" `
    (Join-Path $root 'tests/debug_uart_host_test.c') `
    (Join-Path $root 'platform/stm32_debug_uart.c') '-o' $uartExe
if ($LASTEXITCODE -ne 0) { throw 'Debug UART build failed' }
& $uartExe
if ($LASTEXITCODE -ne 0) { throw 'Debug UART tests failed' }
$transferExe = Join-Path $OutputDirectory 'file_transfer_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/app" "-I$root/device" "-I$root/middleware" `
    (Join-Path $root 'tests/file_transfer_host_test.c') `
    (Join-Path $root 'app/file_transfer.c') '-o' $transferExe
if ($LASTEXITCODE -ne 0) { throw 'File transfer build failed' }
& $transferExe
if ($LASTEXITCODE -ne 0) { throw 'File transfer tests failed' }
Write-Host "PASS: debug host tests. Evidence: $OutputDirectory"
