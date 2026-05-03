# Analyze Hypersion's losses across gauntlet PGNs to surface weakness patterns.

function Analyze-Pgn {
    param([string]$Path, [string]$HyperName)
    if (-not (Test-Path $Path)) { return }

    $raw = Get-Content $Path -Raw

    # Split on [Event tag - each game starts with one
    $gameTexts = ($raw -split '(?m)^\[Event ') | Where-Object { $_ -ne '' }

    $games = @()
    foreach ($gtxt in $gameTexts) {
        if (-not ($gtxt -match '\[Result "([^"]+)"\]')) { continue }
        $result = $matches[1]
        $white  = if ($gtxt -match '\[White "([^"]+)"\]')  { $matches[1] } else { '' }
        $black  = if ($gtxt -match '\[Black "([^"]+)"\]')  { $matches[1] } else { '' }
        $term   = if ($gtxt -match '\[Termination "([^"]+)"\]') { $matches[1] } else { 'unknown' }
        $plyCount = if ($gtxt -match '\[PlyCount "([^"]+)"\]')  { [int]$matches[1] } else { 0 }
        $reason = if ($gtxt -match '\{([^}]+)\}\s*(?:1-0|0-1|1/2-1/2|\*)\s*$') { $matches[1] } else { '' }

        # Hypersion's color and outcome
        $hyperIsWhite = $white -eq $HyperName
        $hyperIsBlack = $black -eq $HyperName
        if (-not $hyperIsWhite -and -not $hyperIsBlack) { continue }

        $hyperOutcome = switch ($result) {
            '1-0'      { if ($hyperIsWhite) { 'W' } else { 'L' } }
            '0-1'      { if ($hyperIsWhite) { 'L' } else { 'W' } }
            '1/2-1/2'  { 'D' }
            default    { '?' }
        }

        $games += [PSCustomObject]@{
            Outcome = $hyperOutcome
            PlyCount = $plyCount
            Termination = $term
            Reason = $reason.Trim()
            Color = if ($hyperIsWhite) { 'White' } else { 'Black' }
        }
    }
    return $games
}

# Combine all gauntlet matches (Hypersion as challenger)
$all = @()
$lmrFiles = @(
    @{Path='C:\Engine\Hypersion\testing\gauntlet2_vs_sf2400.pgn';     Name='Hyp_LMR'; Opp='SF2400'},
    @{Path='C:\Engine\Hypersion\testing\gauntlet2_vs_obsidian.pgn';   Name='Hyp_LMR'; Opp='Obsidian'},
    @{Path='C:\Engine\Hypersion\testing\gauntlet2_vs_alexandria.pgn'; Name='Hyp_LMR'; Opp='Alexandria'},
    @{Path='C:\Engine\Hypersion\testing\gauntlet2_vs_rubichess.pgn';  Name='Hyp_LMR'; Opp='RubiChess'}
)
$v1Files = @(
    @{Path='C:\Engine\Hypersion\testing\gauntlet2_v1_vs_sf2400.pgn';     Name='Hyp_V1'; Opp='SF2400'},
    @{Path='C:\Engine\Hypersion\testing\gauntlet2_v1_vs_obsidian.pgn';   Name='Hyp_V1'; Opp='Obsidian'},
    @{Path='C:\Engine\Hypersion\testing\gauntlet2_v1_vs_alexandria.pgn'; Name='Hyp_V1'; Opp='Alexandria'},
    @{Path='C:\Engine\Hypersion\testing\gauntlet2_v1_vs_rubichess.pgn';  Name='Hyp_V1'; Opp='RubiChess'}
)

foreach ($f in ($lmrFiles + $v1Files)) {
    $games = Analyze-Pgn -Path $f.Path -HyperName $f.Name
    foreach ($g in $games) {
        $g | Add-Member -NotePropertyName 'Opponent' -NotePropertyValue $f.Opp
        $g | Add-Member -NotePropertyName 'HyperVer' -NotePropertyValue $f.Name
        $all += $g
    }
}

"=== Total games analyzed: $($all.Count) ==="
""

"=== Outcome × Opponent ==="
$all | Group-Object Opponent, Outcome |
    Sort-Object @{Expression='Name'} |
    ForEach-Object { "{0,-22} {1,3}" -f $_.Name, $_.Count } | Out-String

""
"=== Termination types in losses (Hypersion lost) ==="
$losses = $all | Where-Object { $_.Outcome -eq 'L' }
"Total losses: $($losses.Count)"
$losses | Group-Object Termination | Sort-Object Count -Descending |
    ForEach-Object { "{0,-30} {1,3}" -f $_.Name, $_.Count } | Out-String

""
"=== Loss reasons (parsed from PGN comment, top 10) ==="
$losses | Group-Object Reason | Sort-Object Count -Descending | Select-Object -First 12 |
    ForEach-Object { "{0,3}  {1}" -f $_.Count, ($_.Name -replace '\s+', ' ' | ForEach-Object { $_.Substring(0, [math]::Min(70, $_.Length)) }) } | Out-String

""
"=== Game length (PlyCount) by outcome ==="
foreach ($outcome in 'W', 'D', 'L') {
    $set = $all | Where-Object { $_.Outcome -eq $outcome -and $_.PlyCount -gt 0 }
    if ($set.Count -eq 0) { continue }
    $plies = $set | ForEach-Object { $_.PlyCount } | Sort-Object
    $median = $plies[[int]($plies.Count / 2)]
    $min = $plies[0]
    $max = $plies[-1]
    "{0}  count={1,3}  ply median={2,3}  min={3}  max={4}" -f $outcome, $set.Count, $median, $min, $max
}

""
"=== Loss length distribution (Hypersion lost) ==="
$buckets = @{ 'short(<40)'=0; 'mid(40-80)'=0; 'long(80-120)'=0; 'verylong(120+)'=0 }
foreach ($l in $losses) {
    if ($l.PlyCount -lt 40)        { $buckets['short(<40)']++ }
    elseif ($l.PlyCount -lt 80)    { $buckets['mid(40-80)']++ }
    elseif ($l.PlyCount -lt 120)   { $buckets['long(80-120)']++ }
    else                            { $buckets['verylong(120+)']++ }
}
$buckets.Keys | Sort-Object | ForEach-Object { "{0,-18} {1,3}" -f $_, $buckets[$_] } | Out-String

""
"=== Per-opponent loss length median ==="
foreach ($opp in 'SF2400', 'Obsidian', 'Alexandria', 'RubiChess') {
    foreach ($ver in 'Hyp_LMR', 'Hyp_V1') {
        $set = $losses | Where-Object { $_.Opponent -eq $opp -and $_.HyperVer -eq $ver -and $_.PlyCount -gt 0 }
        if ($set.Count -eq 0) { continue }
        $plies = $set | ForEach-Object { $_.PlyCount } | Sort-Object
        $median = $plies[[int]($plies.Count / 2)]
        "{0,-12} {1,-9} losses={2,3}  median ply={3,4}" -f $opp, $ver, $set.Count, $median
    }
}
