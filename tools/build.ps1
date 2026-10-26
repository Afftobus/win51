# Builds the Win51 driver and service with the Enterprise WDK, test-signs the driver and assembles a
# self-contained install bundle in out\win51 (copy that folder to any PC to install).
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Ewdk <iso|dir>] [-Ffmpeg <dir>]
#
# Run elevated: mounting the EWDK ISO and the machine certificate store need administrator rights.
# By default the EWDK ISO and the FFmpeg build are taken from deps\ next to tools\.
param(
    [ValidateSet('Release', 'Debug')] [string]$Configuration = 'Release',
    [string]$Ewdk,          # EWDK .iso (mounted on demand) or the root of a mounted/extracted EWDK
    [string]$Ffmpeg,        # FFmpeg "lgpl-shared" build for win64 (include\, lib\, bin\)
    [string]$OutDir,        # default: out\ in the repository
    [switch]$DriverOnly,
    [switch]$ServiceOnly
)
$ErrorActionPreference = 'Stop'
$src = Split-Path $PSScriptRoot
$deps = "$src\deps"
$out = if ($OutDir) { $OutDir } else { "$src\out" }
$bundle = "$out\win51"
New-Item -ItemType Directory -Force $out | Out-Null

# --- Enterprise WDK: a mounted ISO stays mounted until reboot.
if (-not $Ewdk) {
    $Ewdk = (Get-ChildItem "$deps\*.iso" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    if (-not $Ewdk) { throw "No EWDK: put the ISO into $deps or pass -Ewdk" }
}
if ($Ewdk -like '*.iso') {
    $image = Get-DiskImage -ImagePath (Resolve-Path $Ewdk).Path
    if (-not $image.Attached) { $image = Mount-DiskImage -ImagePath $image.ImagePath -PassThru }
    $Ewdk = (($image | Get-Volume).DriveLetter) + ':'
}
$setupEnv = "$Ewdk\BuildEnv\SetupBuildEnv.cmd"
if (-not (Test-Path $setupEnv)) { throw "Unexpected EWDK layout: $setupEnv not found" }

function Invoke-Ewdk([string]$command) {
    cmd /c "call `"$setupEnv`" amd64 >nul 2>&1 && $command"
    if ($LASTEXITCODE -ne 0) { throw "Failed ($LASTEXITCODE): $command" }
}

if (-not $ServiceOnly) {
    # --- Test-signing certificate, created once per build machine. Kept in the machine store: key-based SSH
    # logons cannot use the user's DPAPI keys. Targets trust it through enable-testsigning.ps1.
    $certSubject = 'CN=Win51 Test Signing'
    $cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $certSubject } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certSubject -CertStoreLocation Cert:\LocalMachine\My `
            -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
    }

    $driverOut = "$out\driver\"
    Invoke-Ewdk "msbuild `"$src\driver\win51.vcxproj`" /m /nologo /v:minimal /p:Configuration=$Configuration /p:Platform=x64 /p:OutDir=$driverOut /p:IntDir=$out\obj\driver\"

    $package = "$bundle\driver"
    Remove-Item -Recurse -Force $package -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $package | Out-Null
    Copy-Item "$driverOut\win51.sys", "$driverOut\win51.inf" $package
    Invoke-Ewdk "signtool sign /q /fd sha256 /sm /s My /sha1 $($cert.Thumbprint) `"$package\win51.sys`""
    # Inf2Cat is not on the EWDK PATH; it lives next to the other kit tools.
    $inf2cat = Get-ChildItem "$Ewdk\Program Files\Windows Kits\10\bin\*\x86\Inf2Cat.exe" | Select-Object -First 1
    Invoke-Ewdk "`"$($inf2cat.FullName)`" /driver:`"$package`" /os:10_VB_X64,10_CO_X64 /uselocaltime"
    Invoke-Ewdk "signtool sign /q /fd sha256 /sm /s My /sha1 $($cert.Thumbprint) `"$package\win51.cat`""
    # Added after Inf2Cat, outside the catalog: the certificate the target has to trust, and devcon, which
    # creates the root-enumerated device node (pnputil cannot do that on Windows 10).
    Export-Certificate -Cert $cert -FilePath "$package\win51-test.cer" | Out-Null
    $devcon = Get-ChildItem "$Ewdk\Program Files\Windows Kits\10\Tools\*\x64\devcon.exe" | Select-Object -First 1
    Copy-Item $devcon.FullName $package
    Write-Host "Driver: $package"
}

if (-not $DriverOnly) {
    if (-not $Ffmpeg) {
        $Ffmpeg = (Get-ChildItem $deps -Directory -Filter 'ffmpeg-*' -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
        if (-not $Ffmpeg) { throw "No FFmpeg: put an lgpl-shared build into $deps or pass -Ffmpeg" }
    }
    $serviceOut = "$out\service\"
    Invoke-Ewdk "msbuild `"$src\service\win51svc.vcxproj`" /m /nologo /v:minimal /p:Configuration=$Configuration /p:Platform=x64 /p:OutDir=$serviceOut /p:IntDir=$out\obj\service\ /p:FfmpegDir=`"$Ffmpeg`""

    # Only the libraries win51svc loads (avcodec needs swresample), plus FFmpeg's license for the DLLs.
    $service = "$bundle\service"
    Remove-Item -Recurse -Force $service -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $service | Out-Null
    Copy-Item "$serviceOut\win51svc.exe" $service
    foreach ($lib in 'avcodec', 'avutil', 'swresample') { Copy-Item "$serviceOut\$lib-*.dll" $service }
    Copy-Item "$Ffmpeg\LICENSE.txt" "$service\FFMPEG-LICENSE.txt"
    Write-Host "Service: $service"
}

# Install scripts and license texts go to the bundle root.
Copy-Item "$PSScriptRoot\enable-testsigning.ps1", "$PSScriptRoot\disable-testsigning.ps1", "$PSScriptRoot\install-driver.ps1",
    "$PSScriptRoot\uninstall-driver.ps1", "$PSScriptRoot\install-service.ps1", "$PSScriptRoot\uninstall-service.ps1" $bundle
Copy-Item "$src\LICENSE" "$bundle\LICENSE.txt"
Copy-Item "$src\driver\LICENSE-MS-PL.txt" $bundle
Write-Host "Install bundle: $bundle"
