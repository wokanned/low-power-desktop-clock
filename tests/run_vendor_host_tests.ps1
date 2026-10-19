param(
    [string]$Compiler = 'C:/Users/a1/x86_64-8.1.0-release-posix-seh-rt_v6-rev0/mingw64/bin/gcc.exe',
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $OutputDirectory) {$OutputDirectory=Join-Path ([IO.Path]::GetTempPath()) ('epd-vendor-'+[Guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
# Imported assets are verified against origin/EinKTest Git blobs in
# build/epd-host/import-blobs.json; the upstream vendor checkout is not required.
$sources=@('tests/vendor_host_test.c','device/epd_gdey029t94.c','app/epd_vendor_demo.c',
    'app/epd_clock_view.c','app/clock_model.c','resources/clock_gray_assets.c',
    'resources/epd_vendor_assets.c','gui/GUI_Paint.c','fonts/font12CN.c','fonts/font24CN.c',
    'fonts/font8.c','fonts/font12.c','fonts/font16.c','fonts/font20.c','fonts/font24.c') |
    ForEach-Object {Join-Path $root $_}
$exe=Join-Path $OutputDirectory 'vendor_host_test.exe'
& $Compiler '-std=c99' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' @sources '-o' $exe
if($LASTEXITCODE) {throw 'Vendor host compilation failed'}
& $exe $OutputDirectory
if($LASTEXITCODE) {throw 'Vendor host tests failed'}
for($vendor=-1;$vendor -le 7;$vendor++) {
    for($legacy=0;$legacy -lt 8;$legacy++) {
        $expected=($vendor -ge 0 -and $vendor -le 6) -and
            (($vendor -eq 0 -and $legacy -in 0,1,2,4) -or ($vendor -gt 0 -and $legacy -eq 0))
        & $Compiler '-fsyntax-only' "-DEPD_VENDOR_DEMO_MODE=$vendor" `
            "-DEPD_GUI_DEMO=$($legacy -band 1)" "-DEPD_CLOCK_DIAGNOSTIC_PATTERN=$(($legacy -shr 1) -band 1)" `
            "-DEPD_CLOCK_DIAGNOSTIC_FORCE_FULL=$(($legacy -shr 2) -band 1)" `
            (Join-Path $PSScriptRoot 'mode_config_test.c') 2> (Join-Path $OutputDirectory "mode-$vendor-$legacy.log")
        if(($LASTEXITCODE -eq 0) -ne $expected) {throw "Mode validation failed $vendor/$legacy"}
    }
}
Write-Host 'PASS: 72 mode combinations (including invalid selectors).'
