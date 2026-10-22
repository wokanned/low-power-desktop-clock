param(
    [string]$Compiler = 'gcc',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $root 'build/clock-config-host'
} else {
    [IO.Path]::GetFullPath($OutputDirectory)
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$executable = Join-Path $output 'clock_config_host_test.exe'
$arguments = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
    ('-I' + (Join-Path $root 'device')), ('-I' + (Join-Path $root 'app')),
    (Join-Path $root 'app/clock_config.c'),
    (Join-Path $root 'tests/clock_config_host_test.c'), '-o', $executable)
& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Host compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Host tests failed' }
$service = Join-Path $output 'storage_service_host_test.exe'
$serviceArguments = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
    '-DSTORAGE_BOOT_TEST_ENABLE=0', ('-I' + (Join-Path $root 'tests/stubs/storage')),
    ('-I' + (Join-Path $root 'device')), ('-I' + (Join-Path $root 'platform')),
    ('-I' + (Join-Path $root 'app')), (Join-Path $root 'app/storage.c'),
    (Join-Path $root 'tests/storage_service_host_test.c'), '-o', $service)
& $Compiler @serviceArguments
if ($LASTEXITCODE -ne 0) { throw 'Storage service host compilation failed' }
foreach ($scenario in @('normal', 'init-failure', 'load-failure', 'save-failure', 'late-commit',
    'corrupt', 'bind-failure', 'task-failure', 'queue-failure')) {
    & $service $scenario
    if ($LASTEXITCODE -ne 0) { throw ('Storage service failed: ' + $scenario) }
}
$usage = Join-Path $output 'storage_usage_host_test.exe'
$usageArguments = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
    '-DSTORAGE_BOOT_TEST_ENABLE=0', ('-I' + (Join-Path $root 'tests/stubs/storage')),
    ('-I' + (Join-Path $root 'device')), ('-I' + (Join-Path $root 'platform')),
    ('-I' + (Join-Path $root 'app')), (Join-Path $root 'app/storage.c'),
    (Join-Path $root 'tests/storage_usage_host_test.c'), '-o', $usage)
& $Compiler @usageArguments
if ($LASTEXITCODE -ne 0) { throw 'Storage usage host compilation failed' }
foreach ($scenario in @('empty', 'full', 'boundaries', 'periodic', 'tick-wrap', 'timer-preempt',
    'read-failure', 'init-failure', 'retry-init-failure', 'tail-read-failure',
    'requests', 'config-tail', 'partial-write',
    'late-commit-scan', 'fault-aggregation')) {
    & $usage $scenario
    if ($LASTEXITCODE -ne 0) { throw ('Storage usage failed: ' + $scenario) }
}
$bootUsage = Join-Path $output 'storage_usage_boot_host_test.exe'
$bootUsageArguments = @($usageArguments | ForEach-Object {
    if ($_ -eq '-DSTORAGE_BOOT_TEST_ENABLE=0') { '-DSTORAGE_BOOT_TEST_ENABLE=1' }
    elseif ($_ -eq $usage) { $bootUsage }
    else { $_ }
})
& $Compiler @bootUsageArguments
if ($LASTEXITCODE -ne 0) { throw 'Storage usage boot host compilation failed' }
& $bootUsage 'boot-failure'
if ($LASTEXITCODE -ne 0) { throw 'Storage usage failed: boot-failure' }
