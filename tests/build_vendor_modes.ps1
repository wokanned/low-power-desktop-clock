param(
    [int[]]$Modes = @(0,1,2,3,4,5,6),
    [string]$Keil = 'D:/realThirdKeil/UV4/UV4.exe',
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$mdk=Join-Path $root 'MDK-ARM'
if(-not $OutputDirectory) {throw 'Specify an evidence output directory'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach($mode in $Modes) {
    if($mode -lt 0 -or $mode -gt 6) {throw 'Invalid build mode'}
    $directory=Join-Path $output "mode-$mode"
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $project=New-Object Xml.XmlDocument
    $project.PreserveWhitespace=$true
    $project.Load((Join-Path $mdk 'DeskClock.uvprojx'))
    $common=$project.Project.Targets.Target.TargetOption.TargetCommonOption
    $relativeOutput=[IO.Path]::GetRelativePath($mdk,$directory)+'\'
    $common.OutputDirectory=$relativeOutput
    $common.ListingPath=$relativeOutput
    $controls=$project.Project.Targets.Target.TargetOption.TargetArmAds.Cads.VariousControls
    $definitions=@($controls.Define -split ',' | ForEach-Object {$_.Trim()} |
        Where-Object {$_ -and $_ -notmatch '^EPD_VENDOR_DEMO_MODE(?:\s*=|$)'})
    $controls.Define=($definitions+"EPD_VENDOR_DEMO_MODE=$mode") -join ','
    $temporary=Join-Path $mdk ('v'+$mode+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)+'.uvprojx')
    $settings=New-Object Xml.XmlWriterSettings
    $settings.Encoding=New-Object Text.UTF8Encoding $false
    $writer=[Xml.XmlWriter]::Create($temporary,$settings)
    $project.Save($writer)
    $writer.Dispose()
    Copy-Item -LiteralPath $temporary -Destination (Join-Path $directory 'build-project.uvprojx')
    $log=Join-Path $directory 'build.log'
    try {
        $arguments="-r $([IO.Path]::GetFileName($temporary)) -t DeskClock -j0 -o `"$log`""
        $process=Start-Process -FilePath $Keil -ArgumentList $arguments -WorkingDirectory $mdk -WindowStyle Hidden -PassThru -Wait
        if(-not (Test-Path -LiteralPath $log)) {throw "Keil did not create a log: exit $($process.ExitCode)"}
        $buildText=Get-Content -LiteralPath $log -Raw
        if($process.ExitCode -ne 0 -or $buildText -notmatch '0 Error\(s\), 0 Warning\(s\)') {
            throw "Keil mode $mode failed: exit $($process.ExitCode)`n$buildText"
        }
        $size=[regex]::Match($buildText,'Code=(\d+) RO-data=(\d+) RW-data=(\d+) ZI-data=(\d+)')
        if(-not $size.Success) {throw 'Missing memory totals'}
        $rom=[int]$size.Groups[1].Value+[int]$size.Groups[2].Value+[int]$size.Groups[3].Value
        $ram=[int]$size.Groups[3].Value+[int]$size.Groups[4].Value
        if($rom -gt 524288 -or $ram -gt 98304) {throw 'Memory budget exceeded'}
        Write-Host "PASS: mode=$mode ROM=$rom RAM=$ram, 0 errors/warnings"
        Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $directory 'DeskClock.hex') | Format-List
    } finally {
        # Only remove the exact disposable project created by this run.
        Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue
    }
}
