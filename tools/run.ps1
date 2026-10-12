# Roda o jogo por N segundos, grava run.err.log / run.out.log e encerra.
# Uso: powershell -File tools\run.ps1 [-Seconds 30] [-Dump "60,300"] [-GpuLog] [-Trace] [-Extra "--novsync"]
param(
    [int]$Seconds = 30,
    [string]$Dump = "",
    [switch]$GpuLog,
    [switch]$Trace,
    [string]$Extra = ""
)
$root = Split-Path $PSScriptRoot
Set-Location $root
$iso = "C:\Games\Cars Race-O-Rama Recomp"
Get-ChildItem -Path "$root\*" -Include "frame_*.bmp", "frame_*.png" -File | ForEach-Object { [IO.File]::Delete($_.FullName) }
if ($GpuLog) { $env:CARS_GPU_LOG = "1" } else { $env:CARS_GPU_LOG = $null }
if ($Dump) { $env:CARS_DUMP_FRAMES = $Dump } else { $env:CARS_DUMP_FRAMES = $null }
$env:CARS_HIDDEN_WINDOW = "1"   # testes nao roubam o foco do usuario
$args = @("C:\DEB\cars\default_dec.xex", "`"$iso`"")
if ($Trace) { $args += "--trace" }
if ($Extra) { $args += $Extra.Split(" ") }
$p = Start-Process -FilePath "$root\build\Release\CarsRecomp.exe" -ArgumentList $args `
    -RedirectStandardError "$root\run.err.log" -RedirectStandardOutput "$root\run.out.log" -PassThru
$null = $p.Handle   # garante que o ExitCode fique disponivel
$exited = $p.WaitForExit($Seconds * 1000)
if (-not $exited) { Stop-Process -Id $p.Id -Force; "encerrado apos $Seconds s" } else { "saiu sozinho com codigo 0x{0:X8}" -f $p.ExitCode }
