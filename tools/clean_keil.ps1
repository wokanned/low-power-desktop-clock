#requires -Version 5.1
<#
.SYNOPSIS
清理 Keil 编译中间文件，保留固件、报告、日志和工程输入。
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\clean_keil.ps1 -Preview
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\clean_keil.ps1 -Root 'D:\OtherProject'
#>
[CmdletBinding()]
param(
    [string]$Root,
    [switch]$Preview,
    [switch]$Details
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if ([Console]::IsOutputRedirected) {
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
}
if (-not $PSBoundParameters.ContainsKey('Root')) {
    # Windows PowerShell 5.1 may not populate PSScriptRoot during parameter defaults.
    $Root = Split-Path -Parent $PSScriptRoot
}

function New-PathSet {
    # The unary comma keeps PowerShell from enumerating an empty collection.
    return ,([Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase))
}

function Test-InRoot {
    param([string]$Path, [string]$Boundary)
    return $Path.Equals($Boundary, [StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith($Boundary + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)
}

function Test-LinkInPath {
    param([string]$Path)
    $current = $Path
    while ($current) {
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return $true
        }
        $parent = [IO.Path]::GetDirectoryName($current.TrimEnd('\', '/'))
        if (-not $parent -or $parent -eq $current) { break }
        $current = $parent
    }
    return $false
}

function Format-ByteCount {
    param([long]$Bytes)
    if ($Bytes -ge 1GB) { return ('{0:N2} GiB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:N2} MiB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:N2} KiB' -f ($Bytes / 1KB)) }
    return ('{0} B' -f $Bytes)
}

function Resolve-ProjectPath {
    param([string]$Value, [string]$ProjectDirectory)
    $valueText = $Value.Trim().Trim('"')
    # Keil macros and wildcards are not filesystem paths. Never expand them.
    if (-not $valueText -or $valueText -match '[$%*?]') { return $null }
    try {
        if ([IO.Path]::IsPathRooted($valueText)) {
            return [IO.Path]::GetFullPath($valueText).TrimEnd('\', '/')
        }
        return [IO.Path]::GetFullPath((Join-Path $ProjectDirectory $valueText)).TrimEnd('\', '/')
    } catch {
        # Some generated projects contain malformed paths (for example ../D:/...).
        # Output paths are skipped; unresolved object inputs are protected by name below.
        return $null
    }
}

function Invoke-KeilCleanup {
    $extensions = New-PathSet
    foreach ($extension in @('.o', '.obj', '.crf', '.d', '.dep', '.lst', '.lnp')) {
        [void]$extensions.Add($extension)
    }
    $excludedNames = New-PathSet
    foreach ($name in @('.git', '.svn', '.hg', 'node_modules', '.venv', 'venv', 'env',
            '.vscode', '.idea', '.embeddedskills', 'RTE', 'DebugConfig')) {
        [void]$excludedNames.Add($name)
    }
    $buildNames = New-PathSet
    foreach ($name in @('build', 'Objects', 'Listings', 'Debug', 'Release')) {
        [void]$buildNames.Add($name)
    }

    try {
        $rootItem = Get-Item -LiteralPath $Root -Force -ErrorAction Stop
        if ($rootItem -isnot [IO.DirectoryInfo]) { throw '需要指定工程文件夹。' }
        $rootPath = [IO.Path]::GetFullPath($rootItem.FullName).TrimEnd('\', '/')
        $volumeRoot = [IO.Path]::GetPathRoot($rootItem.FullName).TrimEnd('\', '/')
        if ($rootPath -eq $volumeRoot) { throw '请指定工程文件夹，不能指定磁盘根目录。' }
        if ($excludedNames.Contains($rootItem.Name) -or
                (Test-Path -LiteralPath (Join-Path $rootPath 'pyvenv.cfg'))) {
            throw '指定的目录属于版本库、工具配置或虚拟环境。'
        }
        if (Test-LinkInPath $rootPath) { throw '工程路径包含目录链接，请使用实际目录。' }
    } catch {
        Write-Host ('输入无效：' + $_.Exception.Message) -ForegroundColor Red
        return 2
    }

    Write-Host ('工程目录：' + $rootPath)
    if ($Preview) { Write-Host '模式：只读预览' }
    else { Write-Host '模式：直接清理编译中间文件' }

    $projects = [Collections.Generic.List[IO.FileInfo]]::new()
    $intermediates = [Collections.Generic.List[IO.FileInfo]]::new()
    $scanErrors = [Collections.Generic.List[string]]::new()
    $stack = [Collections.Generic.Stack[string]]::new()
    $stack.Push($rootPath)
    $skippedDirectories = 0
    while ($stack.Count -gt 0) {
        $directory = $stack.Pop()
        try {
            # Recheck before entering each directory, including ancestors.
            if (-not (Test-InRoot $directory $rootPath) -or (Test-LinkInPath $directory)) {
                $skippedDirectories++
                continue
            }
            $children = @(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop)
            if (@($children | Where-Object { $_.Name -ieq 'pyvenv.cfg' }).Count -gt 0) {
                $skippedDirectories++
                continue
            }
            foreach ($child in $children) {
                if (($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                    $skippedDirectories++
                    continue
                }
                if ($child.PSIsContainer) {
                    if ($excludedNames.Contains($child.Name)) { $skippedDirectories++ }
                    else { $stack.Push($child.FullName) }
                } elseif ($child.Extension -in @('.uvprojx', '.uvproj')) {
                    $projects.Add($child)
                } elseif ($extensions.Contains($child.Extension)) {
                    $intermediates.Add($child)
                }
            }
        } catch {
            $scanErrors.Add($directory + ': ' + $_.Exception.Message)
        }
    }
    if ($scanErrors.Count -gt 0) {
        Write-Host '扫描未完成，已停止本次清理：' -ForegroundColor Red
        foreach ($scanError in $scanErrors) { Write-Host $scanError }
        return 1
    }
    if ($projects.Count -eq 0) {
        Write-Host '未找到 .uvprojx 或 .uvproj 工程文件，请核对目录。' -ForegroundColor Red
        return 2
    }

    $projectDirectories = New-PathSet
    $outputDirectories = New-PathSet
    $protectedInputs = New-PathSet
    $unresolvedInputNames = New-PathSet
    $protectedExtensions = New-PathSet
    $projectTexts = [Collections.Generic.List[string]]::new()
    $skippedOutputPaths = 0
    foreach ($project in $projects) {
        [void]$projectDirectories.Add($project.DirectoryName)
        $reader = $null
        try {
            $settings = [Xml.XmlReaderSettings]::new()
            $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
            $settings.XmlResolver = $null
            $reader = [Xml.XmlReader]::Create($project.FullName, $settings)
            $document = [Xml.XmlDocument]::new()
            $document.XmlResolver = $null
            $document.Load($reader)
            if ($document.DocumentElement.LocalName -ne 'Project') {
                throw '不是有效的 Keil Project XML。'
            }
            $projectTexts.Add($document.DocumentElement.InnerText)
            $outputNodes = $document.SelectNodes(
                '//*[local-name()="TargetCommonOption"]/*[local-name()="OutputDirectory" or local-name()="ListingPath"]')
            foreach ($node in $outputNodes) {
                if (-not $node.InnerText.Trim()) { continue }
                $outputPath = Resolve-ProjectPath $node.InnerText $project.DirectoryName
                if ($outputPath -and (Test-InRoot $outputPath $rootPath)) {
                    [void]$outputDirectories.Add($outputPath)
                } else { $skippedOutputPaths++ }
            }
            foreach ($node in $document.SelectNodes('//*[local-name()="FilePath"]')) {
                $inputPath = Resolve-ProjectPath $node.InnerText $project.DirectoryName
                if ($inputPath) { [void]$protectedInputs.Add($inputPath) }
                else {
                    # Macro-based precompiled inputs are conservatively protected by name.
                    $inputName = [IO.Path]::GetFileName($node.InnerText.Trim().Trim('"'))
                    $inputExtension = [IO.Path]::GetExtension($inputName)
                    if ($inputName -match '[$%*?]') {
                        if ($extensions.Contains($inputExtension)) {
                            [void]$protectedExtensions.Add($inputExtension)
                        } elseif (-not $inputExtension -or $inputExtension -match '[$%*?]') {
                            foreach ($extension in $extensions) {
                                [void]$protectedExtensions.Add($extension)
                            }
                        }
                    } elseif ($inputName -and $extensions.Contains($inputExtension)) {
                        [void]$unresolvedInputNames.Add($inputName)
                    }
                }
            }
        } catch {
            Write-Host ('无法读取工程配置，已停止清理：' + $project.FullName + ': ' +
                $_.Exception.Message) -ForegroundColor Red
            return 1
        } finally {
            if ($reader) { $reader.Dispose() }
        }
    }

    $candidates = [Collections.Generic.List[IO.FileInfo]]::new()
    $protectedCount = 0
    foreach ($file in $intermediates) {
        $isInput = $protectedInputs.Contains($file.FullName) -or
            $unresolvedInputNames.Contains($file.Name) -or $protectedExtensions.Contains($file.Extension)
        if (-not $isInput -and $file.Extension -in @('.o', '.obj')) {
            # Objects can also be supplied through linker controls outside FilePath.
            # A textual match may keep an extra output, which is preferable to deleting an input.
            foreach ($projectText in $projectTexts) {
                if ($projectText.IndexOf($file.Name, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    $isInput = $true
                    break
                }
            }
        }
        if ($isInput) {
            $protectedCount++
            continue
        }
        $eligible = $projectDirectories.Contains($file.DirectoryName)
        $relativePath = $file.FullName.Substring($rootPath.Length + 1)
        $parts = $relativePath -split '[\\/]'
        for ($index = 0; -not $eligible -and $index -lt ($parts.Length - 1); $index++) {
            $eligible = $buildNames.Contains($parts[$index])
        }
        if (-not $eligible) {
            foreach ($outputDirectory in $outputDirectories) {
                if (Test-InRoot $file.FullName $outputDirectory) { $eligible = $true; break }
            }
        }
        if ($eligible) { $candidates.Add($file) }
    }

    [long]$candidateBytes = 0
    foreach ($file in $candidates) { $candidateBytes += $file.Length }
    Write-Host ('Keil 工程：{0}；跳过的目录/链接：{1}；受保护输入：{2}' -f
        $projects.Count, $skippedDirectories, $protectedCount)
    if ($protectedExtensions.Count -gt 0) {
        Write-Host ('输入文件名包含未解析宏/通配符，保守保留这些类型：' +
            (($protectedExtensions | Sort-Object) -join '、'))
    }
    if ($skippedOutputPaths -gt 0) {
        Write-Host ('已跳过根目录之外或包含未解析宏的输出配置：{0}' -f $skippedOutputPaths)
    }
    Write-Host ('可清理：{0} 个文件，{1}（{2} 字节）' -f
        $candidates.Count, (Format-ByteCount $candidateBytes), $candidateBytes)
    if ($candidates.Count -gt 0) {
        $table = $candidates | Group-Object Extension | Sort-Object Name | ForEach-Object {
            [long]$groupBytes = 0
            foreach ($file in $_.Group) { $groupBytes += $file.Length }
            [pscustomobject]@{Extension=$_.Name; Files=$_.Count; Size=(Format-ByteCount $groupBytes)}
        }
        Write-Host ($table | Format-Table -AutoSize | Out-String)
    }
    if ($Preview) {
        $shown = 0
        foreach ($file in ($candidates | Sort-Object FullName)) {
            if ($Details -or $shown -lt 15) {
                Write-Host ('  ' + $file.FullName.Substring($rootPath.Length + 1))
                $shown++
            }
        }
        if (-not $Details -and $candidates.Count -gt $shown) {
            Write-Host ('另有 {0} 个文件，可加 -Details 查看完整清单。' -f ($candidates.Count - $shown))
        }
        Write-Host '预览完成。文件保持原样。'
        return 0
    }

    $deletedCount = 0
    [long]$deletedBytes = 0
    $failureCount = 0
    foreach ($file in $candidates) {
        try {
            $current = Get-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
            if ($current -isnot [IO.FileInfo] -or
                    -not (Test-InRoot $current.FullName $rootPath) -or
                    (Test-LinkInPath $current.FullName) -or
                    -not $extensions.Contains($current.Extension)) {
                throw '路径或文件类型已变化。'
            }
            if ($current.Length -ne $file.Length -or
                    $current.LastWriteTimeUtc.Ticks -ne $file.LastWriteTimeUtc.Ticks) {
                throw '扫描后文件发生变化，本次保留。'
            }
            # Delete exactly this file; never remove directories or use wildcard paths.
            Remove-Item -LiteralPath $current.FullName -ErrorAction Stop
            $deletedCount++
            $deletedBytes += $file.Length
            Write-Host ('已删除：' + $file.FullName)
        } catch {
            $failureCount++
            Write-Host ('清理失败：' + $file.FullName + ': ' + $_.Exception.Message) -ForegroundColor Yellow
        }
    }
    Write-Host ('清理完成：删除 {0} 个文件，释放文件大小合计 {1}（{2} 字节），失败 {3} 个。' -f
        $deletedCount, (Format-ByteCount $deletedBytes), $deletedBytes, $failureCount)
    if ($failureCount -gt 0) { return 1 }
    return 0
}

$runStartedAt = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8))
$runTimer = [Diagnostics.Stopwatch]::StartNew()
$runLabel = if ($Preview) { '预览' } else { '清理' }
Write-Host ('{0}开始时间：{1}' -f $runLabel, $runStartedAt.ToString('yyyy-MM-dd HH:mm:ss zzz'))
$result = 1
try {
    $result = Invoke-KeilCleanup
} catch {
    Write-Host ('清理中止：' + $_.Exception.Message) -ForegroundColor Red
    $result = 1
} finally {
    $runTimer.Stop()
    $runEndedAt = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8))
    Write-Host ('{0}结束时间：{1}' -f $runLabel, $runEndedAt.ToString('yyyy-MM-dd HH:mm:ss zzz'))
    Write-Host ('{0}总耗时：{1:F3} 秒' -f $runLabel, $runTimer.Elapsed.TotalSeconds)
}
exit $result
