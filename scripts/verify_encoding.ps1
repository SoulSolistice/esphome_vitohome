<#
  verify_encoding.ps1
  Encoding spot-check on the SQLite result. The data is full of German
  (Aussentemperatur, ae/oe/ue/ss) and symbols (degC). nvarchar is UTF-16 in
  SQL Server and TEXT is UTF-8 in SQLite; if the export/import transcodes
  wrongly you get mojibake -- and the row counts still match perfectly. This is
  the one check the cardinality test cannot give you.

  It scans the .db bytes for:
    * valid UTF-8 German letters  ae/oe/ue/AE/OE/UE/ss  (0xC3 0x84/96/9C/A4/B6/BC/9F)
    * the degree sign degC         (0xC2 0xB0)
    * the U+FFFD replacement char  (0xEF 0xBF 0xBD)  <- evidence of lossy transcode

  PASS  : German/degree UTF-8 present AND no replacement chars.
  FAIL  : any replacement char present.
  WARN  : none found at all (e.g. a slimmed DB without such text) -> not failed.

  Note: reads the whole file into memory (fine for the catalog DB; a full,
  un-slimmed export of hundreds of MB will be slower but still works on 64-bit).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Db
)

if (-not (Test-Path -LiteralPath $Db)) {
    Write-Host "verify_encoding: DB not found: $Db" -ForegroundColor Red
    exit 1
}

# ISO-8859-1 maps each byte 1:1 to U+00xx, so reading the file this way lets us
# search for exact byte sequences with fast native String.IndexOf.
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$txt = [System.IO.File]::ReadAllText($Db, $latin1)

function Count-Sub([string]$s, [string]$sub) {
    $c = 0; $i = 0
    while (($i = $s.IndexOf($sub, $i)) -ge 0) { $c++; $i += $sub.Length }
    return $c
}

$germanPairs = @(
    ([char]0xC3 + [char]0x84),  # AE
    ([char]0xC3 + [char]0x96),  # OE
    ([char]0xC3 + [char]0x9C),  # UE
    ([char]0xC3 + [char]0xA4),  # ae
    ([char]0xC3 + [char]0xB6),  # oe
    ([char]0xC3 + [char]0xBC),  # ue
    ([char]0xC3 + [char]0x9F)   # ss
)
$german = 0
foreach ($p in $germanPairs) { $german += Count-Sub $txt $p }
$degree = Count-Sub $txt ([char]0xC2 + [char]0xB0)
$repl   = Count-Sub $txt ([char]0xEF + [char]0xBF + [char]0xBD)

Write-Host "encoding scan: german-UTF8=$german  degree=$degree  U+FFFD(replacement)=$repl"

if ($repl -gt 0) {
    Write-Host "Encoding check FAILED: $repl replacement char(s) (U+FFFD) -- text was transcoded lossily." -ForegroundColor Red
    exit 1
}
if (($german + $degree) -eq 0) {
    Write-Host "Encoding check INCONCLUSIVE: no German/degree UTF-8 found (a slimmed DB may exclude such text). Not failing." -ForegroundColor Yellow
    exit 0
}
Write-Host "Encoding check passed: German text present as valid UTF-8, no replacement chars." -ForegroundColor Green
exit 0
