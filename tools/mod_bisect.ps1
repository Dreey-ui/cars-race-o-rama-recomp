# Testa se um conjunto de scripts em mods\ muda o fluxo da tela de titulo: sem input, o jogo original entra no modo
# demonstracao (corrida 3D, > 40 mil draws/s) por volta de 75 s. Uso: tools\mod_bisect.ps1 -Files "C\Script\defs.lua;C\Story\x.lua" | -All
param([string]$Files = "", [switch]$All, [int]$Seconds = 120)
$root = Split-Path $PSScriptRoot
$mods = "$root\build\Release\mods"
if (Test-Path $mods) { Remove-Item -Recurse -Force $mods }
if ($All) { Copy-Item -Recurse "$root\lua_src" $mods } else { $list = $Files.Split(";") | Where-Object { $_ } }
foreach ($f in $list) {
    $dst = Join-Path $mods $f
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item (Join-Path "$root\lua_src" $f) $dst
}
$null = & powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\run.ps1" -Seconds $Seconds
$max = 0
foreach ($line in Get-Content "$root\run.err.log") {
    if ($line -match '^\[gpu\] [0-9.]+ fps .*\| draws (\d+),') { $max = [Math]::Max($max, [int]$matches[1]) }
}
$lua = (Select-String -Path "$root\run.err.log" -Pattern '^\[lua\]').Count
"{0}: max draws/s {1}, erros lua {2} -> {3}" -f $(if ($All) { "todos os scripts" } else { $Files }), $max, $lua, $(if ($max -gt 40000) { 'DEMO OK' } else { 'SEM DEMO' })
