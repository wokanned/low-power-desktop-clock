param([string]$Compiler = 'gcc', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/file-store-host' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$executable = Join-Path $OutputDirectory 'file_store_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' `
    '-DLFS_NO_MALLOC' '-DLFS_NO_DEBUG' '-DLFS_NO_WARN' '-DLFS_NO_ERROR' '-DLFS_NAME_MAX=31' `
    "-I$root/device" "-I$root/middleware" `
    (Join-Path $root 'tests/file_store_host_test.c') `
    (Join-Path $root 'middleware/file_store.c') `
    (Join-Path $root 'middleware/littlefs/lfs.c') `
    (Join-Path $root 'middleware/littlefs/lfs_util.c') '-Wl,--wrap=lfs_fs_size' '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'File store host compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'File store host tests failed' }
Write-Host "PASS: file store host tests. Evidence: $OutputDirectory"
