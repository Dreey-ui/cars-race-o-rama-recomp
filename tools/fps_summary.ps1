# Resume as estatisticas [gpu] do run.err.log: fps medio/minimo nas cenas pesadas e custos do processador de comandos
param([string]$Log = "$PSScriptRoot\..\run.err.log", [int]$HeavyDraws = 100000)
$rows = foreach ($line in Get-Content $Log) {
    if ($line -match '^\[gpu\] ([0-9.]+) fps \(pior frame (\d+) ms\) \| draws (\d+),.*CP trabalho (-?\d+) \(esperando CPU (\d+)\), draw (\d+) \(bind (\d+), hash (\d+), tex (\d+), vb (\d+)') {
        [pscustomobject]@{ fps = [double]$matches[1]; worst = [int]$matches[2]; draws = [int]$matches[3]; cp = [int]$matches[4];
                           wait = [int]$matches[5]; draw = [int]$matches[6]; vb = [int]$matches[10]; line = $line }
    }
}
$all = $rows | Select-Object -Skip 10
$heavy = $all | Where-Object { $_.draws -ge $HeavyDraws }
"segundos: {0} | fps medio {1:N1} | cenas pesadas (>= {2} draws/s): {3} s, fps medio {4:N1}, minimo {5:N1}" -f $all.Count,
    ($all | Measure-Object fps -Average).Average, $HeavyDraws, $heavy.Count, ($heavy | Measure-Object fps -Average).Average,
    ($heavy | Measure-Object fps -Minimum).Minimum
if ($heavy) {
    "nas pesadas: draws/s {0:N0}, CP trabalho {1:N0} ms/s, esperando CPU {2:N0}, draw {3:N0}, vb {4:N0}" -f ($heavy | Measure-Object draws -Average).Average,
        ($heavy | Measure-Object cp -Average).Average, ($heavy | Measure-Object wait -Average).Average,
        ($heavy | Measure-Object draw -Average).Average, ($heavy | Measure-Object vb -Average).Average
}
