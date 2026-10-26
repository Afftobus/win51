# Installs (or updates) win51svc as an auto-start Windows service, so the Win51 -> AC3 -> HDMI pipeline
# runs from boot and is restarted if it ever fails. Run as administrator from the bundle (out\win51).
#   -Out <name>   output endpoint (name fragment, see "win51svc list"); by default the HDMI/DisplayPort
#                 output that accepts AC3. Needed for S/PDIF or when several such outputs exist.
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$name = 'win51svc'                      # the driver's own service is called Win51
$dir = "$env:ProgramFiles\Win51"        # LocalSystem runs it: only administrators may change these files
$log = "$env:ProgramData\Win51\win51svc.log"
$binPath = "`"$dir\win51svc.exe`" service --log `"$log`""
if ($Out) { $binPath += " --out `"$Out`"" }

$service = Get-Service $name -ErrorAction SilentlyContinue
if ($service) { Stop-Service $name }
# A copy started by hand ("win51svc run") would keep the HDMI output busy.
Get-Process win51svc -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }

New-Item -ItemType Directory -Force $dir, (Split-Path $log) | Out-Null
Copy-Item "$PSScriptRoot\service\*" $dir

if ($service) {
    Set-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$name" ImagePath $binPath
} else {
    New-Service -Name $name -BinaryPathName $binPath -DisplayName 'Win51 AC3 encoder' -StartupType Automatic `
        -DependsOn Audiosrv -Description 'Encodes the Win51 AC3 5.1 output to AC3 and plays it to HDMI.' | Out-Null
}
# Restart after a crash or an error exit: in 2 s, 5 s, then every 10 s; the count resets after a day.
sc.exe failure $name reset= 86400 actions= restart/2000/restart/5000/restart/10000 | Out-Null
if ($LASTEXITCODE) { throw "sc.exe failure: $LASTEXITCODE" }
sc.exe failureflag $name 1 | Out-Null
if ($LASTEXITCODE) { throw "sc.exe failureflag: $LASTEXITCODE" }

Start-Service $name
Get-Service $name | Format-Table -AutoSize Status, StartType, Name, DisplayName
Write-Host "Log: $log"
