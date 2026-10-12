# Instala o jogo recompilado DENTRO da pasta do jogo (ISO extraida): exe com o XEX embutido + DLLs + cache de shaders
# + pasta de mods + fontes Lua. Depois e so abrir "Cars Race-O-Rama.exe" na pasta do jogo (sem argumentos, sem .xex).
# Uso: powershell -File tools\deploy.ps1 [-Game "<pasta do jogo>"]
param(
    [string]$Game = "C:\Games\Cars Race-O-Rama Recomp"
)
$root = Split-Path $PSScriptRoot
$rel = "$root\build\Release"
if (-not (Test-Path "$Game\C")) { throw "Pasta do jogo invalida: $Game" }
if (Get-Process -Name "Cars Race-O-Rama" -ErrorAction SilentlyContinue) { throw "Feche o jogo antes de instalar." }

Copy-Item "$rel\CarsRecomp.exe" "$Game\Cars Race-O-Rama.exe" -Force
foreach ($dll in "dxcompiler.dll", "dxil.dll", "avcodec-62.dll", "avutil-60.dll", "swresample-6.dll") {
    Copy-Item "$rel\$dll" "$Game\$dll" -Force
}
# Shaders ja compilados (evita recompilar na primeira vez)
New-Item -ItemType Directory -Force "$Game\shader_cache" | Out-Null
Get-ChildItem "$rel\shader_cache" -File -Filter *.dxil | ForEach-Object {
    $dst = Join-Path "$Game\shader_cache" $_.Name
    if (-not (Test-Path $dst)) { Copy-Item $_.FullName $dst }
}
# Mods (nunca apaga os mods existentes) e fontes Lua descompiladas para referencia
New-Item -ItemType Directory -Force "$Game\mods" | Out-Null
Copy-Item "$rel\mods\LEIA-ME.txt" "$Game\mods\LEIA-ME.txt" -Force
if (Test-Path "$root\lua_src") {
    New-Item -ItemType Directory -Force "$Game\lua_src" | Out-Null
    Copy-Item "$root\lua_src\*" "$Game\lua_src" -Recurse -Force
}
"instalado em: $Game"
Get-ChildItem $Game -File | Where-Object { $_.Extension -in ".exe", ".dll" } | ForEach-Object { "  {0,-24} {1,6:N1} MB" -f $_.Name, ($_.Length / 1MB) }
