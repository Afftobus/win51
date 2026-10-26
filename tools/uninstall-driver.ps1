# Removes the Win51 device and deletes its driver package from the driver store.
# Remove the service first (uninstall-service.ps1): without the driver it would wait for its endpoint forever.
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot\driver\devcon.exe" remove 'ROOT\Win51'
Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\win51.inf' } | ForEach-Object {
    pnputil /delete-driver $_.Driver /uninstall /force
}
