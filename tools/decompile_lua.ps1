# Descompila todos os scripts Lua do jogo (*.lua.bin: 4 bytes de tamanho + bytecode Lua 5.1 com numeros float)
# para lua_src\<mesmo caminho>\X.lua (fontes de referencia, editaveis).
# Para mudar um script: copie o .lua para mods\<mesmo caminho>\X.lua e edite; o runtime usa no lugar do X.lua.bin.
# Uso: powershell -File tools\decompile_lua.ps1 [-Game "<pasta da ISO>"] [-Out lua_src]
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
        # ja e codigo-fonte (ex.: FE_FrontEnd): so tira o prefixo de tamanho
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
"descompilados: $ok de $($files.Count) -> $Out"
if ($fail) { "falharam:"; $fail }

# Corrige os trechos com goto (Lua 5.2) que o unluac gera em 4 arquivos
python -I "$PSScriptRoot\fix_lua_goto.py" $Out
