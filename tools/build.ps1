# Builds the Win51 driver, service and tray icon with the Enterprise WDK, test-signs the driver and assembles
#   out\win51\                    the install bundle (setup.ps1 installs from it)
#   out\Win51-<version>-x64.zip   the same bundle as an archive
#   out\Win51-Setup-<version>.exe the installer, when Inno Setup is available
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Ewdk <iso|dir>] [-Ffmpeg <dir>] [-InnoSetup <ISCC.exe>]
#
# Run elevated: mounting the EWDK ISO and the machine certificate store need administrator rights.
# By default the EWDK ISO, the FFmpeg build and Inno Setup are taken from deps\ next to tools\.
param(
    [ValidateSet('Release', 'Debug')] [string]$Configuration = 'Release',
    [string]$Ewdk,          # EWDK .iso (mounted on demand) or the root of a mounted/extracted EWDK
    [string]$Ffmpeg,        # FFmpeg "lgpl-shared" build for win64 (include\, lib\, bin\)
    [string]$InnoSetup,     # ISCC.exe of Inno Setup 6.3+; default: deps\innosetup or the installed one
    [string]$OutDir         # default: out\ in the repository
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Compress-Archive's progress bar fails without a console
$src = Split-Path $PSScriptRoot
$deps = "$src\deps"
$out = if ($OutDir) { $OutDir } else { "$src\out" }
$bundle = "$out\win51"
$props = [xml](Get-Content "$src\Directory.Build.props")
$group = $props.Project.PropertyGroup
$version = "$($group.Win51VersionMajor).$($group.Win51VersionMinor).$($group.Win51VersionPatch)"
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
if (-not $Ffmpeg) {
    $Ffmpeg = (Get-ChildItem $deps -Directory -Filter 'ffmpeg-*' -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    if (-not $Ffmpeg) { throw "No FFmpeg: put an lgpl-shared build into $deps or pass -Ffmpeg" }
}

function Invoke-Ewdk([string]$command) {
    cmd /c "call `"$setupEnv`" amd64 >nul 2>&1 && $command"
    if ($LASTEXITCODE -ne 0) { throw "Failed ($LASTEXITCODE): $command" }
}
function Invoke-MsBuild([string]$project, [string]$name, [string]$extra = '') {
    Invoke-Ewdk "msbuild `"$src\$project`" /m /nologo /v:minimal /p:Configuration=$Configuration /p:Platform=x64 /p:OutDir=$out\$name\ /p:IntDir=$out\obj\$name\ $extra"
}

Remove-Item -Recurse -Force $bundle -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$bundle\driver" | Out-Null

# --- Driver, signed with the test certificate. The certificate is created once per build machine and kept in
# the machine store: key-based SSH logons cannot use the user's DPAPI keys.
$certSubject = 'CN=Win51 Test Signing'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $certSubject } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certSubject -CertStoreLocation Cert:\LocalMachine\My `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
}
Invoke-MsBuild 'driver\win51.vcxproj' 'driver'
$package = "$bundle\driver"
Copy-Item "$out\driver\win51.sys", "$out\driver\win51.inf" $package
Invoke-Ewdk "signtool sign /q /fd sha256 /sm /s My /sha1 $($cert.Thumbprint) `"$package\win51.sys`""
# Inf2Cat is not on the EWDK PATH; it lives next to the other kit tools.
$inf2cat = Get-ChildItem "$Ewdk\Program Files\Windows Kits\10\bin\*\x86\Inf2Cat.exe" | Select-Object -First 1
Invoke-Ewdk "`"$($inf2cat.FullName)`" /driver:`"$package`" /os:10_VB_X64,10_CO_X64 /uselocaltime"
Invoke-Ewdk "signtool sign /q /fd sha256 /sm /s My /sha1 $($cert.Thumbprint) `"$package\win51.cat`""
# Added after Inf2Cat, outside the catalog: the certificate the target has to trust.
Export-Certificate -Cert $cert -FilePath "$package\win51-test.cer" | Out-Null

# --- Service (with the FFmpeg libraries it loads; avcodec needs swresample) and tray icon.
Invoke-MsBuild 'service\win51svc.vcxproj' 'service' "/p:FfmpegDir=`"$Ffmpeg`""
Invoke-MsBuild 'tray\win51tray.vcxproj' 'tray'
Copy-Item "$out\service\win51svc.exe", "$out\tray\win51tray.exe" $bundle
foreach ($lib in 'avcodec', 'avutil', 'swresample') { Copy-Item "$out\service\$lib-*.dll" $bundle }
Copy-Item "$PSScriptRoot\setup.ps1" $bundle
Copy-Item "$src\LICENSE" "$bundle\LICENSE.txt"
Copy-Item "$src\driver\LICENSE-MS-PL.txt" $bundle
Copy-Item "$Ffmpeg\LICENSE.txt" "$bundle\FFMPEG-LICENSE.txt"
Write-Host "Install bundle: $bundle"

$zip = "$out\Win51-$version-x64.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$bundle\*" -DestinationPath $zip
Write-Host "Archive: $zip"

# --- Installer.
if (-not $InnoSetup) {
    $InnoSetup = @("$deps\innosetup\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
                   "$env:ProgramFiles\Inno Setup 7\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if ($InnoSetup) {
    & $InnoSetup /Q "/DVersion=$version" "/DBundleDir=$bundle" "/O$out" "$src\installer\win51.iss"
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
    Write-Host "Installer: $out\Win51-Setup-$version.exe"
} else {
    Write-Host 'Inno Setup not found: no installer (put it into deps\innosetup or pass -InnoSetup)'
}
