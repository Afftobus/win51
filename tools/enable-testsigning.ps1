# Lets Windows load the test-signed Win51 driver. Run once, by hand, in an elevated PowerShell, then reboot.
# Undo with disable-testsigning.ps1.
# Secure Boot must be off. With BitLocker on, suspend it first or Windows asks for the recovery key.
$ErrorActionPreference = 'Stop'
$cer = "$PSScriptRoot\driver\win51-test.cer"
if (-not (Test-Path $cer)) { throw "$cer not found - run this from the bundle that tools\build.ps1 makes (out\win51)" }

bcdedit /set testsigning on
if ($LASTEXITCODE -ne 0) { throw 'bcdedit failed (is Secure Boot enabled?)' }

# Trusted Root: the signature chain is valid. Trusted Publishers: the driver installs without a prompt.
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Write-Host 'Test signing enabled and the Win51 test certificate is trusted. Reboot to apply.'
