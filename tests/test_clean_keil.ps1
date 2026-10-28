param(
    [string]$ScriptPath = (Join-Path $PSScriptRoot '../tools/clean_keil.ps1')
)

$ErrorActionPreference = 'Stop'
$script:Passed = 0
$script:Failed = 0
$script:Skipped = 0
$script:CreatedLinks = New-Object 'System.Collections.Generic.List[string]'
$script:FixturePrefix = 'deskclock_clean_keil_tests_'
$script:FixtureRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) (
    $script:FixturePrefix + [Guid]::NewGuid().ToString('N') + ' 空格[中文]')))
$script:Utf8 = New-Object System.Text.UTF8Encoding($false)
$ScriptPath = [IO.Path]::GetFullPath($ScriptPath)

if (-not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
    throw ('Cleaner script does not exist: ' + $ScriptPath)
}
$engineCommand = Get-Command powershell.exe -ErrorAction SilentlyContinue
if ($null -eq $engineCommand) {
    $engineCommand = Get-Command pwsh -ErrorAction Stop
}
$script:PowerShellEngine = $engineCommand.Source
[IO.Directory]::CreateDirectory($script:FixtureRoot) | Out-Null

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-File {
    param([string]$Path, [bool]$Exists = $true)
    $actual = Test-Path -LiteralPath $Path -PathType Leaf
    Assert-True ($actual -eq $Exists) ('Unexpected file state: ' + $Path + '; expected exists=' + $Exists)
}

function Write-FixtureFile {
    param([string]$Path, [string]$Content = 'fixture sentinel')
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    [IO.File]::WriteAllText($Path, $Content, $script:Utf8)
}

function New-Fixture {
    param([string]$Name)
    $path = Join-Path $script:FixtureRoot $Name
    [IO.Directory]::CreateDirectory($path) | Out-Null
    return $path
}

function Write-KeilProject {
    param(
        [string]$Path,
        [string]$OutputDirectory = '.\Objects\',
        [string]$ListingPath = '.\Listings\',
        [string]$InputPath = '.\Objects\referenced.o'
    )
    $outputXml = [Security.SecurityElement]::Escape($OutputDirectory)
    $listingXml = [Security.SecurityElement]::Escape($ListingPath)
    $inputXml = [Security.SecurityElement]::Escape($InputPath)
    $xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<Project><SchemaVersion>2.1</SchemaVersion><Targets><Target>
  <TargetName>fixture</TargetName><TargetOption><TargetCommonOption>
    <OutputDirectory>$outputXml</OutputDirectory>
    <ListingPath>$listingXml</ListingPath>
  </TargetCommonOption></TargetOption>
  <Groups><Group><GroupName>Inputs</GroupName><Files><File>
    <FileName>referenced.o</FileName><FileType>1</FileType><FilePath>$inputXml</FilePath>
  </File></Files></Group></Groups>
</Target></Targets></Project>
"@
    Write-FixtureFile $Path $xml
}

function Quote-ProcessArgument {
    param([string]$Value)
    # Windows paths cannot contain a quote. Double final backslashes before the closing quote.
    if ($Value.Contains('"')) { throw 'Unexpected quote in a test process argument' }
    $trailing = [regex]::Match($Value, '\\+$').Value
    return ('"' + $Value + $trailing + '"')
}

function Invoke-Cleaner {
    param([string]$RootPath, [string[]]$ExtraArguments = @())
    $arguments = @('-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
        '-File', $ScriptPath, '-Root', $RootPath) + $ExtraArguments
    $quoted = @($arguments | ForEach-Object { Quote-ProcessArgument $_ })
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $script:PowerShellEngine
    $start.Arguments = $quoted -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    return (Invoke-TestProcess $start)
}

function Invoke-BatchCleaner {
    param([string]$BatchPath, [switch]$Preview)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $env:ComSpec
    $command = '"' + $BatchPath + '"'
    if ($Preview) { $command += ' -Preview' }
    $start.Arguments = '/d /c "' + $command + '"'
    $start.WorkingDirectory = [IO.Path]::GetTempPath()
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    return (Invoke-TestProcess $start)
}

function Invoke-TestProcess {
    param([Diagnostics.ProcessStartInfo]$StartInfo)
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $StartInfo
    try {
        Assert-True ($process.Start()) 'Could not start cleaner process'
        if ($StartInfo.RedirectStandardInput) { $process.StandardInput.Close() }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdoutTask.GetAwaiter().GetResult()
            Stderr = $stderrTask.GetAwaiter().GetResult()
        }
    } finally {
        $process.Dispose()
    }
}

function Assert-ExitCode {
    param($Result, [int]$Expected)
    Assert-True ($Result.ExitCode -eq $Expected) (
        'Expected exit ' + $Expected + ', got ' + $Result.ExitCode + "`n" + $Result.Stdout + "`n" + $Result.Stderr)
}

function Get-FixtureSnapshot {
    param([string]$Path)
    $rows = @(Get-ChildItem -LiteralPath $Path -Recurse -Force -File | Sort-Object FullName | ForEach-Object {
        $_.FullName.Substring($Path.Length) + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    })
    return ($rows -join "`n")
}

function Run-Test {
    param([string]$Name, [scriptblock]$Body)
    try {
        $skippedBefore = $script:Skipped
        & $Body
        if ($script:Skipped -eq $skippedBefore) {
            $script:Passed++
            Write-Host ('PASS: ' + $Name)
        }
    } catch {
        $script:Failed++
        Write-Host ('FAIL: ' + $Name + "`n" + $_.Exception.Message)
    }
}

function Assert-OwnedPath {
    param([string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $script:FixtureRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    Assert-True ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) (
        'Refusing fixture cleanup outside its owned root: ' + $full)
    return $full
}

try {
    Run-Test 'preview preserves every fixture file and its contents' {
        $root = New-Fixture 'preview 工程[1]'
        Write-KeilProject (Join-Path $root 'demo.uvprojx')
        Write-FixtureFile (Join-Path $root 'Objects/nested 缓存[2]/remove.o')
        Write-FixtureFile (Join-Path $root 'Listings/remove.lst')
        Write-FixtureFile (Join-Path $root 'Objects/firmware.hex')
        $before = Get-FixtureSnapshot $root
        Assert-ExitCode (Invoke-Cleaner $root @('-Preview', '-Details')) 0
        Assert-True ((Get-FixtureSnapshot $root) -ceq $before) 'Preview changed the fixture tree'
    }

    Run-Test 'exact intermediate allowlist, XML input protection, and repeat execution' {
        $root = New-Fixture 'whitelist'
        Write-KeilProject (Join-Path $root 'demo.uvprojx')
        $delete = @('Objects/a.o', 'Objects/b.OBJ', 'Objects/c.crf', 'Objects/d.d',
            'Objects/e.dep', 'Listings/f.lst', 'Objects/g.lnp', 'same_folder.o',
            'Objects/nested 空格[3]/deep.o')
        $keep = @('Objects/referenced.o', 'Objects/firmware.axf', 'Objects/firmware.hex',
            'Objects/capture.bin', 'Objects/firmware.elf', 'Objects/firmware.map',
            'Objects/build.log', 'Objects/build.htm', 'Objects/layout.sct', 'Objects/source.c',
            'Objects/source.h', 'Objects/board.ioc', 'Objects/settings.uvoptx',
            'Objects/state.json', 'Objects/data.csv', 'Objects/frame.png', 'Objects/backup.bak',
            'Objects/host.exe', 'assets/unrelated.o', 'src/unrelated.obj')
        foreach ($relative in @($delete + $keep)) { Write-FixtureFile (Join-Path $root $relative) }
        Assert-ExitCode (Invoke-Cleaner $root @('-Details')) 0
        foreach ($relative in $delete) { Assert-File (Join-Path $root $relative) $false }
        foreach ($relative in $keep) { Assert-File (Join-Path $root $relative) }
        Assert-File (Join-Path $root 'demo.uvprojx')
        $after = Get-FixtureSnapshot $root
        Assert-ExitCode (Invoke-Cleaner $root) 0
        Assert-True ((Get-FixtureSnapshot $root) -ceq $after) 'Repeat execution changed the preserved tree'
    }

    Run-Test 'version control, dependency, and Python environment trees are excluded' {
        $root = New-Fixture 'excluded'
        Write-KeilProject (Join-Path $root 'demo.uvprojx')
        Write-FixtureFile (Join-Path $root 'Objects/remove.o')
        $skipped = @('.git', '.svn', '.hg', 'node_modules', '.venv', 'venv', 'env')
        foreach ($name in $skipped) {
            Write-FixtureFile (Join-Path $root ('Objects/' + $name + '/keep.o'))
            Write-FixtureFile (Join-Path $root ($name + '/keep.crf'))
            Write-FixtureFile (Join-Path $root ($name + '/broken.uvprojx')) '<broken'
        }
        Write-FixtureFile (Join-Path $root 'Objects/python-runtime/pyvenv.cfg') 'home = fixture'
        Write-FixtureFile (Join-Path $root 'Objects/python-runtime/Lib/keep.o')
        Write-FixtureFile (Join-Path $root 'python-other/pyvenv.cfg') 'home = fixture'
        Write-FixtureFile (Join-Path $root 'python-other/broken.uvprojx') '<broken'
        Assert-ExitCode (Invoke-Cleaner $root) 0
        Assert-File (Join-Path $root 'Objects/remove.o') $false
        foreach ($name in $skipped) {
            Assert-File (Join-Path $root ('Objects/' + $name + '/keep.o'))
            Assert-File (Join-Path $root ($name + '/keep.crf'))
        }
        Assert-File (Join-Path $root 'Objects/python-runtime/Lib/keep.o')
        Assert-File (Join-Path $root 'Objects/python-runtime/pyvenv.cfg')
        Assert-File (Join-Path $root 'python-other/broken.uvprojx')
    }

    Run-Test 'macro and wildcard object inputs conservatively protect their extensions' {
        $scenarios = @(
            @{ Name = 'macro object input'; Input = '$(OBJECT_NAME).obj'; Keep = 'foo.obj'; Remove = 'foo.o' },
            @{ Name = 'wildcard object input'; Input = '*.o'; Keep = 'foo.o'; Remove = 'foo.obj' }
        )
        foreach ($scenario in $scenarios) {
            $root = New-Fixture $scenario.Name
            Write-KeilProject -Path (Join-Path $root 'demo.uvprojx') -InputPath $scenario.Input
            $keep = Join-Path $root ('build/' + $scenario.Keep)
            $remove = Join-Path $root ('build/' + $scenario.Remove)
            $other = Join-Path $root 'build/other.crf'
            Write-FixtureFile $keep
            Write-FixtureFile $remove
            Write-FixtureFile $other
            Assert-ExitCode (Invoke-Cleaner $root) 0
            Assert-File $keep
            Assert-File $remove $false
            Assert-File $other $false
        }
    }

    Run-Test 'linker MiscControls object references are preserved independently of FilePath entries' {
        $root = New-Fixture 'linker object input'
        $project = Join-Path $root 'demo.uvprojx'
        Write-KeilProject -Path $project -InputPath '.\source.c'
        $linkerOptions = '<TargetArmAds><LDads><MiscControls>build/prebuilt.obj</MiscControls></LDads></TargetArmAds>'
        $xml = [IO.File]::ReadAllText($project).Replace('</TargetOption>', ($linkerOptions + '</TargetOption>'))
        Write-FixtureFile $project $xml
        $keep = Join-Path $root 'build/prebuilt.obj'
        $remove = Join-Path $root 'build/other.obj'
        Write-FixtureFile $keep
        Write-FixtureFile $remove
        Assert-ExitCode (Invoke-Cleaner $root) 0
        Assert-File $keep
        Assert-File $remove $false
    }

    Run-Test 'malformed generated paths preserve object inputs and allow valid output cleanup' {
        $root = New-Fixture 'malformed generated paths'
        Write-KeilProject -Path (Join-Path $root 'demo.uvprojx') `
            -InputPath '../D:/source/prebuilt.obj' -OutputDirectory '../D:/output/' -ListingPath '../D:/listings/'
        $keep = Join-Path $root 'build/prebuilt.obj'
        $remove = Join-Path $root 'build/foo.o'
        Write-FixtureFile $keep
        Write-FixtureFile $remove
        Assert-ExitCode (Invoke-Cleaner $root) 0
        Assert-File $keep
        Assert-File $remove $false
    }

    Run-Test 'XML output and listing directories with nonstandard names' {
        $root = New-Fixture 'legacy XML'
        Write-KeilProject -Path (Join-Path $root 'legacy.uvproj') `
            -OutputDirectory '.\custom output [中文]\' -ListingPath '.\custom listings [中文]\'
        Write-FixtureFile (Join-Path $root 'custom output [中文]/remove.obj')
        Write-FixtureFile (Join-Path $root 'custom listings [中文]/remove.lst')
        Write-FixtureFile (Join-Path $root 'assets/keep.o')
        Assert-ExitCode (Invoke-Cleaner $root) 0
        Assert-File (Join-Path $root 'custom output [中文]/remove.obj') $false
        Assert-File (Join-Path $root 'custom listings [中文]/remove.lst') $false
        Assert-File (Join-Path $root 'assets/keep.o')
    }

    Run-Test 'nested MDK-ARM project, root build history, and same-directory listing' {
        $root = New-Fixture 'repository layout'
        Write-KeilProject -Path (Join-Path $root 'MDK-ARM/demo.uvprojx') -OutputDirectory 'DeskClock\'
        $delete = @('MDK-ARM/DeskClock/remove.o', 'MDK-ARM/startup.lst', 'build/history/mode-0/remove.crf')
        foreach ($relative in $delete) { Write-FixtureFile (Join-Path $root $relative) }
        Write-FixtureFile (Join-Path $root 'assets/keep.o')
        Assert-ExitCode (Invoke-Cleaner $root) 0
        foreach ($relative in $delete) { Assert-File (Join-Path $root $relative) $false }
        Assert-File (Join-Path $root 'assets/keep.o')
    }

    Run-Test 'relative and absolute external output paths stay outside the cleanup boundary' {
        $fixture = New-Fixture 'external output'
        $root = Join-Path $fixture 'project'
        $outside = Join-Path $fixture 'outside'
        Write-KeilProject -Path (Join-Path $root 'demo.uvprojx') `
            -OutputDirectory '..\outside\' -ListingPath $outside
        Write-FixtureFile (Join-Path $root 'Objects/remove.o')
        Write-FixtureFile (Join-Path $outside 'keep.o')
        Write-FixtureFile (Join-Path $outside 'keep.lst')
        Assert-ExitCode (Invoke-Cleaner $root) 0
        Assert-File (Join-Path $root 'Objects/remove.o') $false
        Assert-File (Join-Path $outside 'keep.o')
        Assert-File (Join-Path $outside 'keep.lst')
    }

    Run-Test 'invalid paths and non-Keil directories are rejected without deleting files' {
        $root = New-Fixture 'invalid input'
        Write-FixtureFile (Join-Path $root 'Objects/keep.o')
        Assert-ExitCode (Invoke-Cleaner (Join-Path $root 'missing')) 2
        Assert-ExitCode (Invoke-Cleaner (Join-Path $root 'Objects/keep.o')) 2
        Assert-ExitCode (Invoke-Cleaner $root) 2
        Assert-File (Join-Path $root 'Objects/keep.o')
    }

    Run-Test 'a locked intermediate reports partial failure and leaves other candidates cleanable' {
        $root = New-Fixture 'partial failure'
        Write-KeilProject (Join-Path $root 'demo.uvprojx')
        $lockedPath = Join-Path $root 'Objects/locked.o'
        $normalPath = Join-Path $root 'Objects/remove.o'
        Write-FixtureFile $lockedPath
        Write-FixtureFile $normalPath
        $handle = [IO.File]::Open($lockedPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            Assert-ExitCode (Invoke-Cleaner $root) 1
            Assert-File $lockedPath
            Assert-File $normalPath $false
        } finally {
            $handle.Dispose()
        }
    }

    Run-Test 'batch preview and default cleanup use the batch location from an outside working directory' {
        $root = New-Fixture 'batch 默认根目录[4]'
        $batchSource = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetDirectoryName($ScriptPath))) 'clean_keil.bat'
        $batchPath = Join-Path $root 'clean_keil.bat'
        $copiedScript = Join-Path $root 'tools/clean_keil.ps1'
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($copiedScript)) | Out-Null
        [IO.File]::Copy($batchSource, $batchPath)
        [IO.File]::Copy($ScriptPath, $copiedScript)
        Write-KeilProject (Join-Path $root 'demo.uvprojx')
        $remove = Join-Path $root 'Objects/remove.o'
        $keep = Join-Path $root 'Objects/firmware.hex'
        Write-FixtureFile $remove
        Write-FixtureFile $keep
        $before = Get-FixtureSnapshot $root
        Assert-ExitCode (Invoke-BatchCleaner -BatchPath $batchPath -Preview) 0
        Assert-True ((Get-FixtureSnapshot $root) -ceq $before) 'Batch preview changed the fixture tree'
        Assert-ExitCode (Invoke-BatchCleaner -BatchPath $batchPath) 0
        Assert-File $remove $false
        Assert-File $keep
        Assert-File $batchPath
        Assert-File $copiedScript
    }

    Run-Test 'directory junctions do not expose external files to deletion' {
        $fixture = New-Fixture 'junction boundary'
        $root = Join-Path $fixture 'project'
        $outside = Join-Path $fixture 'outside'
        $link = Join-Path $root 'Objects/linked'
        Write-KeilProject (Join-Path $root 'demo.uvprojx')
        Write-FixtureFile (Join-Path $root 'Objects/remove.o')
        Write-FixtureFile (Join-Path $outside 'keep.o')
        try {
            New-Item -ItemType Junction -Path $link -Target ([WildcardPattern]::Escape($outside)) -ErrorAction Stop | Out-Null
        } catch {
            $script:Skipped++
            Write-Host ('SKIP: junction creation is unavailable: ' + $_.Exception.Message)
            return
        }
        $script:CreatedLinks.Add($link)
        Assert-ExitCode (Invoke-Cleaner $root) 0
        Assert-File (Join-Path $root 'Objects/remove.o') $false
        Assert-File (Join-Path $outside 'keep.o')
        Assert-True (Test-Path -LiteralPath $link) 'Cleaner removed the junction'
    }
} finally {
    # Unlink junctions first. Never recurse into their targets during fixture cleanup.
    foreach ($link in $script:CreatedLinks) {
        $ownedLink = Assert-OwnedPath $link
        if (Test-Path -LiteralPath $ownedLink) { [IO.Directory]::Delete($ownedLink) }
    }
    $resolvedRoot = [IO.Path]::GetFullPath($script:FixtureRoot)
    $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
    Assert-True ([IO.Path]::GetDirectoryName($resolvedRoot).Equals($resolvedTemp, [StringComparison]::OrdinalIgnoreCase)) `
        'Refusing cleanup: fixture root is not directly under the system temporary directory'
    Assert-True ([IO.Path]::GetFileName($resolvedRoot).StartsWith($script:FixturePrefix, [StringComparison]::Ordinal)) `
        'Refusing cleanup: fixture root has an unexpected name'
    if (Test-Path -LiteralPath $resolvedRoot) { Remove-Item -LiteralPath $resolvedRoot -Recurse -Force }
}

Write-Host ('Integration tests: ' + $script:Passed + ' passed, ' + $script:Failed + ' failed, ' + $script:Skipped + ' skipped')
if ($script:Failed -gt 0) { exit 1 }
exit 0
