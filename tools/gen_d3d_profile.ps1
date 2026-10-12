# Gera src/d3d_profile_gen.cpp: um wrapper (hook) para cada funcao guest dos arquivos indicados.
# Cada wrapper conta a chamada (e o chamador) e chama a implementacao original __imp__sub_X.
param(
    [string]$PpcDir = "C:\DEB\cars\ppc",
    [int[]]$Files = @(30, 31),
    [string]$Out = "$PSScriptRoot\..\src\d3d_profile_gen.cpp"
)

# Funcoes que ja tem hook proprio em outro arquivo do runtime (ex.: o kick XMA em src/xma.cpp) ficam de fora
$srcDir = Split-Path $Out
$hooked = Get-ChildItem "$srcDir\*.cpp" | Where-Object { $_.Name -ne (Split-Path $Out -Leaf) } |
    Select-String -Pattern '^PPC_FUNC\(sub_([0-9A-F]{8})\)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
$addrs = foreach ($n in $Files) {
    Select-String -Path "$PpcDir\ppc_recomp.$n.cpp" -Pattern '^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } | Where-Object { $hooked -notcontains $_ }
}
if ($hooked) { "com hook proprio (excluidas): $($hooked -join ', ')" }

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("// GERADO por tools/gen_d3d_profile.ps1 - nao editar")
[void]$sb.AppendLine("#include `"runtime.h`"")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("void D3DProfHit(uint32_t index, PPCContext& ctx);")
[void]$sb.AppendLine("")
$i = 0
foreach ($a in $addrs) {
    [void]$sb.AppendLine("extern `"C`" PPC_FUNC(__imp__sub_$a);")
    [void]$sb.AppendLine("PPC_FUNC(sub_$a) { D3DProfHit($i, ctx); __imp__sub_$a(ctx, base); }")
    $i++
}
[void]$sb.AppendLine("")
[void]$sb.AppendLine("extern const uint32_t g_d3dProfCount = $($addrs.Count);")
[void]$sb.AppendLine("extern const uint32_t g_d3dProfAddrs[] = {")
foreach ($a in $addrs) { [void]$sb.AppendLine("    0x$a,") }
[void]$sb.AppendLine("};")

[IO.File]::WriteAllText((Resolve-Path -LiteralPath (Split-Path $Out)).Path + "\" + (Split-Path $Out -Leaf), $sb.ToString())
"gerado: $($addrs.Count) funcoes ($($addrs[0]) .. $($addrs[-1])) -> $Out"
