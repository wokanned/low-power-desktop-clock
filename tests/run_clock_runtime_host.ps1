param(
    [string]$Compiler = 'gcc',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $root 'build/clock-runtime-host'
} else {
    [IO.Path]::GetFullPath($OutputDirectory)
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$executable = Join-Path $output 'clock_runtime_host_test.exe'
$arguments = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
    ('-I' + (Join-Path $root 'tests/stubs/clock')), ('-I' + (Join-Path $root 'app')),
    ('-I' + (Join-Path $root 'platform')), ('-I' + (Join-Path $root 'device')),
    ('-I' + (Join-Path $root 'middleware')),
    (Join-Path $root 'app/clock_model.c'), (Join-Path $root 'app/clock_faults.c'),
    (Join-Path $root 'app/clock_schedule.c'), (Join-Path $root 'app/clock_service.c'),
    (Join-Path $root 'tests/clock_runtime_host_test.c'), '-o', $executable)
& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Clock runtime host compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Clock runtime host tests failed' }
