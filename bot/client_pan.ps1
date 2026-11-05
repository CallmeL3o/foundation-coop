# Scenario step: the client moves its camera far away (W and A held via repeated key presses); no tools.
$ui = Join-Path $PSScriptRoot "ui.ps1"
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
Start-Sleep 30
for ($i = 0; $i -lt 60; $i++) { & $ui -GamePid $idB -Action key -Key 0x57 | Out-Null; & $ui -GamePid $idB -Action key -Key 0x41 | Out-Null }
& $ui -GamePid $idB -Action shot -Out "E:\FoundationCoopTest\shots\pan-B.png" | Out-Null
"client_pan done"
