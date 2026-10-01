param(
    [Parameter(Mandatory=$true)][string]$Stage,
    [ValidateSet('candidate','control')][string]$Mode = 'candidate'
)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$stagePath = (Resolve-Path -LiteralPath $Stage).Path
$allowed = Join-Path $repository '.cache\cs2-perf\'
if (-not $stagePath.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Expected an isolated workspace stage.' }
$manifest = Get-Content -Raw -LiteralPath (Join-Path $stagePath 'candidate.json') | ConvertFrom-Json
$report = Get-Content -Raw -LiteralPath $manifest.validationReport | ConvertFrom-Json
if (-not $report.complete -or $report.trials.Count -ne 6 -or @($report.trials | Where-Object { -not $_.passed }).Count) { throw 'Incomplete compatibility validation.' }
foreach ($file in @('clipture.exe','clipture_engine.exe','ffmpeg.exe','assets/default.mp3','assets/option2.wav')) {
    if ((Get-FileHash -LiteralPath (Join-Path $stagePath $file)).Hash -ne $manifest.files.$file) { throw "Runtime hash mismatch: $file" }
}
if ($report.sha256 -ne $manifest.files.'clipture_engine.exe') { throw 'Report belongs to a different engine.' }
$profile = Join-Path $stagePath 'profile'
$settingsPath = Join-Path $profile 'settings.json'
if ((Get-Content -Raw -LiteralPath $settingsPath | ConvertFrom-Json).fps -ne 120) { throw 'This experiment requires 120 FPS.' }
if (Get-Process -Name clipture,clipture_engine -ErrorAction SilentlyContinue) { throw 'Exit Clipture from its tray first; this script never stops another recorder.' }
$flags = @{
    CLIPTURE_DATA_DIR = $profile; CLIPTURE_ENGINE_PATH = (Join-Path $stagePath 'clipture_engine.exe')
    CLIPTURE_FFMPEG_PATH = (Join-Path $stagePath 'ffmpeg.exe'); CLIPTURE_TEST_MODE = $null
    CLIPTURE_CAPTURE_BACKEND = 'wgc'; CLIPTURE_PIPELINE_TRACE = '1'
    CLIPTURE_DIRECT_TEXTURE_READ = '1'; CLIPTURE_ISOLATE_NVENC = '1'
    CLIPTURE_NV12_INPUT = '1'; CLIPTURE_DEFER_PREPARATION = '1'
    CLIPTURE_GPU_HANDOFF = '1'; CLIPTURE_CAPTURE_IDLE_BACKOFF = '1'
    CLIPTURE_EARLY_SOURCE_RETIRE = '0'
    CLIPTURE_DIRECT_FRESH_CONVERSION = $(if ($Mode -eq 'candidate') { '1' } else { '0' })
}
$previous = @{}
$log = Join-Path $stagePath ($Mode + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
try {
    foreach ($key in $flags.Keys) {
        $previous[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        [Environment]::SetEnvironmentVariable($key, $flags[$key], 'Process')
    }
    $process = Start-Process -FilePath (Join-Path $stagePath 'clipture.exe') -ArgumentList '--hidden' -WorkingDirectory $stagePath -WindowStyle Hidden -RedirectStandardError "$log.stderr.log" -RedirectStandardOutput "$log.stdout.log" -PassThru
    Write-Output "Started $Mode PID $($process.Id). Private profile: $profile. Trace: $log.stderr.log"
} finally {
    foreach ($key in $previous.Keys) { [Environment]::SetEnvironmentVariable($key, $previous[$key], 'Process') }
}
