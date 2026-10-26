# Drives a running game without window focus: scripted pad input and frame dumps.
#   uo_pad.ps1 -Launch [-Exe <path to "Unicorn Overlord.exe">]   start the game wired to $Work
#   uo_pad.ps1 "cross,wait800,lx=0:1500,dump"                     send a sequence
# Tokens: name = press for -Hold ms; name:ms = hold ms (join with + to hold together, e.g.
# lx=0+r1:900); waitN = pause N ms; dump = frame dump, converted to png, path printed.
# Pad names: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad touchpad_left
# touchpad_right up down left right, sticks lx= ly= rx= ry= (0..255, 128 centre).
param([string]$Seq = '', [switch]$Launch, [string]$Exe = '', [string]$Work = '',
      [int]$Hold = 150, [int]$Gap = 350)
$port = Split-Path $PSScriptRoot -Parent
if (-not $Work) { $Work = Join-Path $port 'run_uo\drive' }
if (-not $Exe) { $Exe = Join-Path $port 'dist\Unicorn Overlord PC\Unicorn Overlord.exe' }
$pad = Join-Path $Work 'pad.txt'; $dumps = Join-Path $Work 'dumps'
New-Item -ItemType Directory -Force $dumps | Out-Null
function Set-Pad([string]$v) { [IO.File]::WriteAllText($pad, $v) }
if ($Launch) {
    Set-Pad ''
    $env:VW_PAD_FILE = $pad; $env:VW_FRAME_DUMP_TRIGGER = Join-Path $Work 'dump_now'
    $env:VW_DUMP_DIR = $dumps; $env:VW_UPDATES = '0'
    Start-Process $Exe | Out-Null
    "started $Exe (pad $pad)"
}
if (-not $Seq) { return }
foreach ($t in $Seq.Split(',')) {
    if ($t -match '^wait(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    if ($t -eq 'dump') {
        $before = @(Get-ChildItem $dumps -Filter *.raw).Count
        New-Item -ItemType File (Join-Path $Work 'dump_now') -Force | Out-Null
        for ($i = 0; $i -lt 40 -and @(Get-ChildItem $dumps -Filter *.raw).Count -le $before; $i++) { Start-Sleep -Milliseconds 150 }
        Start-Sleep -Milliseconds 300
        $f = Get-ChildItem $dumps -Filter *.raw | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($f) { python -I (Join-Path $port 'tools\raw2png.py') $f.FullName }
        continue
    }
    $ms = $Hold
    if ($t -match '^(.+):(\d+)$') { $t = $Matches[1]; $ms = [int]$Matches[2] }
    Set-Pad ($t -replace '\+', ' '); Start-Sleep -Milliseconds $ms; Set-Pad ''; Start-Sleep -Milliseconds $Gap
}
