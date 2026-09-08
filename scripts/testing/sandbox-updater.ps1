# Mutating updater integration. Refuses host execution and uses only guest fixtures.
param([Parameter(Mandatory=$true)][string]$Token)
$ErrorActionPreference = 'Stop'
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or
    (Get-Content -Raw -LiteralPath 'C:\CliptureInput\probe-token.txt').Trim() -ne $Token) {
    throw 'Refusing updater installation outside the identified Sandbox.'
}
$target = 'C:\CliptureInstallerTest\Update Candidate'
$report = [ordered]@{ok=$false;token=$Token;checks=@();manifestFixtureVersion='1.4.3';installerFixtureVersion='1.4.2'}
$output = 'C:\CliptureOutput\updater-install.json'
$server = $null
$client = $null
function Publish { $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $output -Encoding UTF8 }
function Check([bool]$condition, [string]$message) {
    if (!$condition) { throw $message }
    $report.checks += $message
    Publish
}
function Install([string]$file, [string]$arguments) {
    $process = Start-Process -FilePath $file -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $null = $process.Handle
    if (!$process.WaitForExit(120000)) { throw "Installer timed out: $file" }
    if ($process.ExitCode -ne 0) { throw "Installer failed ($($process.ExitCode)): $file" }
}
function Check-Data {
    foreach ($file in (Get-Content -Raw -LiteralPath 'C:\CliptureOutput\data-baseline.json' | ConvertFrom-Json)) {
        Check ((Test-Path -LiteralPath $file.path) -and (Get-FileHash -LiteralPath $file.path).Hash -eq $file.hash) "Preserved $($file.path)"
    }
}
Publish
try {
    Check (!(Test-Path -LiteralPath "$target\clipture.exe")) 'Updater fixture target is initially absent'
    Install 'C:\CliptureInput\Tauri.exe' "/S /currentuser /D=$target"
    Check (Test-Path -LiteralPath "$target\clipture.exe") 'Installed isolated Tauri baseline'
    $expectedHash = (Get-FileHash -LiteralPath 'C:\CliptureInput\expected-controller.exe').Hash
    Check ((Get-FileHash -LiteralPath "$target\clipture.exe").Hash -eq $expectedHash) 'Baseline matches the rebuilt candidate'
    # Replace only this test-installed executable with the guarded debug role.
    # No real user file, host installation, registry, or trust store is changed.
    Copy-Item -LiteralPath 'C:\CliptureInput\updater-harness.exe' -Destination "$target\clipture.exe" -Force
    Check ((Get-FileHash -LiteralPath "$target\clipture.exe").Hash -ne $expectedHash) 'Harness replacement makes actual updater publication observable'
    $server = Start-Process -FilePath 'C:\CliptureInput\node.exe' -ArgumentList "C:\CliptureInput\updater-server.cjs $Token" -WindowStyle Hidden -PassThru
    $null = $server.Handle
    $ready = $false
    for ($attempt=0; $attempt -lt 100; $attempt++) {
        if (Test-Path -LiteralPath 'C:\CliptureOutput\updater-server-ready.json') {
            $origin = Get-Content -Raw -LiteralPath 'C:\CliptureOutput\updater-server-ready.json' | ConvertFrom-Json
            $ready = $origin.token -eq $Token -and $origin.pid -eq $server.Id
            if ($ready) { break }
        }
        if ($server.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }
    Check $ready 'Private localhost HTTPS fixture server is ready'
    $env:CLIPTURE_UPDATER_SMOKE_TOKEN = $Token
    $client = Start-Process -FilePath "$target\clipture.exe" -ArgumentList '--updater-smoke' -WindowStyle Hidden -PassThru -RedirectStandardError 'C:\CliptureOutput\updater-stderr.log'
    $null = $client.Handle
    Check ($client.WaitForExit(185000)) 'Real updater client exits within its bounded timeout'
    $native = Get-Content -Raw -LiteralPath 'C:\CliptureOutput\updater-native.json' | ConvertFrom-Json
    $report.native = $native
    Check ($client.ExitCode -eq 0 -and !$native.error) 'Signed updater client handed off successfully'
    Check ($native.checks -contains 'signed HTTPS installer streamed, authenticated, and staged') 'Production streaming and signature verification ran over HTTPS'
    Check ($native.checks -contains 'native updater reached installer handoff hook') 'Native updater extracted and launched its installer'
    $published = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    do {
        if (Test-Path -LiteralPath "$target\clipture.exe") {
            try { $published = (Get-FileHash -LiteralPath "$target\clipture.exe").Hash -eq $expectedHash } catch { }
        }
        if ($published) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    Check $published 'Signed updater installed exactly the candidate controller bytes'
    Start-Sleep -Seconds 5
    $roots = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -eq "$target\clipture.exe" -and $_.CommandLine -notmatch ' --ui-worker(?:\s|$)'
    })
    Check ($roots.Count -eq 1) 'Updater relaunches one installed controller'
    $rootPid = [int]$roots[0].ProcessId
    $tree = @(Get-CimInstance Win32_Process)
    $ids = [Collections.Generic.HashSet[int]]::new()
    $null = $ids.Add($rootPid)
    do {
        $added = $false
        foreach ($entry in $tree) {
            if ($ids.Contains([int]$entry.ParentProcessId) -and $ids.Add([int]$entry.ProcessId)) { $added=$true }
        }
    } while ($added)
    $children = @($tree | Where-Object { $_.ProcessId -ne $rootPid -and $ids.Contains([int]$_.ProcessId) })
    Check (@($children | Where-Object { $_.ExecutablePath -eq "$target\clipture.exe" -and $_.CommandLine -match ' --ui-worker(?:\s|$)' }).Count -eq 1) 'Updated app creates one disposable UI worker'
    Check-Data
    $live = Get-Process -Id $rootPid
    $null = $live.Handle
    Check ($live.Path -eq "$target\clipture.exe") 'Teardown still targets the owned updated controller'
    $live.Kill()
    $live.WaitForExit(5000) | Out-Null
    Start-Sleep -Seconds 5
    $survivors = @(foreach ($child in $children) {
        Get-CimInstance Win32_Process -Filter "ProcessId=$($child.ProcessId)" |
            Where-Object { $_.CreationDate -eq $child.CreationDate }
    })
    $report.survivors = @($survivors | Select-Object ProcessId,Name,ExecutablePath)
    Check ($survivors.Count -eq 0) 'Updated app leaves no owned child or grandchild processes'
    Install "$target\uninstall.exe" '/S /currentuser'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((Test-Path -LiteralPath "$target\clipture.exe") -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    Check (!(Test-Path -LiteralPath "$target\clipture.exe")) 'Updated candidate uninstalls'
    Check-Data
    $report.ok = $true
} catch {
    $report.error = $_.Exception.Message
} finally {
    if ($client -and !$client.HasExited) { $client.Kill(); $client.WaitForExit(5000) | Out-Null }
    if ($server -and !$server.HasExited) { $server.Kill(); $server.WaitForExit(5000) | Out-Null }
    Publish
}
if (!$report.ok) { exit 1 }
