param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/storage-files-integration-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'storage_files_integration_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    '-DSTORAGE_BOOT_TEST_ENABLE=0' '-DLFS_NO_MALLOC' '-DLFS_NO_DEBUG' '-DLFS_NO_WARN' `
    '-DLFS_NO_ERROR' '-DLFS_NAME_MAX=31' "-I$root/tests/stubs/storage" `
    "-I$root/device" "-I$root/middleware" "-I$root/app" "-I$root/platform" `
    (Join-Path $root 'tests/storage_files_integration_host_test.c') `
    (Join-Path $root 'app/storage.c') (Join-Path $root 'app/clock_config.c') `
    (Join-Path $root 'middleware/file_store.c') `
    (Join-Path $root 'middleware/littlefs/lfs.c') `
    (Join-Path $root 'middleware/littlefs/lfs_util.c') '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Storage files integration compilation failed' }
foreach ($scenario in @('blank', 'formatted', 'diagnostics')) {
    & $executable $scenario
    if ($LASTEXITCODE -ne 0) { throw "Storage files integration failed: $scenario" }
}
Write-Host "PASS: storage files integration tests. Evidence: $OutputDirectory"
