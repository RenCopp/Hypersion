# Game blunder analyzer.
# Phase 1: parse Hypersion's per-move eval from PGN comments, classify each loss.
# Phase 2: spot-check critical moves with Stockfish (depth 14, ~0.3s each).

param(
    [string[]]$PgnPaths = @(
        'C:\Engine\Hypersion\testing\gauntlet2_vs_sf2400.pgn',
        'C:\Engine\Hypersion\testing\gauntlet2_vs_obsidian.pgn',
        'C:\Engine\Hypersion\testing\gauntlet2_vs_alexandria.pgn',
        'C:\Engine\Hypersion\testing\gauntlet2_vs_rubichess.pgn'
    ),
    [string]$HyperName = 'Hyp_LMR',
    [string]$StockfishExe = 'C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe',
    [int]$SfDepth = 14,
    [switch]$SkipStockfish
)

function Parse-Game {
    param([string]$Text, [string]$HyperName)
    if (-not ($Text -match '\[Result "([^"]+)"\]')) { return $null }
    $result = $matches[1]
    $hyperWhite = $Text -match "\[White `"$HyperName`"\]"
    $hyperBlack = $Text -match "\[Black `"$HyperName`"\]"
    if (-not $hyperWhite -and -not $hyperBlack) { return $null }

    $hyperColor = if ($hyperWhite) { 'W' } else { 'B' }
    $outcome = switch ($result) {
        '1-0'      { if ($hyperWhite) { 'W' } else { 'L' } }
        '0-1'      { if ($hyperWhite) { 'L' } else { 'W' } }
        '1/2-1/2'  { 'D' }
        default    { '?' }
    }

    # PGN move section (after the headers)
    $moveSection = ($Text -split '(?m)^\s*$', 2)[-1]

    # Strip line breaks within the move section
    $moves = ($moveSection -replace '\r?\n', ' ').Trim()

    # Extract eval comments paired with the move number context.
    # Comment format: {<eval>/<depth> <time>s} where <eval> is +N.NN, -N.NN, or +M5/-M5
    # We pair each comment with the move that preceded it.
    # Pattern: SAN_move {comment}
    $token = '(?:[KQRBN]?[a-h]?[1-8]?x?[a-h][1-8](?:=[QRBN])?[+#]?|O-O(?:-O)?[+#]?)'
    $pattern = "($token)\s*\{([+\-]?M?\d+(?:\.\d+)?)/(\d+)\s+([\d.]+)s\}"

    $matchesAll = [regex]::Matches($moves, $pattern)
    $records = @()
    $moveIdx = 0
    foreach ($m in $matchesAll) {
        $san  = $m.Groups[1].Value
        $eval = $m.Groups[2].Value
        $dep  = [int]$m.Groups[3].Value
        $tim  = [double]$m.Groups[4].Value

        # Determine which side made this move: even index = White, odd = Black
        $sideMoved = if ($moveIdx % 2 -eq 0) { 'W' } else { 'B' }
        $isHypersion = ($sideMoved -eq $hyperColor)

        # Convert eval to centipawns. Cap mate flags at ±1500 cp so a
        # transition from +0.5 → +M5 doesn't look like a 30000-cp move.
        $evalCp = if ($eval -match '^([+\-])M(\d+)$') {
            $sign = if ($matches[1] -eq '+') { 1 } else { -1 }
            $sign * 1500
        } elseif ($eval -match '^[+\-]?\d+(\.\d+)?$') {
            [double]$eval * 100
        } else { $null }

        $records += [PSCustomObject]@{
            MoveIdx   = $moveIdx
            FullMove  = [int]([math]::Floor($moveIdx / 2)) + 1
            Side      = $sideMoved
            San       = $san
            EvalCp    = $evalCp
            Depth     = $dep
            Time      = $tim
            IsHypersion = $isHypersion
        }
        $moveIdx++
    }

    return [PSCustomObject]@{
        Outcome  = $outcome
        HyperColor = $hyperColor
        Records  = $records
        PlyCount = $records.Count
    }
}

function Classify-Loss {
    param($Game)
    $hyperEvals = $Game.Records | Where-Object { $_.IsHypersion -and $_.EvalCp -ne $null }
    if ($hyperEvals.Count -lt 2) { return @{Type='no-data'; CriticalMove=$null; MaxDrop=0} }

    # Compute consecutive eval drops (negative direction = Hypersion losing ground)
    $maxDrop = 0
    $criticalMove = $null
    for ($i = 1; $i -lt $hyperEvals.Count; $i++) {
        $delta = $hyperEvals[$i].EvalCp - $hyperEvals[$i-1].EvalCp
        if ($delta -lt $maxDrop) {
            $maxDrop = $delta
            $criticalMove = $hyperEvals[$i]
        }
    }

    # Classify based on max single-move drop
    $type = if ([math]::Abs($maxDrop) -ge 200) { 'tactical-blunder' }
            elseif ([math]::Abs($maxDrop) -ge 100) { 'inaccuracy' }
            else { 'gradual-drift' }

    # Compute total eval slope (start to last hyper eval)
    $startEval = $hyperEvals[0].EvalCp
    $endEval   = $hyperEvals[-1].EvalCp
    $totalDrop = $endEval - $startEval

    return @{
        Type = $type
        CriticalMove = $criticalMove
        MaxDrop = $maxDrop
        TotalDrop = $totalDrop
        StartEval = $startEval
        EndEval = $endEval
        HyperMoves = $hyperEvals.Count
    }
}

# ---- Main analysis loop ----
$allLosses = @()
foreach ($pgnPath in $PgnPaths) {
    if (-not (Test-Path $pgnPath)) { continue }
    $raw = Get-Content $pgnPath -Raw
    $gameTexts = $raw -split '(?m)(?=\[Event )'
    $oppName = (Split-Path -Leaf $pgnPath) -replace 'gauntlet2_(?:v1_)?vs_','' -replace '\.pgn',''
    foreach ($gtxt in $gameTexts) {
        if ($gtxt -notmatch '\[Event ') { continue }
        $game = Parse-Game -Text $gtxt -HyperName $HyperName
        if ($null -eq $game) { continue }
        if ($game.Outcome -ne 'L') { continue }
        $cls = Classify-Loss -Game $game
        $allLosses += [PSCustomObject]@{
            Opponent     = $oppName
            HyperColor   = $game.HyperColor
            PlyCount     = $game.PlyCount
            HyperMoves   = $cls.HyperMoves
            Type         = $cls.Type
            MaxDrop      = $cls.MaxDrop
            TotalDrop    = $cls.TotalDrop
            StartEval    = $cls.StartEval
            EndEval      = $cls.EndEval
            CriticalMoveNum = if ($cls.CriticalMove) { $cls.CriticalMove.FullMove } else { $null }
            CriticalSAN  = if ($cls.CriticalMove) { $cls.CriticalMove.San } else { '' }
            CriticalDepth = if ($cls.CriticalMove) { $cls.CriticalMove.Depth } else { 0 }
        }
    }
}

"=== $($allLosses.Count) Hypersion losses analyzed (HyperName=$HyperName) ==="
""
"--- Loss type distribution ---"
$allLosses | Group-Object Type | Sort-Object Count -Descending |
    ForEach-Object { "{0,-20} {1,3}" -f $_.Name, $_.Count } | Out-String
""
"--- Loss type by opponent ---"
$opponents = $allLosses | Select-Object -ExpandProperty Opponent -Unique | Sort-Object
foreach ($opp in $opponents) {
    $oppLosses = $allLosses | Where-Object { $_.Opponent -eq $opp }
    "{0,-12} ({1,2} losses)" -f $opp, $oppLosses.Count
    $oppLosses | Group-Object Type | Sort-Object Count -Descending |
        ForEach-Object { "  {0,-20} {1,3}" -f $_.Name, $_.Count } | Out-String -Stream
}
""
"--- Critical move number distribution (when did the worst move happen?) ---"
$buckets = @{ 'opening(1-15)'=0; 'middlegame(16-30)'=0; 'late-mg(31-45)'=0; 'endgame(46+)'=0 }
foreach ($l in $allLosses) {
    if ($l.CriticalMoveNum -eq $null) { continue }
    if ($l.CriticalMoveNum -le 15)      { $buckets['opening(1-15)']++ }
    elseif ($l.CriticalMoveNum -le 30)  { $buckets['middlegame(16-30)']++ }
    elseif ($l.CriticalMoveNum -le 45)  { $buckets['late-mg(31-45)']++ }
    else                                 { $buckets['endgame(46+)']++ }
}
$buckets.Keys | Sort-Object | ForEach-Object { "{0,-22} {1,3}" -f $_, $buckets[$_] } | Out-String
""
"--- Eval slope: how much did Hypersion's eval slip over the whole game? ---"
"(start = first hyper move eval; end = last hyper move eval; in centipawns)"
$slopeBuckets = @{ '<-500 (catastrophe)'=0; '-500..-200 (large)'=0; '-200..-50 (moderate)'=0; '-50..+50 (flat)'=0; '+50.. (already losing)'=0 }
foreach ($l in $allLosses) {
    $td = $l.TotalDrop
    if ($td -lt -500)        { $slopeBuckets['<-500 (catastrophe)']++ }
    elseif ($td -lt -200)    { $slopeBuckets['-500..-200 (large)']++ }
    elseif ($td -lt -50)     { $slopeBuckets['-200..-50 (moderate)']++ }
    elseif ($td -lt 50)      { $slopeBuckets['-50..+50 (flat)']++ }
    else                      { $slopeBuckets['+50.. (already losing)']++ }
}
$slopeBuckets.Keys | Sort-Object | ForEach-Object { "{0,-26} {1,3}" -f $_, $slopeBuckets[$_] } | Out-String
""
"--- Top 10 worst single-move drops ---"
$allLosses | Where-Object { $_.MaxDrop -lt 0 } | Sort-Object MaxDrop |
    Select-Object -First 10 |
    ForEach-Object { "  {0,-12} mv{1,3} {2,5} drop={3,5:0} cp  ({4})" -f $_.Opponent, $_.CriticalMoveNum, $_.CriticalSAN, $_.MaxDrop, $_.Type } | Out-String
