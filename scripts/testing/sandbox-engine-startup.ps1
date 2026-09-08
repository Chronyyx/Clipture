# Mutating process test, restricted to the disposable VM. Never starts capture.
param([Parameter(Mandatory=$true)][string]$Token)
$ErrorActionPreference = 'Stop'
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or
    (Get-Content -Raw -LiteralPath 'C:\CliptureInput\probe-token.txt').Trim() -ne $Token) {
    throw 'Only the identified disposable Sandbox may run this engine test.'
}
$report = [ordered]@{ok=$false;token=$Token}
$process = [Diagnostics.Process]::new()
$process.StartInfo.FileName = 'C:\CliptureInput\clipture_engine.exe'
$process.StartInfo.WorkingDirectory = 'C:\CliptureOutput'
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.CreateNoWindow = $true
$process.StartInfo.RedirectStandardInput = $true
$process.StartInfo.RedirectStandardOutput = $true
$process.StartInfo.RedirectStandardError = $true
try {
    if (!$process.Start()) { throw 'Engine did not start' }
    $report.pid = $process.Id
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.StandardInput.WriteLine('{"id":1,"type":"getDiagnostics"}')
    $process.StandardInput.Close()
    if (!$process.WaitForExit(20000)) { $process.Kill(); throw 'Engine startup/EOF shutdown timed out' }
    $report.exitCode = $process.ExitCode
    $report.stderr = $stderr.GetAwaiter().GetResult()
    $lines = $stdout.GetAwaiter().GetResult().Split("`n") | Where-Object { $_.Trim() -ne '' }
    $responses = @($lines | ForEach-Object { $_ | ConvertFrom-Json })
    $response = @($responses | Where-Object id -eq 1)
    $report.response = $response
    if ($process.ExitCode -ne 0 -or $response.Count -ne 1 -or !$response[0].payload) {
        throw 'Engine did not return its diagnostics envelope and exit cleanly'
    }
    $report.ok=$true
} catch { $report.error=$_.Exception.Message } finally {
    $process.Dispose()
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath 'C:\CliptureOutput\engine-startup.json' -Encoding UTF8
}
if (!$report.ok) { exit 1 }
