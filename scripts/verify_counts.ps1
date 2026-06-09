<#
  verify_counts.ps1
  Per-table row-count comparison between the SQL Server source and the SQLite
  result. Reports every table whose counts differ (or that is missing on one
  side) and exits non-zero if any discrepancy is found.

  Inputs are two "name,count" files (no header):
    -Mssql   from: sqlcmd ... -h -1 -W -s "," "SELECT name, SUM(rows) ..."
    -Sqlite  from: sqlite3 -csv "SELECT 'name', COUNT(*) ... UNION ALL ..."

  A grand-total check would mask a skipped table and tells you nothing about
  WHICH table; comparing per table pinpoints problems (e.g. an exporter that
  skips tables without a primary key).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Mssql,
    [Parameter(Mandatory = $true)][string]$Sqlite
)

function Read-Counts([string]$path) {
    $h = @{}
    if (-not (Test-Path -LiteralPath $path)) { return $h }
    foreach ($line in Get-Content -LiteralPath $path) {
        $t = $line.Trim()
        if ($t -eq '' -or $t -match '^-+$') { continue }   # blank / sqlcmd rule line
        $i = $t.LastIndexOf(',')
        if ($i -lt 1) { continue }
        $name = $t.Substring(0, $i).Trim()
        $cnt  = $t.Substring($i + 1).Trim()
        $n = [int64]0
        if (-not [int64]::TryParse($cnt, [ref]$n)) { continue }
        $h[$name] = $n
    }
    return $h
}

$src = Read-Counts $Mssql    # SQL Server
$dst = Read-Counts $Sqlite   # SQLite

if ($src.Count -eq 0) {
    Write-Host "verify_counts: no SQL Server counts read from '$Mssql'." -ForegroundColor Red
    exit 1
}

$names = @($src.Keys + $dst.Keys | Sort-Object -Unique)
$bad = 0

Write-Host ("{0,-44}{1,12}{2,12}" -f 'TABLE', 'SQLSERVER', 'SQLITE')
foreach ($n in $names) {
    $x = if ($src.ContainsKey($n)) { $src[$n] } else { $null }
    $y = if ($dst.ContainsKey($n)) { $dst[$n] } else { $null }
    if ($x -ne $y) {
        $bad++
        $xs = if ($null -eq $x) { 'MISSING' } else { "$x" }
        $ys = if ($null -eq $y) { 'MISSING' } else { "$y" }
        Write-Host ("{0,-44}{1,12}{2,12}  <-- MISMATCH" -f $n, $xs, $ys) -ForegroundColor Red
    }
}

if ($bad -gt 0) {
    Write-Host "Row-count check FAILED: $bad table(s) differ." -ForegroundColor Red
    exit 1
}
Write-Host "Row-count check passed: $($names.Count) table(s) match." -ForegroundColor Green
exit 0
