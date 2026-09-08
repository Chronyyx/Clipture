# Mutating integration harness: ONLY the disposable Sandbox test account.
param(
    [Parameter(Mandatory=$true)][ValidateSet('runtime','clean','legacy','crossgrade','rollback','blocked_crossgrade','inspect','cleanup_candidate','cleanup_relaunch')][string]$Phase,
    [Parameter(Mandatory=$true)][string]$Token,
    [ValidateSet('currentuser','allusers')][string]$Scope = 'currentuser',
    [switch]$ForceRun
)
$ErrorActionPreference = 'Stop'
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or
    (Get-Content -Raw -LiteralPath 'C:\CliptureInput\probe-token.txt').Trim() -ne $Token) {
    throw 'Refusing installer tests outside the explicitly identified test Sandbox.'
}
$report = [ordered]@{ok=$false;phase=$Phase;scope=$Scope;forceRun=[bool]$ForceRun;checks=@();token=$Token}
$suffix = if ($Scope -eq 'allusers') { '-allusers' } else { '' }
if ($ForceRun) { $suffix += '-force-run' }
$output = "C:\CliptureOutput\installer-$Phase$suffix.json"
$registryHive = if ($Scope -eq 'allusers') { 'HKLM:' } else { 'HKCU:' }
function Publish { $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $output -Encoding UTF8 }
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
function Wait-Removed([string]$file) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((Test-Path -LiteralPath $file) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    return !(Test-Path -LiteralPath $file)
}
$data = Join-Path $env:APPDATA 'Clipture\data'
$testRoot = 'C:\CliptureInstallerTest'
$legacy = Join-Path $testRoot 'Custom Electron Path'
$clean = Join-Path $testRoot 'Clean Tauri'
function Seed-Data {
    New-Item -ItemType Directory -Path "$data\sounds", "$testRoot\User Clips" -Force | Out-Null
    $settingsJson = [ordered]@{saveFolder="$testRoot\User Clips";startOnLogin=$false;clipSound='none';
        hotkey='Ctrl+Alt+Shift+F24';fps=30;showNotification=$false;
        migrationSentinel='preserve-unknown-legacy-field';
        audioSources=@(@{id='system';kind='system';enabled=$false},
            @{id='mic';kind='microphone';enabled=$false},@{id='game';kind='game';enabled=$false})} |
        ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText("$data\settings.json", $settingsJson, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText("$data\clips.json", '[]', [Text.UTF8Encoding]::new($false))
    'user-owned sound sentinel' | Set-Content -LiteralPath "$data\sounds\preserve.wav"
    'user-owned external clip sentinel' | Set-Content -LiteralPath "$testRoot\User Clips\preserve.mp4"
    $files = @("$data\settings.json", "$data\clips.json", "$data\sounds\preserve.wav", "$testRoot\User Clips\preserve.mp4")
    @($files | ForEach-Object { @{path=$_;hash=(Get-FileHash -LiteralPath $_).Hash} }) |
        ConvertTo-Json | Set-Content -LiteralPath 'C:\CliptureOutput\data-baseline.json' -Encoding UTF8
}
function Check-Data {
    foreach ($file in (Get-Content -Raw -LiteralPath 'C:\CliptureOutput\data-baseline.json' | ConvertFrom-Json)) {
        Check ((Test-Path -LiteralPath $file.path) -and (Get-FileHash -LiteralPath $file.path).Hash -eq $file.hash) "Preserved $($file.path)"
    }
}
function Check-Package([string]$directory) {
    Check (Test-Path -LiteralPath "$directory\clipture.exe") 'Tauri controller installed'
    $files = @(Get-ChildItem -LiteralPath $directory -Recurse -File)
    Check (@($files | Where-Object Name -eq 'clipture_engine.exe').Count -eq 1) 'Exactly one native engine'
    Check (@($files | Where-Object Name -eq 'ffmpeg.exe').Count -eq 1) 'Exactly one FFmpeg'
    Check (@($files | Where-Object { $_.Name -in @('app.asar','electron.exe','chrome_100_percent.pak') }).Count -eq 0) 'No Electron payload remains'
    $report.installedBytes = ($files | Measure-Object Length -Sum).Sum
}
function Launch-Tray([string]$directory) {
    # Test mode forbids capture/updater/autostart, but still tests installed assets
    # and native initialization. It uses the guest's legacy path, never host data.
    $env:CLIPTURE_TEST_MODE='1'; $env:CLIPTURE_DATA_DIR=$data
    $process = Start-Process -FilePath "$directory\clipture.exe" -ArgumentList '--hidden' -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 3
    Check (!$process.HasExited) 'Installed controller starts at tray idle'
    $report.trayPrivateBytes = $process.PrivateMemorySize64
    $expected = [IO.Path]::GetFullPath("$directory\clipture.exe")
    if (!$process.HasExited -and $process.Path -eq $expected) { Stop-Process -Id $process.Id }
    $process.WaitForExit(5000) | Out-Null
    Check-Data
}
function Check-Relaunch([string]$directory) {
    $expected = [IO.Path]::GetFullPath("$directory\clipture.exe")
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $matches = @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -eq $expected -and $_.CommandLine -notmatch ' --ui-worker(?:\s|$)'
        })
        if ($matches.Count -gt 0) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    Check ($matches.Count -eq 1) 'Electron --force-run launches exactly one installed Tauri controller'
    $rootPid = [int]$matches[0].ProcessId
    Start-Sleep -Seconds 3
    $report.relaunchPid = $rootPid
    $live = Get-Process -Id $rootPid
    Check (!$live.HasExited -and $live.Path -eq $expected) 'Relaunched controller remains alive'
    # Only this guest's newly installed application is stopped. RunAsUser may
    # discard TEST_MODE; fixture settings disable audio and all data is guest-only.
    $tree = @(Get-CimInstance Win32_Process)
    $ids = [Collections.Generic.HashSet[int]]::new()
    $null = $ids.Add($rootPid)
    do {
        $added = $false
        foreach ($entry in $tree) {
            if ($ids.Contains([int]$entry.ParentProcessId) -and $ids.Add([int]$entry.ProcessId)) { $added = $true }
        }
    } while ($added)
    $children = @($tree | Where-Object { $_.ProcessId -ne $rootPid -and $ids.Contains([int]$_.ProcessId) })
    Check (@($children | Where-Object { $_.ExecutablePath -eq $expected -and $_.CommandLine -match ' --ui-worker(?:\s|$)' }).Count -eq 1) 'Relaunch creates exactly one disposable UI worker'
    Stop-Process -Id $rootPid
    Start-Sleep -Seconds 5
    $survivors = @(foreach ($child in $children) {
        Get-CimInstance Win32_Process -Filter "ProcessId=$($child.ProcessId)" |
            Where-Object { $_.CreationDate -eq $child.CreationDate }
    })
    $report.relaunchSurvivors = @($survivors | Select-Object ProcessId,Name,ExecutablePath)
    Check ($survivors.Count -eq 0) 'Relaunched app leaves no child or grandchild processes'
    Check-Data
}
Publish
try {
    switch ($Phase) {
        cleanup_relaunch {
            Check (!(Test-Path -LiteralPath "$legacy\resources\app.asar")) 'Cleanup target is the failed-test Tauri upgrade, not Electron'
            foreach ($image in @("$legacy\clipture.exe", "$legacy\clipture_engine.exe")) {
                foreach ($process in @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $image })) {
                    $live = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
                    if ($live -and $live.Path -eq $image) { Stop-Process -Id $live.Id }
                }
            }
            Install "$legacy\uninstall.exe" "/S /$Scope"
            Check (Wait-Removed "$legacy\clipture.exe") 'Removed the failed relaunch test candidate'
        }
        cleanup_candidate {
            $candidate = "$env:ProgramFiles\Clipture"
            Check (!(Test-Path -LiteralPath "$candidate\resources\app.asar")) 'Cleanup target is not the Electron baseline'
            Install "$candidate\uninstall.exe" '/S /allusers'
            Check (Wait-Removed "$candidate\clipture.exe") 'Removed only the failed-test machine-wide Tauri candidate'
            Check-Data
        }
        inspect {
            $report.installs = @(foreach ($registryRoot in @('HKCU:', 'HKLM:')) {
                Get-ChildItem "$registryRoot\Software\Microsoft\Windows\CurrentVersion\Uninstall" |
                    ForEach-Object { Get-ItemProperty $_.PSPath } | Where-Object DisplayName -like '*Clipture*' |
                    Select-Object PSPath,InstallLocation,UninstallString
            })
            $report.files = @(foreach ($directory in @($testRoot, "$env:LOCALAPPDATA\Clipture", "$env:ProgramFiles\Clipture")) {
                if (Test-Path -LiteralPath $directory) {
                    Get-ChildItem -LiteralPath $directory -Recurse -File | Select-Object FullName,Length
                }
            })
            $report.processes = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -match 'clipture|setup|Tauri' } |
                Select-Object ProcessId,Name,ExecutablePath,CommandLine)
        }
        runtime {
            Install 'C:\CliptureInput\WebView2.exe' '/silent /install'
            Check (Test-Path -LiteralPath "${env:ProgramFiles(x86)}\Microsoft\EdgeWebView\Application") 'Offline WebView2 runtime installed in guest'
        }
        clean {
            $prior = "$env:LOCALAPPDATA\Programs\Clipture\uninstall.exe"
            if (Test-Path -LiteralPath $prior) {
                Install $prior '/S /currentuser'
            }
            Seed-Data
            Install 'C:\CliptureInput\Tauri.exe' "/S /currentuser /D=$clean"
            Check-Package $clean
            Launch-Tray $clean
            Install "$clean\uninstall.exe" '/S /currentuser'
            Check (Wait-Removed "$clean\clipture.exe") 'Tauri uninstaller removed the controller'
            Check-Data
        }
        legacy {
            Seed-Data
            Install 'C:\CliptureInput\Electron.exe' "/S /$Scope /D=$legacy"
            Check (Test-Path -LiteralPath "$legacy\resources\app.asar") 'Public Electron installer installed at custom path'
            $report.legacyInstallLocation = (Get-ItemProperty -LiteralPath "$registryHive\Software\7c2d96be-745e-5837-8fec-244a9a96ab0c").InstallLocation
            Check ($report.legacyInstallLocation -eq $legacy) 'Electron custom install path registered'
            Check-Data
        }
        rollback {
            Check (!(Test-Path -LiteralPath "$legacy\clipture.exe")) 'Rollback fixture starts without an installed app'
            Seed-Data
            Install 'C:\CliptureInput\Tauri.exe' "/S /$Scope /D=$legacy"
            Check-Package $legacy
            Launch-Tray $legacy
            Install "$legacy\uninstall.exe" "/S /$Scope"
            Check (Wait-Removed "$legacy\clipture.exe") 'Removed only the Tauri host before explicit rollback'
            Check-Data
            Install 'C:\CliptureInput\Electron.exe' "/S /$Scope /D=$legacy"
            Check (Test-Path -LiteralPath "$legacy\resources\app.asar") 'Public Electron rollback reinstalls at the preserved custom path'
            Launch-Tray $legacy
            Check-Data
            Install "$legacy\Uninstall Clipture.exe" "/S /KEEP_APP_DATA /$Scope"
            Check (Wait-Removed "$legacy\Clipture.exe") 'Rollback fixture uninstalls without removing its user data'
            Check-Data
        }
        blocked_crossgrade {
            Check (Test-Path -LiteralPath "$legacy\resources\app.asar") 'Blocked-upgrade test starts from Electron'
            $settingsPath = "$data\settings.json"
            $original = [IO.File]::ReadAllBytes($settingsPath)
            $nested = Join-Path $legacy 'Preflight Test Clips'
            $marker = Join-Path $nested 'preserve.mp4'
            New-Item -ItemType Directory -Path $nested | Out-Null
            [IO.File]::WriteAllText($marker, 'preflight-owned fixture')
            try {
                $settings = Get-Content -Raw -LiteralPath $settingsPath | ConvertFrom-Json
                $settings.saveFolder = $nested
                [IO.File]::WriteAllText($settingsPath, ($settings | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
                $process = Start-Process -FilePath 'C:\CliptureInput\Tauri.exe' -ArgumentList '--updated /S' -WindowStyle Hidden -PassThru
                $null = $process.Handle
                Check ($process.WaitForExit(30000)) 'Unsafe crossgrade returns without hanging'
                $report.installerExitCode = $process.ExitCode
                Check ($process.ExitCode -ne 0) 'Installer rejects user data inside the legacy directory'
                Check (Test-Path -LiteralPath "$legacy\resources\app.asar") 'Rejected upgrade leaves Electron installed'
                Check ([IO.File]::ReadAllText($marker) -eq 'preflight-owned fixture') 'Rejected upgrade preserves the nested clip'
            } finally {
                [IO.File]::WriteAllBytes($settingsPath, $original)
                Remove-Item -LiteralPath $marker
                Remove-Item -LiteralPath $nested
            }
            Check-Data
        }
        crossgrade {
            $env:CLIPTURE_TEST_MODE='1'; $env:CLIPTURE_DATA_DIR=$data
            $arguments = '--updated /S'
            if ($ForceRun) { $arguments += ' --force-run' }
            Install 'C:\CliptureInput\Tauri.exe' $arguments
            $report.uninstallEntries = @(Get-ChildItem "$registryHive\Software\Microsoft\Windows\CurrentVersion\Uninstall" |
                ForEach-Object { Get-ItemProperty $_.PSPath } | Where-Object DisplayName -like 'Clipture*' |
                Select-Object PSChildName,InstallLocation,UninstallString)
            Check-Package $legacy
            Check (@($report.uninstallEntries).Count -eq 1) 'Cross-grade leaves one uninstall registration'
            if ($ForceRun) { Check-Relaunch $legacy } else { Launch-Tray $legacy }
            Install "$legacy\uninstall.exe" "/S /$Scope"
            Check (Wait-Removed "$legacy\clipture.exe") 'Cross-graded app uninstalls'
            Check-Data
        }
    }
    $report.ok=$true
} catch { $report.error=$_.Exception.Message } finally { Publish }
if (!$report.ok) { exit 1 }
