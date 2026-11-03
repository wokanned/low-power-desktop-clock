param([string]$OutputDirectory = 'build/low-power/regression')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $root
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$scripts = @('run_power_host', 'run_adxl345_host_tests', 'run_debug_host_tests',
    'run_sync_protocol_host', 'run_epd_spi_dma_host', 'run_clock_partial_host',
    'run_clock_display_host', 'run_gray4_diagnostic_host', 'run_clock_runtime_host',
    'run_clock_config_host', 'run_storage_files_integration_host', 'run_w25q128_host',
    'run_time_protocol_host', 'run_stm32_clock_host', 'run_weather_feed_host',
    'run_weather_pages_host', 'run_weather_pages_integration_host')
foreach ($name in $scripts) {
    $log = Join-Path $OutputDirectory ($name + '.log')
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot ($name + '.ps1')) *> $log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log; throw "$name failed" }
    Write-Host "PASS: $name"
}
