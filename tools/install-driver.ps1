# Installs (or updates) the Win51 driver from driver\ next to this script (the bundle tools\build.ps1 makes).
# Needs test mode and the trusted test certificate: enable-testsigning.ps1, then a reboot.
$ErrorActionPreference = 'Stop'
$inf = "$PSScriptRoot\driver\win51.inf"
$devcon = "$PSScriptRoot\driver\devcon.exe"
$hwid = 'ROOT\Win51'

function Get-Win51Device { Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid } }

if (Get-Win51Device) { & $devcon update $inf $hwid } else { & $devcon install $inf $hwid }
# devcon: 0 = done, 1 = done but a reboot is required, anything else = failure
if ($LASTEXITCODE -gt 1) { throw "devcon failed with exit code $LASTEXITCODE" }
Get-Win51Device | Format-Table -AutoSize Status, Class, FriendlyName, InstanceId
