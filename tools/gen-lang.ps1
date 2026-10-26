# Builds res/lang.txt: translations of FastFiles' UI strings (the Chinese literals in src/) for Files' 49 languages.
# A literal that is exactly one of Files' zh-Hans strings takes Files' own translations (MIT, Desktop\11\Files);
# the rest come from tools/lang-en.txt (zh=>en), which also fills en-US. Other languages fall back to English at
# run time (Lang.cpp), as Files does. Run after adding UI strings:  powershell -File tools\gen-lang.ps1 [-Missing]
param([switch]$Missing)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$files = Join-Path (Split-Path $root) 'Files\src\Files.App\Strings'

function Unescape([string]$s) {
  $s -replace '\\t', "`t" -replace '\\n', "`n" -replace '\\"', '"' -replace '\\\\', '\'
}
function Escape([string]$s) { $s.Replace('\', '\\').Replace("`t", '\t').Replace("`n", '\n').Replace("`r", '') }

# Every Chinese literal in the sources; menu texts "名称\tCtrl+E" are looked up by the part before the tab
$keys = New-Object 'System.Collections.Generic.SortedSet[string]' ([StringComparer]::Ordinal)
foreach ($f in Get-ChildItem (Join-Path $root 'src') -Include *.cpp, *.h -Recurse) {
  foreach ($m in [regex]::Matches([IO.File]::ReadAllText($f.FullName), 'L"((?:[^"\\\r\n]|\\.)*)"')) {
    $v = Unescape $m.Groups[1].Value
    if ($v -notmatch '[一-鿿]') { continue }
    # "名称\tCtrl+E": Lang.cpp translates the label and keeps the shortcut
    $tab = $v.IndexOf("`t")
    $keys.Add($(if ($tab -gt 0) { $v.Substring(0, $tab) } else { $v })) | Out-Null
  }
}

function Resw($dir) {
  $map = @{}
  $path = Join-Path $files "$dir\Resources.resw"
  if (-not (Test-Path $path)) { return $map }
  foreach ($d in ([xml](Get-Content $path -Encoding UTF8)).root.data) { if ($d.value) { $map[$d.name] = [string]$d.value } }
  $map
}
# zh-Hans text -> Files resource name. Patterns with {0} / ICU plurals match too: Lang::Format fills them at run time.
$zh = Resw 'zh-Hans'
$byText = @{}
foreach ($k in ($zh.Keys | Sort-Object)) { $t = $zh[$k].Trim(); if ($t -and -not $byText.ContainsKey($t)) { $byText[$t] = $k } }

$manual = @{}
$manualPath = Join-Path $PSScriptRoot 'lang-en.txt'
if (Test-Path $manualPath) {
  foreach ($line in [IO.File]::ReadAllLines($manualPath, [Text.Encoding]::UTF8)) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $i = $line.IndexOf('=>'); if ($i -le 0) { continue }
    $manual[(Unescape $line.Substring(0, $i))] = Unescape $line.Substring($i + 2)
  }
}

if ($Missing) {
  # Literals with neither a Files match nor an English line
  foreach ($k in $keys) { if (-not $byText.ContainsKey($k.Trim()) -and -not $manual.ContainsKey($k)) { Escape $k } }
  return
}

$out = New-Object Text.StringBuilder
$langs = Get-ChildItem $files -Directory | Where-Object { $_.Name -ne 'zh-Hans' } | ForEach-Object Name
foreach ($lang in $langs) {
  $res = Resw $lang
  $lines = New-Object Collections.Generic.List[string]
  foreach ($k in $keys) {
    $v = $null
    if ($lang -eq 'en-US' -and $manual.ContainsKey($k)) {
      $v = $manual[$k]
      # Editors strip the line's trailing space, so carry the literal's trailing space over ("，共 " -> ", total ")
      if ($v -and $k.EndsWith(' ') -and -not $v.EndsWith(' ')) { $v += ' ' }
    }
    elseif ($byText.ContainsKey($k.Trim()) -and $res.ContainsKey($byText[$k.Trim()])) {
      $v = $res[$byText[$k.Trim()]]
      # Keep the literal's own surrounding spaces ("名称 " -> "Name ")
      $lead = $k.Length - $k.TrimStart().Length; $trail = $k.Length - $k.TrimEnd().Length
      $v = (' ' * $lead) + $v.Trim() + (' ' * $trail)
    }
    if ($v -and $v -ne $k) { $lines.Add((Escape $k) + "`t" + (Escape $v)) }
  }
  [void]$out.Append("[$lang]`n")
  foreach ($l in $lines) { [void]$out.Append($l).Append("`n") }
}
$dest = Join-Path $root 'res\lang.txt'
[IO.File]::WriteAllText($dest, $out.ToString(), (New-Object Text.UTF8Encoding $false))
"lang.txt: $($langs.Count) languages, $($keys.Count) strings, $([Math]::Round((Get-Item $dest).Length / 1KB)) KB"
