param([ValidateSet('candidate', 'control')][string]$Mode = 'candidate')
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$stage = Join-Path $repository 'release/tauri-source-retirement'
$manifest = Get-Content -Raw -LiteralPath (Join-Path $stage 'candidate.json') | ConvertFrom-Json
$engine = Join-Path $stage 'clipture_engine.exe'
$validation = Get-Content -Raw -LiteralPath (Join-Path $repository $manifest.validationReport) | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $engine).Hash -ne $manifest.sha256 -or
    $validation.sha256 -ne $manifest.sha256 -or -not $validation.complete -or
    $validation.trials.Count -ne $validation.expectedTrials -or
    @($validation.trials | Where-Object { -not $_.passed }).Count) {
    throw 'Candidate hash or complete hardware validation does not match.'
}
$active = @(Get-Process -Name clipture,clipture_engine -ErrorAction SilentlyContinue |
    Where-Object { -not $_.HasExited })
if ($active.Count) { throw 'Exit the running Clipture from its tray first. This launcher never closes another recorder.' }
$flags = @{
    CLIPTURE_CAPTURE_BACKEND = 'wgc'; CLIPTURE_PIPELINE_TRACE = '1'
    CLIPTURE_DIRECT_TEXTURE_READ = '1'; CLIPTURE_ISOLATE_NVENC = '1'
    CLIPTURE_NV12_INPUT = '1'; CLIPTURE_DEFER_PREPARATION = '1'
    CLIPTURE_GPU_HANDOFF = '1'; CLIPTURE_CAPTURE_IDLE_BACKOFF = '1'
    CLIPTURE_EARLY_SOURCE_RETIRE = $(if ($Mode -eq 'candidate') { '1' } else { '0' })
}
$previous = @{}
$log = Join-Path $repository ('.cache/cs2-perf/source-retirement-' + $Mode + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
try {
    foreach ($key in $flags.Keys) {
        $previous[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        [Environment]::SetEnvironmentVariable($key, $flags[$key], 'Process')
    }
    $process = Start-Process -FilePath (Join-Path $stage 'clipture.exe') -ArgumentList '--hidden' -WorkingDirectory $stage -WindowStyle Hidden -RedirectStandardError "$log.stderr.log" -RedirectStandardOutput "$log.stdout.log" -PassThru
    Write-Output "Started $Mode PID $($process.Id), using existing settings. Trace: $log.stderr.log"
} finally {
    foreach ($key in $previous.Keys) { [Environment]::SetEnvironmentVariable($key, $previous[$key], 'Process') }
}
