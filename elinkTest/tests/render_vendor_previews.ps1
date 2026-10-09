param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
foreach($file in (Get-ChildItem -LiteralPath $Directory -Filter '*.bin')) {
    $data=[IO.File]::ReadAllBytes($file.FullName)
    $bpp=if($data.Length -eq 9472){2}else{1}
    $perByte=8/$bpp
    $gui=$file.BaseName.StartsWith('gui-')
    $bitmap=New-Object Drawing.Bitmap 296,128
    for($y=0;$y -lt 128;$y++) {for($x=0;$x -lt 296;$x++) {
        # GUI preview decodes the unmirrored logical canvas, not the 342-gate panel.
        $px=if($gui){$y}else{127-$y}
        $py=295-$x
        $shade=($data[$py*(128/$perByte)+[int][Math]::Floor($px/$perByte)] -shr (8-$bpp-($px%$perByte)*$bpp)) -band ((1 -shl $bpp)-1)
        $level=if($bpp -eq 2){255-$shade*85}else{$shade*255}
        $bitmap.SetPixel($x,$y,[Drawing.Color]::FromArgb($level,$level,$level))
    }}
    $destination=Join-Path $file.Directory.FullName ($file.BaseName+'.png')
    $bitmap.Save($destination,[Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Write-Host "Preview: $destination"
}
