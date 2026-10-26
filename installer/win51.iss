; Win51 installer, Inno Setup 7. tools\build.ps1 compiles it from the install bundle:
;   ISCC.exe /DVersion=1.0.0 /DBundleDir=<repo>\out\win51 /O<repo>\out installer\win51.iss
; All the real work is done by setup.ps1 from the bundle, the same script a manual install runs.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef BundleDir
  #define BundleDir "..\out\win51"
#endif

[Setup]
AppId={{D3F5B7A2-6C41-4E8B-9F1D-2A7C5E9B0D14}
AppName=Win51
AppVersion={#Version}
AppVerName=Win51 {#Version}
AppPublisher=Win51
AppPublisherURL=https://github.com/Afftobus/win51
; setup.ps1 and the service assume %ProgramFiles%\Win51.
DefaultDirName={autopf}\Win51
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
PrivilegesRequired=admin
OutputBaseFilename=Win51-Setup-{#Version}
SetupIconFile=..\tray\res\ok.ico
UninstallDisplayIcon={app}\win51tray.exe
LicenseFile=..\LICENSE
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; setup.ps1 -Stop stops the service and the tray icon before the files are replaced.
CloseApplications=no
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "info-en.txt"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"; InfoBeforeFile: "info-ru.txt"

[Files]
Source: "{#BundleDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
Source: "{#BundleDir}\setup.ps1"; Flags: dontcopy

[CustomMessages]
en.SecureBootOn=Secure Boot is on.%n%nThe Win51 driver is test-signed, and Windows loads such drivers only in test mode, which Secure Boot does not allow. Turn Secure Boot off in the UEFI (BIOS) settings and run setup again.
ru.SecureBootOn=Включён Secure Boot.%n%nДрайвер Win51 подписан тестовым сертификатом, а такие драйверы Windows загружает только в тестовом режиме, который несовместим с Secure Boot. Выключите Secure Boot в настройках UEFI (BIOS) и запустите установку снова.
en.BitLockerOn=BitLocker protects the system drive.%n%nTurning on test mode changes the boot settings, and on the next start Windows would ask for the BitLocker recovery key. Suspend BitLocker for one restart (recommended)?%n%nYes: suspend and continue. No: continue without suspending. Cancel: quit setup.
ru.BitLockerOn=Системный диск защищён BitLocker.%n%nВключение тестового режима меняет параметры загрузки, и при следующем запуске Windows спросит ключ восстановления BitLocker. Приостановить BitLocker на одну перезагрузку (рекомендуется)?%n%nДа — приостановить и продолжить. Нет — продолжить без этого. Отмена — выйти.
en.Configuring=Installing the driver and the service...
ru.Configuring=Установка драйвера и службы…
en.SetupFailed=Win51 could not be set up (code %1).%n%nDetails are in %2.
ru.SetupFailed=Не удалось установить Win51 (код %1).%n%nПодробности — в %2.

[Code]
var
  SuspendBitLocker: Boolean;
  RestartNeeded: Boolean;
  UninstallRestartNeeded: Boolean;

function RunSetupScript(const Script, Params: String): Integer;
begin
  { The 64-bit PowerShell: setup.ps1 needs bcdedit and the native registry view. }
  if not Exec(ExpandConstant('{sysnative}\WindowsPowerShell\v1.0\powershell.exe'),
              '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Script + '" ' + Params,
              ExpandConstant('{tmp}'), SW_HIDE, ewWaitUntilTerminated, Result) then
    Result := -1;
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  ExtractTemporaryFile('setup.ps1');
  case RunSetupScript(ExpandConstant('{tmp}\setup.ps1'), '-Check') of
    2: begin
         SuppressibleMsgBox(CustomMessage('SecureBootOn'), mbCriticalError, MB_OK, IDOK);
         Result := False;
       end;
    3: case SuppressibleMsgBox(CustomMessage('BitLockerOn'), mbConfirmation, MB_YESNOCANCEL, IDYES) of
         IDYES: SuspendBitLocker := True;
         IDCANCEL: Result := False;
       end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  RunSetupScript(ExpandConstant('{tmp}\setup.ps1'), '-Stop');
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Params, Message: String;
  Code: Integer;
begin
  if CurStep <> ssPostInstall then
    exit;
  WizardForm.StatusLabel.Caption := CustomMessage('Configuring');
  Params := '';
  if SuspendBitLocker then
    Params := '-SuspendBitLocker';
  Code := RunSetupScript(ExpandConstant('{app}\setup.ps1'), Params);
  if Code = 3010 then
    RestartNeeded := True
  else if Code <> 0 then begin
    { A line may not start with "[": ISCC would take it for a section. }
    Message := FmtMessage(CustomMessage('SetupFailed'), [IntToStr(Code), ExpandConstant('{commonappdata}\Win51\setup.log')]);
    SuppressibleMsgBox(Message, mbError, MB_OK, IDOK);
  end;
end;

function NeedRestart: Boolean;
begin
  Result := RestartNeeded;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    UninstallRestartNeeded := RunSetupScript(ExpandConstant('{app}\setup.ps1'), '-Uninstall') = 3010;
end;

function UninstallNeedRestart: Boolean;
begin
  Result := UninstallRestartNeeded;
end;
