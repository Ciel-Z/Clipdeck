; ClipDeck Windows installer (Inno Setup 6). Built by windows/scripts/package-windows.ps1,
; which stages the deployed app and passes AppVersion and StageDir.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef StageDir
  #error StageDir must point at the windeployqt output (see package-windows.ps1)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Never change AppId: upgrades and uninstall find the installed copy by it.
AppId={{E480502C-927C-4282-9D5A-4B90580F38C6}
AppName=ClipDeck
AppVersion={#AppVersion}
AppVerName=ClipDeck {#AppVersion}
AppPublisher=ClipDeck contributors
AppPublisherURL=https://github.com/Ciel-Z/Clipdeck
AppSupportURL=https://github.com/Ciel-Z/Clipdeck/issues
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\ClipDeck
DefaultGroupName=ClipDeck
DisableProgramGroupPage=yes
; Per-user by default (no UAC), like the HKCU autostart entry the app writes;
; "为所有用户安装" is offered and also adds the LAN sync firewall rule.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile=..\..\mac\LICENSE
SetupIconFile=..\..\shared\assets\windows\clipdeck.ico
UninstallDisplayIcon={app}\clipdeck.exe
UninstallDisplayName=ClipDeck
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
OutputDir={#OutputDir}
OutputBaseFilename=ClipDeck-{#AppVersion}-windows-x64-setup

[Languages]
Name: "chs"; MessagesFile: "ChineseSimplified.isl"

[CustomMessages]
chs.LaunchAtLogin=登录 Windows 时自动启动 ClipDeck
chs.StartupGroup=启动：
chs.FirewallRule=ClipDeck 局域网同步

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startup"; Description: "{cm:LaunchAtLogin}"; GroupDescription: "{cm:StartupGroup}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\ClipDeck"; Filename: "{app}\clipdeck.exe"
Name: "{autodesktop}\ClipDeck"; Filename: "{app}\clipdeck.exe"; Tasks: desktopicon

[Registry]
; Same value the app's "开机启动" setting writes (win_launch_at_login.cpp), so
; the setting shows it as on and can turn it off again.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ClipDeck"; \
  ValueData: """{app}\clipdeck.exe"""; Tasks: startup; Flags: uninsdeletevalue

[Run]
; LAN sync listens on TCP 47821 and UDP 47820; only an all-users install may add the rule.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{cm:FirewallRule}"""; \
  Flags: runhidden; Check: IsAdminInstallMode
Filename: "{sys}\netsh.exe"; \
  Parameters: "advfirewall firewall add rule name=""{cm:FirewallRule}"" dir=in action=allow program=""{app}\clipdeck.exe"" profile=private,domain enable=yes"; \
  Flags: runhidden; Check: IsAdminInstallMode
Filename: "{app}\clipdeck.exe"; Description: "{cm:LaunchProgram,ClipDeck}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{cm:FirewallRule}"""; \
  Flags: runhidden; RunOnceId: "RemoveFirewallRule"; Check: IsAdminInstallMode

[Code]
// ClipDeck lives in the tray with no visible window, so Restart Manager may not
// be able to close it; stop it before files are replaced or removed. History is
// in SQLite and survives this.
procedure StopClipDeck();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM clipdeck.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(500);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopClipDeck();
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  StopClipDeck();
  Result := True;
end;
