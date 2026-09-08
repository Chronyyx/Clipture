param([Parameter(Mandatory=$true)][string]$Token)
$ErrorActionPreference = 'Stop'
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or
    (Get-Content -Raw -LiteralPath 'C:\CliptureInput\probe-token.txt').Trim() -ne $Token) {
    throw 'Only the identified disposable Sandbox may run this experiment.'
}
$taskRoot = 'C:\CliptureRawWebView'
$profile = Join-Path $taskRoot ('.cache\probe-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile -Force | Out-Null
$app = Start-Process -FilePath 'C:\CliptureInput\webview_lifecycle.exe' -ArgumentList "$profile 10" -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput "$profile\stdout.log" -RedirectStandardError "$profile\stderr.log"
$monitorArgs = "-NoProfile -ExecutionPolicy Bypass -File C:\CliptureInput\watch-tauri-resources.ps1 -AppProcessId $($app.Id) -Profile $profile"
$monitor = Start-Process -FilePath powershell.exe -ArgumentList $monitorArgs -WindowStyle Hidden -PassThru
$exited = $app.WaitForExit(180000)
if (!$exited) { Stop-Process -Id $app.Id }
$monitor.WaitForExit(15000) | Out-Null
$report = [ordered]@{ok=$false;token=$Token;profile=$profile;appExited=$exited;exitCode=$app.ExitCode;stderr=(Get-Content -Raw -LiteralPath "$profile\stderr.log")}
try {
    $rows = @(Get-Content -Raw -LiteralPath "$profile\raw-webview-result.json" | ConvertFrom-Json)
    $report.handles = @($rows | ForEach-Object { $_.closed.controllerHandleTypes.Process })
    $report.growth = $report.handles[-1] - $report.handles[0]
    $report.orphans = Get-Content -Raw -LiteralPath "$profile\app-exit-resources.json" | ConvertFrom-Json
    $report.runtime = @(Get-ChildItem -LiteralPath "${env:ProgramFiles(x86)}\Microsoft\EdgeWebView\Application" -Directory | Select-Object -ExpandProperty Name)
    $report.ok = $exited -and $app.ExitCode -eq 0 -and $rows.Count -eq 10 -and $report.growth -le 2 -and $report.orphans.ok
    Copy-Item -LiteralPath "$profile\raw-webview-result.json" -Destination 'C:\CliptureOutput\raw-webview-details.json'
} catch { $report.error = $_.Exception.Message }
$report | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath 'C:\CliptureOutput\raw-webview-summary.json' -Encoding UTF8
if (!$report.ok) { exit 1 }
