# Scenario step: the host builds a woodcutter's camp, then the client builds one.
& (Join-Path $PSScriptRoot "host_lumber.ps1")
& (Join-Path $PSScriptRoot "client_lumber.ps1") -PopupWait 3
