param([Parameter(Mandatory=$true)][string]$Token)
$ErrorActionPreference='Stop'
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or
    (Get-Content -Raw -LiteralPath 'C:\CliptureInput\probe-token.txt').Trim() -ne $Token) { throw 'Wrong test VM' }
$installed='C:\CliptureInstallerTest\Update Candidate\clipture.exe'
Get-FileHash -LiteralPath $installed,'C:\CliptureInput\expected-controller.exe' |
    ConvertTo-Json | Set-Content -LiteralPath 'C:\CliptureOutput\updater-hashes.json'
Copy-Item -LiteralPath $installed -Destination 'C:\CliptureOutput\updater-installed.exe'
