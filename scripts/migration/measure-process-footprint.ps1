[CmdletBinding()]
param(
    [int[]] $RootProcessId,
    [string[]] $RootProcessName = @("Clipture"),
    [ValidateRange(1, 1000)]
    [int] $SampleCount = 5,
    [ValidateRange(0, 60000)]
    [int] $IntervalMilliseconds = 1000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-ProcessCategory([string] $Name, [int] $Id, [int[]] $RootIds) {
    if ($RootIds -contains $Id) { return "controller" }
    switch -Regex ($Name) {
        "^clipture_engine(\.exe)?$" { return "engine" }
        "^msedgewebview2(\.exe)?$" { return "webview2" }
        "^electron(\.exe)?$" { return "electron" }
        "^clipture(\.exe)?$" { return "electron" }
        "^node(\.exe)?$" { return "node" }
        "^ffmpeg(\.exe)?$" { return "ffmpeg" }
        default { return "other" }
    }
}

function Get-ProcessTreeSnapshot {
    try {
        $native = @(Get-CimInstance -ClassName Win32_Process -Property ProcessId, ParentProcessId, Name -ErrorAction Stop)
    } catch {
        throw "Win32 process-tree enumeration failed: $($_.Exception.Message)"
    }

    $requestedNames = @($RootProcessName | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_).ToLowerInvariant() })
    $roots = if ($RootProcessId -and @($RootProcessId).Count -gt 0) {
        @($native | Where-Object { $RootProcessId -contains [int]$_.ProcessId })
    } else {
        @($native | Where-Object { $requestedNames -contains [IO.Path]::GetFileNameWithoutExtension($_.Name).ToLowerInvariant() })
    }
    if (@($roots).Count -eq 0) {
        $selector = if ($RootProcessId) { "PID: $($RootProcessId -join ', ')" } else { "name: $($RootProcessName -join ', ')" }
        throw "No root process matched $selector. Start the build first; prefer -RootProcessId for development builds."
    }

    $ids = New-Object "System.Collections.Generic.HashSet[int]"
    $queue = New-Object "System.Collections.Generic.Queue[int]"
    foreach ($root in $roots) {
        if ($ids.Add([int]$root.ProcessId)) { $queue.Enqueue([int]$root.ProcessId) }
    }
    while ($queue.Count -gt 0) {
        $parentId = $queue.Dequeue()
        foreach ($child in ($native | Where-Object { [int]$_.ParentProcessId -eq $parentId })) {
            if ($ids.Add([int]$child.ProcessId)) { $queue.Enqueue([int]$child.ProcessId) }
        }
    }

    $rootIds = @($roots | ForEach-Object { [int]$_.ProcessId })
    $rows = foreach ($entry in ($native | Where-Object { $ids.Contains([int]$_.ProcessId) } | Sort-Object ProcessId)) {
        $process = Get-Process -Id ([int]$entry.ProcessId) -ErrorAction SilentlyContinue
        if (-not $process) { continue }
        $path = $null
        $startedAtUtc = $null
        try { $path = $process.Path } catch { }
        try { $startedAtUtc = $process.StartTime.ToUniversalTime().ToString("o") } catch { }
        [pscustomobject][ordered]@{
            processId = [int]$entry.ProcessId
            parentProcessId = [int]$entry.ParentProcessId
            name = [string]$entry.Name
            category = Get-ProcessCategory ([string]$entry.Name) ([int]$entry.ProcessId) $rootIds
            privateBytes = [long]$process.PrivateMemorySize64
            workingSetBytes = [long]$process.WorkingSet64
            pagedBytes = [long]$process.PagedMemorySize64
            handles = [int]$process.HandleCount
            threads = [int]@($process.Threads).Count
            cpuSeconds = if ($null -eq $process.CPU) { $null } else { [double]$process.CPU }
            startedAtUtc = $startedAtUtc
            executablePath = $path
        }
    }

    $byCategory = foreach ($group in ($rows | Group-Object category)) {
        [pscustomobject][ordered]@{
            category = $group.Name
            processes = $group.Count
            privateBytes = [long](($group.Group | Measure-Object privateBytes -Sum).Sum)
            workingSetBytes = [long](($group.Group | Measure-Object workingSetBytes -Sum).Sum)
        }
    }
    [pscustomobject][ordered]@{
        capturedAtUtc = [DateTime]::UtcNow.ToString("o")
        rootProcessIds = $rootIds
        totals = [ordered]@{
            processes = @($rows).Count
            privateBytes = [long](($rows | ForEach-Object { $_.privateBytes } | Measure-Object -Sum).Sum)
            workingSetBytes = [long](($rows | ForEach-Object { $_.workingSetBytes } | Measure-Object -Sum).Sum)
            handles = [long](($rows | ForEach-Object { $_.handles } | Measure-Object -Sum).Sum)
            threads = [long](($rows | ForEach-Object { $_.threads } | Measure-Object -Sum).Sum)
        }
        byCategory = @($byCategory | Sort-Object category)
        processes = @($rows)
    }
}

$samples = New-Object System.Collections.Generic.List[object]
for ($index = 0; $index -lt $SampleCount; $index += 1) {
    $samples.Add((Get-ProcessTreeSnapshot))
    if ($index + 1 -lt $SampleCount -and $IntervalMilliseconds -gt 0) {
        Start-Sleep -Milliseconds $IntervalMilliseconds
    }
}

$selector = if ($RootProcessId) {
    [ordered]@{ processIds = [int[]]$RootProcessId }
} else {
    [ordered]@{ processNames = [string[]]$RootProcessName }
}

[ordered]@{
    schemaVersion = 1
    readOnly = $true
    note = "Private bytes are the primary comparison; summed working sets may double-count shared pages."
    selector = $selector
    sampleCount = $SampleCount
    intervalMilliseconds = $IntervalMilliseconds
    samples = @($samples.ToArray())
} | ConvertTo-Json -Depth 8
