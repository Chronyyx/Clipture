param(
    [Parameter(Mandatory = $true)][int]$AppProcessId,
    [Parameter(Mandatory = $true)][string]$Profile
)
$ErrorActionPreference = 'Stop'
Add-Type -Path @((Join-Path $PSScriptRoot 'ProcessHandleTypes.cs'), (Join-Path $PSScriptRoot 'ProcessHandleTracing.cs'))
if ($env:CLIPTURE_SMOKE_TRACE_HANDLES -eq '1') { [ProcessHandleTypes]::EnableTrace($AppProcessId) }
$knownChildren = @{}
$lastRequest = ''
$rootStartedAt = (Get-Process -Id $AppProcessId).StartTime
function Test-TestAppRunning {
    $live = Get-Process -Id $AppProcessId -ErrorAction SilentlyContinue
    if ($null -eq $live) { return $false }
    try { return $live.StartTime -eq $rootStartedAt } finally { $live.Dispose() }
}
function Measure-AppTree {
    $processes = @(Get-CimInstance Win32_Process)
    $included = @{}
    $root = $processes | Where-Object { $_.ProcessId -eq $AppProcessId } | Select-Object -First 1
    if ($null -eq $root) { throw 'The test application exited before the resource sample.' }
    $included[$AppProcessId] = $root.CreationDate
    foreach ($process in $processes) {
        $identity = "$($process.ProcessId):$($process.CreationDate.Ticks)"
        if ($knownChildren.ContainsKey($identity)) { $included[[int]$process.ProcessId] = $process.CreationDate }
    }
    do {
        $changed = $false
        foreach ($process in $processes) {
            # Windows keeps the numeric parent PID after that parent exits.
            # Do not adopt an unrelated older process when that PID is reused.
            if ($included.ContainsKey([int]$process.ParentProcessId) -and
                $process.CreationDate -ge $included[[int]$process.ParentProcessId] -and
                !$included.ContainsKey([int]$process.ProcessId)) {
                $included[[int]$process.ProcessId] = $process.CreationDate
                $knownChildren["$($process.ProcessId):$($process.CreationDate.Ticks)"] = $true
                $changed = $true
            }
        }
    } while ($changed)
    $rows = @(foreach ($process in $processes) {
        if (!$included.ContainsKey([int]$process.ProcessId)) { continue }
        $live = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
        if ($null -ne $live) {
            [ordered]@{ pid = [int]$process.ProcessId; name = $process.Name;
                role = ([regex]::Match($process.CommandLine, '--utility-sub-type=([^ ]+)').Groups[1].Value);
                workingSetBytes = $live.WorkingSet64; privateBytes = $live.PrivateMemorySize64;
                handles = $live.HandleCount }
        }
    })
    return ,$rows
}
while (Test-TestAppRunning) {
    $requestPath = Join-Path $Profile 'resource-request.json'
    if (!(Test-Path -LiteralPath $requestPath)) { Start-Sleep -Milliseconds 100; continue }
    try { $request = Get-Content -Raw -LiteralPath $requestPath | ConvertFrom-Json } catch { Start-Sleep -Milliseconds 100; continue }
    if ($request.key -eq $lastRequest) { Start-Sleep -Milliseconds 100; continue }
    $lastRequest = $request.key
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $rows = Measure-AppTree
        $webviews = @($rows | Where-Object { $_.name -ieq 'msedgewebview2.exe' })
        if ($request.phase -ne 'closed' -or $webviews.Count -eq 0) { break }
        Start-Sleep -Milliseconds 250
    } while ($watch.Elapsed.TotalSeconds -lt 8)
    $result = [ordered]@{ ok = ($request.phase -ne 'closed' -or $webviews.Count -eq 0);
        processes = @($rows); webviewProcesses = $webviews.Count; elapsedMs = $watch.ElapsedMilliseconds;
        controllerHandleTypes = [ProcessHandleTypes]::Sample($AppProcessId) }
    if ([ProcessHandleTypes]::TraceEnabled) { $result.handleTraces = @([ProcessHandleTypes]::LastProcessTraces) }
    # Publish complete JSON atomically so the app never reads a partial sample.
    $target = Join-Path $Profile "resources-$($request.key).json"
    $result | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath "$target.tmp" -Encoding UTF8
    Move-Item -LiteralPath "$target.tmp" -Destination $target
}

# Do not stop at controller exit: verify previously observed children also exit.
# Process identity includes creation time, so reused PIDs are never adopted.
$exitWatch = [Diagnostics.Stopwatch]::StartNew()
do {
    $survivors = @(Get-CimInstance Win32_Process | Where-Object {
        $knownChildren.ContainsKey("$($_.ProcessId):$($_.CreationDate.Ticks)")
    } | ForEach-Object { [ordered]@{pid=[int]$_.ProcessId;name=$_.Name} })
    if ($survivors.Count -eq 0) { break }
    Start-Sleep -Milliseconds 250
} while ($exitWatch.Elapsed.TotalSeconds -lt 5)
[ordered]@{ok=($survivors.Count -eq 0);survivors=$survivors;elapsedMs=$exitWatch.ElapsedMilliseconds} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Profile 'app-exit-resources.json') -Encoding UTF8
