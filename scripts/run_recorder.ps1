# Keeps the BlockSprint market-data recorder running: restarts it whenever it exits.
# Started at logon by the "BlockSprint Recorder" scheduled task (see docs/recorder_ops.md).
param(
    [string[]]$Symbols = @("BTCUSDT", "ETHUSDT"),
    [string]$Out = "data/raw",
    [int]$RestartDelaySeconds = 10
)

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
New-Item -ItemType Directory -Force -Path "logs" | Out-Null
$supervisorLog = Join-Path $repo "logs/recorder_supervisor.log"

function Write-SupervisorLog([string]$message) {
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $message
    Add-Content -Path $supervisorLog -Value $line -Encoding utf8
}

$uv = Join-Path $env:USERPROFILE ".local\bin\uv.exe"
if (-not (Test-Path $uv)) {
    $uv = (Get-Command uv -ErrorAction Stop).Source
}

Write-SupervisorLog "supervisor started (pid $PID), symbols $($Symbols -join ' '), archive $Out"
while ($true) {
    $started = Get-Date
    & $uv run python -m research.data.recorder --symbols @Symbols --out $Out *> $null
    $code = $LASTEXITCODE
    $ran = [int]((Get-Date) - $started).TotalSeconds
    Write-SupervisorLog "recorder exited with code $code after ${ran}s; restarting in ${RestartDelaySeconds}s"
    Start-Sleep -Seconds $RestartDelaySeconds
}
