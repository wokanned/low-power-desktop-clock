param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $root 'build/epd-spi-dma-20261028/host'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'epd_spi_dma_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/epd_spi" "-I$root/platform" "-I$root/device" `
    (Join-Path $root 'tests/epd_spi_dma_host_test.c') `
    (Join-Path $root 'platform/stm32_gdey029t94.c') `
    (Join-Path $root 'platform/stm32_debug_uart.c') `
    (Join-Path $root 'device/epd_gdey029t94.c') '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'EPD SPI DMA host compilation failed' }
& $executable | Tee-Object -FilePath (Join-Path $OutputDirectory 'test.log')
if ($LASTEXITCODE -ne 0) { throw 'EPD SPI DMA host tests failed' }
Write-Host "PASS: EPD SPI DMA host tests. Evidence: $OutputDirectory"
