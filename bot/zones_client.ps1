# Scenario step: only the client paints a residential zone (same spot the host uses in zones_host.ps1).
& (Join-Path $PSScriptRoot "zones.ps1") -ClientOnly
