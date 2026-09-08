[CmdletBinding()]
param(
    [string[]] $RootPath,
    [ValidateRange(1, [long]::MaxValue)]
    [long] $MinimumDuplicateBytes = 65536,
    [ValidateRange(1, 1000)]
    [int] $MaximumFilesInReport = 100,
    [switch] $IncludeHidden
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))

if (-not $RootPath -or $RootPath.Count -eq 0) {
    $RootPath = @("release", "dist", "build", "assets", "node_modules\ffmpeg-static")
}

$resolvedRoots = New-Object System.Collections.Generic.List[string]
$missingRoots = New-Object System.Collections.Generic.List[string]
foreach ($candidate in $RootPath) {
    $absolute = if ([IO.Path]::IsPathRooted($candidate)) {
        [IO.Path]::GetFullPath($candidate)
    } else {
        [IO.Path]::GetFullPath((Join-Path $repositoryRoot $candidate))
    }
    if (Test-Path -LiteralPath $absolute) {
        if (-not $resolvedRoots.Contains($absolute)) { $resolvedRoots.Add($absolute) }
    } else {
        $missingRoots.Add($absolute)
    }
}

$files = New-Object System.Collections.Generic.List[object]
$scanErrors = New-Object System.Collections.Generic.List[object]
foreach ($root in $resolvedRoots) {
    try {
        $rootItem = Get-Item -LiteralPath $root -Force
        $items = if ($rootItem.PSIsContainer) {
            Get-ChildItem -LiteralPath $root -File -Recurse -Force:$IncludeHidden -ErrorAction Stop
        } else {
            @($rootItem)
        }
        foreach ($item in $items) {
            $relative = if ($rootItem.PSIsContainer) {
                $separator = [IO.Path]::DirectorySeparatorChar
                $item.FullName.Substring($root.TrimEnd($separator).Length).TrimStart($separator)
            } else {
                $item.Name
            }
            $files.Add([pscustomobject][ordered]@{
                root = $root
                path = $item.FullName
                relativePath = $relative
                name = $item.Name
                extension = $item.Extension.ToLowerInvariant()
                bytes = [long]$item.Length
            })
        }
    } catch {
        $scanErrors.Add([pscustomobject]@{ path = $root; error = $_.Exception.Message })
    }
}

$hashErrors = New-Object System.Collections.Generic.List[object]
$duplicateGroups = New-Object System.Collections.Generic.List[object]
$sameSizeGroups = $files | Where-Object { $_.bytes -ge $MinimumDuplicateBytes } | Group-Object bytes | Where-Object Count -gt 1
foreach ($sizeGroup in $sameSizeGroups) {
    $hashed = foreach ($file in $sizeGroup.Group) {
        try {
            [pscustomobject]@{ file = $file; hash = (Get-FileHash -LiteralPath $file.path -Algorithm SHA256).Hash }
        } catch {
            $hashErrors.Add([pscustomobject]@{ path = $file.path; error = $_.Exception.Message })
        }
    }
    foreach ($hashGroup in ($hashed | Where-Object { $_ } | Group-Object hash | Where-Object Count -gt 1)) {
        $size = [long]$hashGroup.Group[0].file.bytes
        $duplicateGroups.Add([pscustomobject][ordered]@{
            sha256 = $hashGroup.Name
            bytesEach = $size
            copies = $hashGroup.Count
            potentialDuplicateBytes = $size * ($hashGroup.Count - 1)
            paths = @($hashGroup.Group.file.path | Sort-Object)
        })
    }
}

$byRoot = foreach ($group in ($files | Group-Object root)) {
    [pscustomobject]@{
        root = $group.Name
        files = $group.Count
        bytes = [long](($group.Group | Measure-Object bytes -Sum).Sum)
    }
}

$binaryOccurrences = $files | Where-Object {
    $_.name -ieq "clipture_engine.exe" -or $_.name -ieq "ffmpeg.exe"
} | Select-Object name, bytes, path

$report = [ordered]@{
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    readOnly = $true
    repositoryRoot = $repositoryRoot
    roots = @($resolvedRoots.ToArray())
    missingRoots = @($missingRoots.ToArray())
    minimumDuplicateBytes = $MinimumDuplicateBytes
    totals = [ordered]@{
        files = $files.Count
        bytes = [long](($files | ForEach-Object { $_.bytes } | Measure-Object -Sum).Sum)
        exactDuplicateGroups = $duplicateGroups.Count
        potentialDuplicateBytes = [long](($duplicateGroups | ForEach-Object { $_.potentialDuplicateBytes } | Measure-Object -Sum).Sum)
    }
    byRoot = @($byRoot | Sort-Object root)
    binaryOccurrences = @($binaryOccurrences | Sort-Object name, path)
    largestFiles = @($files | Sort-Object bytes -Descending | Select-Object -First $MaximumFilesInReport)
    exactDuplicates = @($duplicateGroups.ToArray() | Sort-Object potentialDuplicateBytes -Descending)
    errors = @($scanErrors.ToArray()) + @($hashErrors.ToArray())
}

$report | ConvertTo-Json -Depth 8
