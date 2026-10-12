# Decompiles all the game's Lua scripts (*.lua.bin: 4-byte size + Lua 5.1 bytecode with float numbers)
# to lua_src\<same path>\X.lua (reference sources, editable).
# To change a script: copy the .lua to mods\<same path>\X.lua and edit it; the runtime uses it instead of X.lua.bin.
# Usage: powershell -File tools\decompile_lua.ps1 [-Game "<game folder>"] [-Out lua_src]
param(
    [string]$Game = "C:\Games\Cars Race-O-Rama Recomp",
    [string]$Out = "$PSScriptRoot\..\lua_src"
)
$root = Split-Path $PSScriptRoot
$java = "$root\third_party\jre\bin\java.exe"
$jar = (Get-ChildItem "$root\third_party\unluac\unluac_*.jar" | Sort-Object Name | Select-Object -Last 1).FullName
$tmp = Join-Path $env:TEMP "cars_unluac"
New-Item -ItemType Directory -Force $tmp | Out-Null
$files = Get-ChildItem $Game -Recurse -File -Filter *.lua.bin
$ok = 0; $fail = @()
foreach ($f in $files) {
    $rel = $f.FullName.Substring($Game.Length).TrimStart('\')
    $dst = Join-Path $Out ($rel -replace '\.bin$', '')
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    $b = [IO.File]::ReadAllBytes($f.FullName)
    if ($b.Length -gt 4 -and $b[4] -ne 0x1B) {
        # already source code (e.g. FE_FrontEnd): just strip the size prefix
        [IO.File]::WriteAllBytes($dst, $b[4..($b.Length - 1)])
        $ok++
        continue
    }
    $luac = Join-Path $tmp "x.luac"
    [IO.File]::WriteAllBytes($luac, $b[4..($b.Length - 1)])
    $src = & $java -XX:TieredStopAtLevel=1 -jar $jar $luac 2>$null
    if ($LASTEXITCODE -eq 0 -and $src) {
        [IO.File]::WriteAllText($dst, (($src -join "`n") + "`n"))
        $ok++
    } else {
        $fail += $rel
    }
}
"decompiled: $ok of $($files.Count) -> $Out"
if ($fail) { "failed:"; $fail }

# Fixes the goto spots (Lua 5.2) that unluac generates in 4 files
python -I "$PSScriptRoot\fix_lua_goto.py" $Out
