# Installs, updates or removes Win51. Run it elevated from the install bundle (out\win51) or from
# C:\Program Files\Win51, where the installer (Win51-Setup-*.exe) puts it and runs it:
#
#   setup.ps1                     install or update: test mode, certificate, driver, service, tray icon
#   setup.ps1 -Out "S/PDIF"       ... and use this output instead of the automatic HDMI/DisplayPort choice
#   setup.ps1 -Uninstall          remove everything; turns test mode off if setup turned it on
#   setup.ps1 -Check              exit code only: 0 = can install, 2 = Secure Boot is on, 3 = BitLocker needs
#                                 to be suspended (pass -SuspendBitLocker to allow that)
#   setup.ps1 -Stop               stop the service and the tray icon (the installer does this before copying)
#
# Exit codes: 0 = done, 3010 = done, restart Windows to finish, 1 = failed. Log: C:\ProgramData\Win51\setup.log
param(
    [switch]$Uninstall,
    [switch]$Check,
    [switch]$Stop,
    [string]$Out,
    [switch]$SuspendBitLocker
)
$ErrorActionPreference = 'Stop'

# A 32-bit PowerShell (e.g. started by a 32-bit installer) sees neither bcdedit nor pnputil, and its HKLM\SOFTWARE
# is the WOW6432Node view the 64-bit service does not read: run again as 64-bit.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
    foreach ($name in 'Uninstall', 'Check', 'Stop', 'SuspendBitLocker') {
        if ($PSBoundParameters[$name]) { $arguments += "-$name" }
    }
    if ($PSBoundParameters.ContainsKey('Out')) { $arguments += '-Out', $Out }
    & "$env:windir\Sysnative\WindowsPowerShell\v1.0\powershell.exe" $arguments
    exit $LASTEXITCODE
}

$source = (Resolve-Path $PSScriptRoot).Path
Set-Location $source   # DISM (Get-WindowsDriver) fails if started with a working directory it cannot open
$target = Join-Path $env:ProgramFiles 'Win51'
$data = Join-Path $env:ProgramData 'Win51'
$settingsKey = 'HKLM:\SOFTWARE\Win51'
$runKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$shortcut = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Win51.lnk'
$serviceName = 'win51svc'   # the driver's own kernel service is called Win51
$fromTarget = $source -eq $target

function Test-TestSigningActive {
    (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions -match 'TESTSIGNING'
}
function Test-TestSigningConfigured { [bool]((bcdedit /enum '{current}') -match '^testsigning\s+Yes') }
function Test-SecureBoot {
    (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -ErrorAction SilentlyContinue).UEFISecureBootEnabled -eq 1
}
function Test-BitLocker {
    $volume = Get-CimInstance -Namespace 'root/cimv2/security/microsoftvolumeencryption' -ClassName Win32_EncryptableVolume `
        -Filter "DriveLetter='$env:SystemDrive'" -ErrorAction SilentlyContinue
    $null -ne $volume -and $volume.ProtectionStatus -eq 1
}

# What enabling test mode needs: 0 = nothing in the way, 2 = Secure Boot is on, 3 = BitLocker is on.
function Get-TestSigningObstacle {
    if (Test-TestSigningActive) { return 0 }
    if (Test-SecureBoot) { return 2 }
    if (-not (Test-TestSigningConfigured) -and (Test-BitLocker)) { return 3 }
    return 0
}

function Stop-Win51 {
    Get-Process win51tray -ErrorAction SilentlyContinue | Stop-Process -Force
    if (Get-Service $serviceName -ErrorAction SilentlyContinue) { Stop-Service $serviceName }
    # A copy started by hand ("win51svc run") would keep the HDMI output and the files busy.
    Get-Process win51svc -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }
}

# Runs win51svc.exe; returns $true when Windows has to be restarted. Its output goes to the console only,
# not into the function's return value.
function Invoke-Win51Tool([string[]]$arguments) {
    & (Join-Path $target 'win51svc.exe') $arguments | Out-Host
    if ($LASTEXITCODE -eq 3010) { return $true }
    if ($LASTEXITCODE -ne 0) { throw "win51svc $($arguments -join ' ') failed ($LASTEXITCODE)" }
    return $false
}

function Install-Win51 {
    $restart = $false
    if ([Environment]::OSVersion.Version.Build -lt 19041 -or -not [Environment]::Is64BitOperatingSystem) {
        throw 'Win51 needs 64-bit Windows 10 2004 (build 19041) or newer.'
    }

    # 1. Test mode: the driver is test-signed.
    switch (Get-TestSigningObstacle) {
        2 { throw 'Secure Boot is on. Turn it off in the UEFI settings and run setup again.' }
        3 {
            if (-not $SuspendBitLocker) {
                throw 'BitLocker protects the system drive: changing the boot settings would make Windows ask for the recovery key. Run setup with -SuspendBitLocker to suspend it for one restart.'
            }
            manage-bde -protectors -disable $env:SystemDrive -RebootCount 1 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "manage-bde failed ($LASTEXITCODE)" }
            Write-Host 'BitLocker suspended for one restart'
        }
    }
    if (-not (Test-TestSigningConfigured)) {
        bcdedit /set testsigning on | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "bcdedit /set testsigning on failed ($LASTEXITCODE)" }
        New-Item -Force $settingsKey | Out-Null
        Set-ItemProperty $settingsKey TestSigningBySetup 1 -Type DWord   # uninstall turns it off again
        Write-Host 'Test mode turned on'
    }
    if (-not (Test-TestSigningActive)) { $restart = $true }

    # 2. Trust the certificate the driver is signed with.
    $cer = Join-Path $source 'driver\win51-test.cer'
    $thumbprint = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer).Thumbprint
    foreach ($store in 'Root', 'TrustedPublisher') {
        if (-not (Test-Path "Cert:\LocalMachine\$store\$thumbprint")) {
            Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
            Write-Host "Certificate added to $store"
        }
    }

    # 3. Files.
    Stop-Win51
    if (-not $fromTarget) {
        New-Item -ItemType Directory -Force $target | Out-Null
        Copy-Item "$source\*" $target -Recurse -Force
    }

    # 4. Driver: create or update the device, then drop older packages of it from the driver store, which
    # would otherwise outrank this one if the device is ever reinstalled. Windows keeps the package it already
    # has for the same DriverVer, so the same version is not installed again (that would only ask for a restart).
    $inf = Join-Path $target 'driver\win51.inf'
    $version = (Select-String -Path $inf -Pattern '^DriverVer\s*=\s*[^,]+,\s*([\d.]+)').Matches[0].Groups[1].Value
    $device = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'ROOT\Win51' } |
        Select-Object -First 1
    $installed = if ($device) { ($device | Get-PnpDeviceProperty -KeyName DEVPKEY_Device_DriverVersion).Data }
    if ($device -and $device.Status -eq 'OK' -and $installed -eq $version) {
        Write-Host "Driver $version is already installed"
    } elseif (Invoke-Win51Tool @('driver-install', $inf)) {
        $restart = $true
    }
    $current = (Get-CimInstance Win32_PnPSignedDriver -Filter "HardWareID='ROOT\\Win51'" -ErrorAction SilentlyContinue).InfName
    if ($current) {
        Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\win51.inf' -and $_.Driver -ne $current } |
            ForEach-Object { pnputil /delete-driver $_.Driver | Out-Null }
    }

    # 5. Service: starts with Windows, restarts after a failure (in 2 s, 5 s, then every 10 s).
    $binPath = "`"$target\win51svc.exe`" service --log `"$data\win51svc.log`""
    if (Get-Service $serviceName -ErrorAction SilentlyContinue) {
        Set-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName" ImagePath $binPath
    } else {
        New-Service -Name $serviceName -BinaryPathName $binPath -DisplayName 'Win51 AC3 encoder' -StartupType Automatic `
            -DependsOn Audiosrv -Description 'Encodes the Win51 AC3 5.1 output to AC3 and plays it to HDMI.' | Out-Null
    }
    sc.exe failure $serviceName reset= 86400 actions= restart/2000/restart/5000/restart/10000 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe failure: $LASTEXITCODE" }
    sc.exe failureflag $serviceName 1 | Out-Null
    if ($PSBoundParameters.ContainsKey('Out')) {
        New-Item -Force $settingsKey | Out-Null
        Set-ItemProperty $settingsKey Output $Out
    }
    Start-Service $serviceName

    # 6. Tray icon: starts at sign-in for every user, and from the Start menu.
    $tray = Join-Path $target 'win51tray.exe'
    Set-ItemProperty $runKey Win51Tray "`"$tray`""
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    $link.TargetPath = $tray
    $link.Description = 'Win51 AC3 5.1'
    $link.Save()
    # Start it now in this desktop session, unelevated, through the shell (there is none over SSH).
    $session = (Get-Process -Id $PID).SessionId
    if (Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session) {
        Start-Process explorer.exe "`"$tray`""
    }
    return $restart
}

function Uninstall-Win51 {
    $restart = $false
    Remove-ItemProperty $runKey Win51Tray -ErrorAction SilentlyContinue
    Remove-Item $shortcut -ErrorAction SilentlyContinue
    Stop-Win51
    if (Get-Service $serviceName -ErrorAction SilentlyContinue) {
        sc.exe delete $serviceName | Out-Null
    }

    if (Test-Path (Join-Path $target 'win51svc.exe')) {
        if (Invoke-Win51Tool @('driver-remove')) { $restart = $true }
    }
    Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\win51.inf' } |
        ForEach-Object { pnputil /delete-driver $_.Driver /uninstall /force | Out-Null }

    $cer = @((Join-Path $target 'driver\win51-test.cer'), (Join-Path $source 'driver\win51-test.cer')) |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($cer) {
        $thumbprint = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer).Thumbprint
        foreach ($store in 'Root', 'TrustedPublisher') { Remove-Item "Cert:\LocalMachine\$store\$thumbprint" -ErrorAction SilentlyContinue }
    }

    if ((Get-ItemProperty $settingsKey -ErrorAction SilentlyContinue).TestSigningBySetup -eq 1) {
        bcdedit /set testsigning off | Out-Null
        if ($LASTEXITCODE -eq 0) { $restart = $true; Write-Host 'Test mode turned off' }
    }
    Remove-Item $settingsKey -Recurse -ErrorAction SilentlyContinue

    # The installer removes its own files; a manual install is removed here (the log in ProgramData stays).
    if (-not $fromTarget) { Remove-Item $target -Recurse -Force -ErrorAction SilentlyContinue }
    return $restart
}

if ($Check) { exit (Get-TestSigningObstacle) }
if ($Stop) { Stop-Win51; exit 0 }

New-Item -ItemType Directory -Force $data | Out-Null
Start-Transcript -Append -Path (Join-Path $data 'setup.log') | Out-Null
try {
    if ($Uninstall) { $restart = Uninstall-Win51 } else { $restart = Install-Win51 }
    if ($restart) { Write-Host 'Done. Restart Windows to finish.' } else { Write-Host 'Done.' }
    Stop-Transcript | Out-Null
    if ($restart) { exit 3010 }
    exit 0
} catch {
    Write-Host "FAILED: $_"
    Stop-Transcript | Out-Null
    exit 1
}
