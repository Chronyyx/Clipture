# Recover only a failed, identified updater fixture so its test can be repeated.
param([Parameter(Mandatory=$true)][string]$Token)
$ErrorActionPreference='Stop'
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or
    (Get-Content -Raw -LiteralPath 'C:\CliptureInput\probe-token.txt').Trim() -ne $Token) { throw 'Wrong test VM' }
$target='C:\CliptureInstallerTest\Update Candidate'
if (!(Test-Path -LiteralPath "$target\clipture.exe")) { exit 0 }
$expected=@((Get-FileHash -LiteralPath 'C:\CliptureInput\expected-controller.exe').Hash,
    (Get-FileHash -LiteralPath 'C:\CliptureInput\updater-harness.exe').Hash)
if ((Get-FileHash -LiteralPath "$target\clipture.exe").Hash -notin $expected) { throw 'Unexpected cleanup target bytes' }
foreach ($entry in @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq "$target\clipture.exe" })) {
    $live=Get-Process -Id $entry.ProcessId -ErrorAction SilentlyContinue
    if ($live -and $live.Path -eq "$target\clipture.exe") { $null=$live.Handle; $live.Kill(); $live.WaitForExit(5000) | Out-Null }
}
$uninstall=Start-Process -FilePath "$target\uninstall.exe" -ArgumentList '/S /currentuser' -WindowStyle Hidden -PassThru
$null=$uninstall.Handle
if (!$uninstall.WaitForExit(30000) -or $uninstall.ExitCode -ne 0) { throw 'Fixture uninstaller failed' }
$deadline=[DateTime]::UtcNow.AddSeconds(30)
while ((Test-Path -LiteralPath "$target\clipture.exe") -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
}
if (Test-Path -LiteralPath "$target\clipture.exe") { throw 'Fixture executable still exists' }
'Removed only the failed updater fixture installation; guest user data retained.' |
    Set-Content -LiteralPath 'C:\CliptureOutput\updater-cleanup.txt'
