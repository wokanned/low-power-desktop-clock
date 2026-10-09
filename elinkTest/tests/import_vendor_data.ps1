param([switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$vendor = Get-ChildItem -LiteralPath $root -Filter Ap_29demo.h -Recurse |
    Where-Object FullName -Match 'S-GDEY029T94-FP4G-20250210' | Select-Object -First 1
$vendorRoot = [IO.Path]::GetFullPath((Join-Path $vendor.Directory.FullName '../..'))
$encoding = New-Object Text.UTF8Encoding $false
$assets = "#include `"epd_vendor_assets.h`"`n" + [IO.File]::ReadAllText($vendor.FullName)
$files = @{ 'resources/epd_vendor_assets.c' = $assets }
foreach ($size in 12,24) {
    $source = [IO.File]::ReadAllText((Join-Path $vendorRoot "HARDWARE/Fonts/font${size}CN.c"), [Text.Encoding]::GetEncoding(936))
    $source = $source.Replace('"fonts.h"', '"fonts_cn.h"').Replace("cFONT Font${size}CN", "const cFONT Font${size}CN")
    $entries = [regex]::Matches($source, '\{\s*"([^"\r\n]+)",\s*((?:\s*0x[0-9A-Fa-f]+\s*,?)+)\}')
    if ($entries.Count -ne 6) { throw "Unexpected font entry count: $size" }
    foreach ($entry in $entries) {
        $index = [Text.Encoding]::GetEncoding(936).GetBytes($entry.Groups[1].Value)
        $first = '0x{0:X2}U' -f $index[0]
        $second = if ($index.Length -eq 2) { '0x{0:X2}U' -f $index[1] } else { '0x00U' }
        $matrix = $entry.Groups[2].Value.Trim().TrimEnd(',')
        $expected = if ($size -eq 12) {42} else {164}
        if ([regex]::Matches($matrix,'0x[0-9A-Fa-f]+').Count -ne $expected) {throw 'Unexpected bitmap length'}
        $source = $source.Replace($entry.Value, "{{$first, $second}, {`n$matrix}}")
    }
    $files["fonts/font${size}CN.c"] = $source
}
foreach ($relative in $files.Keys) {
    $destination = Join-Path $root "elinkTest/$relative"
    if ($VerifyOnly) {
        if ([IO.File]::ReadAllText($destination) -cne $files[$relative]) {throw "Vendor import mismatch: $relative"}
    } else {
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        [IO.File]::WriteAllText($destination, $files[$relative], $encoding)
    }
}
Write-Host 'PASS: all vendor asset bytes, font indices, matrices and comments match reproducible imports.'
