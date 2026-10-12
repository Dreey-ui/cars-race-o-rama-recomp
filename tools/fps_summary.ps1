# Summarizes the [gpu] stats of run.err.log: average/minimum fps in heavy scenes and command processor costs
param([string]$Log = "$PSScriptRoot\..\run.err.log", [int]$HeavyDraws = 100000)
$rows = foreach ($line in Get-Content $Log) {
    if ($line -match '^\[gpu\] ([0-9.]+) fps \((?:worst|pior) frame (\d+) ms\) \| draws (\d+),.*(?:CP work|CP trabalho) (-?\d+) \((?:waiting for CPU|esperando CPU) (\d+)\), draw (\d+) \(bind (\d+), hash (\d+), tex (\d+), vb (\d+)') {
        [pscustomobject]@{ fps = [double]$matches[1]; worst = [int]$matches[2]; draws = [int]$matches[3]; cp = [int]$matches[4];
                           wait = [int]$matches[5]; draw = [int]$matches[6]; vb = [int]$matches[10]; line = $line }
    }
}
$all = $rows | Select-Object -Skip 10
$heavy = $all | Where-Object { $_.draws -ge $HeavyDraws }
"seconds: {0} | average fps {1:N1} | heavy scenes (>= {2} draws/s): {3} s, average fps {4:N1}, minimum {5:N1}" -f $all.Count,
    ($all | Measure-Object fps -Average).Average, $HeavyDraws, $heavy.Count, ($heavy | Measure-Object fps -Average).Average,
    ($heavy | Measure-Object fps -Minimum).Minimum
if ($heavy) {
    "in heavy scenes: draws/s {0:N0}, CP work {1:N0} ms/s, waiting for CPU {2:N0}, draw {3:N0}, vb {4:N0}" -f ($heavy | Measure-Object draws -Average).Average,
        ($heavy | Measure-Object cp -Average).Average, ($heavy | Measure-Object wait -Average).Average,
        ($heavy | Measure-Object draw -Average).Average, ($heavy | Measure-Object vb -Average).Average
}
