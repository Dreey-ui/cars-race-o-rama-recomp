# Installs the recompiled game INSIDE the game folder (extracted ISO): exe with the embedded XEX + DLLs + shader cache
# + mods folder + Lua sources. Then just open "Cars Race-O-Rama.exe" in the game folder (no arguments, no .xex).
# Usage: powershell -File tools\deploy.ps1 [-Game "<game folder>"]
param(
    [string]$Game = "C:\Games\Cars Race-O-Rama Recomp"
)
$root = Split-Path $PSScriptRoot
$rel = "$root\build\Release"
if (-not (Test-Path "$Game\C")) { throw "Invalid game folder: $Game" }
if (Get-Process -Name "Cars Race-O-Rama" -ErrorAction SilentlyContinue) { throw "Close the game before installing." }

Copy-Item "$rel\CarsRecomp.exe" "$Game\Cars Race-O-Rama.exe" -Force
foreach ($dll in "dxcompiler.dll", "dxil.dll", "avcodec-62.dll", "avutil-60.dll", "swresample-6.dll") {
    Copy-Item "$rel\$dll" "$Game\$dll" -Force
}
# Already compiled shaders (avoids compiling them again on the first run)
New-Item -ItemType Directory -Force "$Game\shader_cache" | Out-Null
Get-ChildItem "$rel\shader_cache" -File -Filter *.dxil | ForEach-Object {
    $dst = Join-Path "$Game\shader_cache" $_.Name
    if (-not (Test-Path $dst)) { Copy-Item $_.FullName $dst }
}
# Mods (never deletes existing mods) and decompiled Lua sources for reference
New-Item -ItemType Directory -Force "$Game\mods" | Out-Null
Copy-Item "$root\dist\mods\README.txt" "$Game\mods\README.txt" -Force
if (Test-Path "$Game\mods\LEIA-ME.txt") { Remove-Item "$Game\mods\LEIA-ME.txt" }   # Portuguese readme of older builds
if (Test-Path "$root\lua_src") {
    New-Item -ItemType Directory -Force "$Game\lua_src" | Out-Null
    Copy-Item "$root\lua_src\*" "$Game\lua_src" -Recurse -Force
}
"installed to: $Game"
Get-ChildItem $Game -File | Where-Object { $_.Extension -in ".exe", ".dll" } | ForEach-Object { "  {0,-24} {1,6:N1} MB" -f $_.Name, ($_.Length / 1MB) }
