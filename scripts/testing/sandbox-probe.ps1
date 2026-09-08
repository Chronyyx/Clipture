# Runs only inside a disposable Windows Sandbox, never on the host desktop.
$ErrorActionPreference = 'Stop'
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or !(Test-Path -LiteralPath 'C:\CliptureInput\probe-token.txt')) {
    throw 'This probe must be launched by the isolated Sandbox runner.'
}
$runtimeRoots = @(
    "${env:ProgramFiles(x86)}\Microsoft\EdgeWebView\Application",
    "$env:LOCALAPPDATA\Microsoft\EdgeWebView\Application"
)
$runtimes = @(foreach ($root in $runtimeRoots) {
    if (Test-Path -LiteralPath $root) {
        Get-ChildItem -LiteralPath $root -Directory | Select-Object -ExpandProperty Name
    }
})
[ordered]@{
    ok = $true
    token = (Get-Content -Raw -LiteralPath 'C:\CliptureInput\probe-token.txt').Trim()
    user = $env:USERNAME
    osVersion = [Environment]::OSVersion.Version.ToString()
    webviewRuntimes = $runtimes
    availableBytes = (Get-PSDrive C).Free
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath 'C:\CliptureOutput\probe-result.json' -Encoding UTF8
