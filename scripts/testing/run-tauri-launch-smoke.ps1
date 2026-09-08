# Mutating, opt-in launch test. Uses only a fresh workspace profile; no capture.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$executable = Join-Path $root 'release/tauri-unpacked/clipture.exe'
$legacy = Join-Path $env:LOCALAPPDATA 'Programs/Clipture/Clipture.exe'
$existing = @(Get-CimInstance Win32_Process -Filter "Name='clipture.exe'")
foreach ($item in $existing) {
    # Only the known Electron install can coexist; do not forward to another Tauri profile.
    if ($item.ExecutablePath -ne $legacy -or -not (Test-Path -LiteralPath (Join-Path (Split-Path $legacy) 'resources/app.asar'))) {
        throw "Another Clipture instance must be closed before this isolated test: $($item.ProcessId)"
    }
}
$profile = Join-Path $root ('.cache/tauri-launch-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
$oldData = $env:CLIPTURE_DATA_DIR
$oldMode = $env:CLIPTURE_TEST_MODE
$env:CLIPTURE_DATA_DIR = $profile
$env:CLIPTURE_TEST_MODE = '1'
$controller = $null
$checks = [Collections.Generic.List[string]]::new()
$owned = @()
$result = [ordered]@{ ok = $false; profile = $profile; checks = $checks; survivors = @() }
function Check([bool] $condition, [string] $message) {
    if (-not $condition) { throw $message }
    $checks.Add($message)
}
function OwnedTree([int] $rootId) {
    $all = @(Get-CimInstance Win32_Process)
    $ids = [Collections.Generic.HashSet[int]]::new()
    [void]$ids.Add($rootId)
    do {
        $changed = $false
        foreach ($item in $all) {
            if ($ids.Contains([int]$item.ParentProcessId) -and $ids.Add([int]$item.ProcessId)) { $changed = $true }
        }
    } while ($changed)
    @($all | Where-Object { $ids.Contains([int]$_.ProcessId) })
}
try {
    $launchOutput = & node (Join-Path $root 'scripts/run-tauri.cjs') --hidden
    Check ($LASTEXITCODE -eq 0) 'Node launcher exits successfully after hidden launch'
    $match = [regex]::Match(($launchOutput -join "`n"), 'PID (\d+)')
    Check $match.Success 'Launcher reports the exact owned controller PID'
    $controller = Get-Process -Id ([int]$match.Groups[1].Value)
    $null = $controller.Handle
    Check ($controller.Path -eq $executable) 'Launched the optimized unpacked Tauri controller'
    Start-Sleep -Seconds 3
    Check (-not $controller.HasExited) 'Controller remains alive after Node exits'
    $owned = @(OwnedTree $controller.Id)
    Check ($owned.Count -eq 1) 'Hidden test-mode startup has no engine, WebView, FFmpeg or Node descendants'
    $result.hiddenPrivateBytes = $controller.PrivateMemorySize64
    $second = & node (Join-Path $root 'scripts/run-tauri.cjs')
    Check ($LASTEXITCODE -eq 0) 'Second normal launch exits its launcher'
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 250
        $owned = @(OwnedTree $controller.Id)
        $workers = @($owned | Where-Object { $_.ExecutablePath -eq $executable -and $_.CommandLine -match '--ui-worker' })
        $webviews = @($owned | Where-Object { $_.Name -eq 'msedgewebview2.exe' })
    } while (($workers.Count -ne 1 -or $webviews.Count -eq 0) -and [DateTime]::UtcNow -lt $deadline)
    Check ($workers.Count -eq 1) 'Normal launch opens exactly one disposable UI worker'
    Check ($webviews.Count -gt 0) 'Optimized UI worker starts WebView2'
    $roots = @(Get-CimInstance Win32_Process -Filter "Name='clipture.exe'" | Where-Object { $_.ExecutablePath -eq $executable -and $_.CommandLine -notmatch '--ui-worker' })
    Check ($roots.Count -eq 1 -and $roots[0].ProcessId -eq $controller.Id) 'Second launch reuses the original resident controller'
    Check (@($owned | Where-Object { $_.Name -in @('node.exe', 'clipture_engine.exe', 'ffmpeg.exe') }).Count -eq 0) 'Isolated UI test never starts capture or keeps a Node launcher'
    $result.ok = $true
} catch {
    $result.error = $_.Exception.Message
} finally {
    if ($null -ne $controller) {
        if (-not $controller.HasExited) {
            $owned = @(OwnedTree $controller.Id)
            $controller.Kill() # Cached owned Process handle, never a name-based kill.
            [void]$controller.WaitForExit(10000)
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(12)
        do {
            Start-Sleep -Milliseconds 250
            $remaining = @(Get-CimInstance Win32_Process | Where-Object {
                $candidate = $_
                @($owned | Where-Object { $_.ProcessId -eq $candidate.ProcessId -and $_.CreationDate -eq $candidate.CreationDate }).Count -gt 0
            })
        } while ($remaining.Count -gt 0 -and [DateTime]::UtcNow -lt $deadline)
        $result.survivors = @($remaining | Select-Object ProcessId, ParentProcessId, Name)
        if ($remaining.Count -gt 0) { $result.ok = $false } else { $checks.Add('Owned controller and complete UI process tree exit without orphans') }
        $controller.Dispose()
    }
    $env:CLIPTURE_DATA_DIR = $oldData
    $env:CLIPTURE_TEST_MODE = $oldMode
    $json = $result | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText((Join-Path $profile 'launch-result.json'), $json)
    Write-Output $json
}
if (-not $result.ok) { exit 1 }
