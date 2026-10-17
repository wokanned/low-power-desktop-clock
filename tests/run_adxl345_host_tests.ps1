param(
    [string]$Compiler = 'C:/Users/a1/x86_64-8.1.0-release-posix-seh-rt_v6-rev0/mingw64/bin/gcc.exe',
    [string]$Python,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Python) {
    $Python = Join-Path $root 'build/adxl345-python/Scripts/python.exe'
    if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) { $Python = 'py' }
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('adxl345-tests-' + [Guid]::NewGuid().ToString('N'))
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$executable = Join-Path $output 'adxl345_host_test.exe'
$fixture = Join-Path $output 'c_encoder.bin'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/device" "-I$root/app" `
    (Join-Path $root 'tests/adxl345_host_test.c') `
    (Join-Path $root 'device/imu_adxl345.c') `
    (Join-Path $root 'app/vofa.c') '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Host C build failed' }
& $executable $fixture
if ($LASTEXITCODE -ne 0) { throw 'Host C tests failed' }
$taskExecutable = Join-Path $output 'accel_task_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    "-I$root/tests/stubs/accel" "-I$root/device" "-I$root/app" "-I$root/platform" `
    (Join-Path $root 'tests/accel_task_test.c') `
    (Join-Path $root 'app/accel.c') '-o' $taskExecutable
if ($LASTEXITCODE -ne 0) { throw 'Host task build failed' }
& $taskExecutable
if ($LASTEXITCODE -ne 0) { throw 'Host task tests failed' }
$previousFixture = $env:ADXL345_C_FIXTURE
try {
    $env:ADXL345_C_FIXTURE = $fixture
    & $Python '-X' 'utf8' (Join-Path $root 'tools/test_justfloat.py') '-v'
    if ($LASTEXITCODE -ne 0) { throw 'Python protocol tests failed' }
    & $Python '-X' 'utf8' (Join-Path $root 'tools/adxl345_capture.py') '--replay' $fixture `
        '--no-plot' '--csv' (Join-Path $output ('replay-' + [Guid]::NewGuid().ToString('N') + '.csv'))
    if ($LASTEXITCODE -ne 0) { throw 'Capture/replay integration failed' }
} finally {
    $env:ADXL345_C_FIXTURE = $previousFixture
}
Write-Host "PASS: host protocol tests and replay. Evidence: $output"
