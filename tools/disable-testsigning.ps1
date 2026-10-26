# Reverts enable-testsigning.ps1. Run in an elevated PowerShell, then reboot.
# Remove the service and the driver first (uninstall-service.ps1, uninstall-driver.ps1),
# otherwise the driver simply stops loading and the service waits for its endpoint forever.
bcdedit /set testsigning off
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq 'CN=Win51 Test Signing' } | Remove-Item
}
Write-Host 'Test signing disabled. Reboot to apply.'
