# Test-only: start a test instance, run the new-game wizard (German UI, 1280x720 window, coop_probe already enabled),
# use the test rules (tutorial off, unlimited coins, all territories owned) and found the village.
# Usage: newgame.ps1 -Instance A [-NoVillage]
param([string]$Instance = "A", [switch]$NoVillage, [string]$MapCode = "")
$ui = Join-Path $PSScriptRoot "ui.ps1"
$inst = "E:\FoundationCoopTest\instances\$Instance"
$id = [int](& "E:\FoundationCoopTest\bin\launch.exe" "E:\FoundationCoopTest\game\foundation.exe" $inst)
Set-Content "$inst\pid.txt" $id
"instance $Instance pid $id"
Start-Sleep 40
function Step($x, $y, $wait) { & $ui -GamePid $id -Action click -X $x -Y $y | Out-Null; Start-Sleep $wait }
Step 90 515 4      # Neues Spiel
Step 728 721 3     # -> Mods
Step 728 721 3     # -> Karte
Step 648 413 4     # achievements prompt (mods): Ja
if ($MapCode) {    # paste a fixed map code (the owner's clipboard is saved and restored)
  $saved = $null; try { $saved = Get-Clipboard -Raw -ErrorAction Stop } catch {}
  Set-Clipboard -Value $MapCode
  Step 457 648 2
  if ($null -ne $saved) { Set-Clipboard -Value $saved } else { Set-Clipboard -Value $null }
  & $ui -GamePid $id -Action shot -Out "E:\FoundationCoopTest\shots\mapcode-$Instance.png" | Out-Null
}
Step 728 721 3     # -> Regeln
Step 798 187 1     # tutorial: Aus
Step 868 274 1     # unlimited coins: Ein
Step 868 421 1     # all territories owned: Ein
Step 728 721 3     # -> Herausforderung
Step 648 418 3     # achievements prompt (custom rules): Ja
Step 728 721 3     # -> Dorf beschreiben
Step 728 721 60    # Spiel starten, wait for the map
if (-not $NoVillage) {
  Step 760 420 2   # wake the UI (hammer appears)
  Step 648 712 2   # build menu
  Step 374 578 2   # village centre
  Step 760 420 3   # place preview
  Step 605 721 15  # Bauen
  Step 1110 740 1  # close the controls help
}
& $ui -GamePid $id -Action shot -Out "E:\FoundationCoopTest\shots\newgame-$Instance.png" | Out-Null
"ready"
