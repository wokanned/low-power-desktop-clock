param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/buttons-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$filterExecutable = Join-Path $OutputDirectory 'button_filter_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' "-I$root/middleware" `
    (Join-Path $root 'tests/button_filter_host_test.c') (Join-Path $root 'middleware/button_filter.c') '-o' $filterExecutable
if ($LASTEXITCODE -ne 0) { throw 'Button filter host compilation failed' }
& $filterExecutable
if ($LASTEXITCODE -ne 0) { throw 'Button filter host tests failed' }
$executable = Join-Path $OutputDirectory 'buttons_host_test.exe'
$sources = @('tests/buttons_host_test.c', 'app/buttons.c', 'middleware/button_filter.c',
    'platform/stm32_buttons.c', 'platform/stm32_gpio_exti.c') | ForEach-Object { Join-Path $root $_ }
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/buttons" "-I$root/app" "-I$root/middleware" "-I$root/platform" "-I$root/device" `
    @sources '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Buttons integration host compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Buttons integration host tests failed' }
