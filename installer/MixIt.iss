; MIX IT installer (Inno Setup 6, M9). Built by scripts/build-installer.ps1 from
; the self-contained package folder. Per-machine install, Start menu entry,
; .mixit show files open in MIX IT, clean uninstall (settings and logs in the
; user profile are kept).
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\artifacts\MixIt-win-x64"
#endif
#ifndef OutputDir
  #define OutputDir "..\artifacts"
#endif

[Setup]
AppId={{6C1F3A52-7E0B-4C8B-9B8E-3F5D2A9C7E41}
AppName=MIX IT
AppVersion={#AppVersion}
AppVerName=MIX IT {#AppVersion}
AppPublisher=MIX IT
DefaultDirName={autopf}\MIX IT
DefaultGroupName=MIX IT
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=MixIt-Setup-{#AppVersion}-x64
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern
ChangesAssociations=yes
UninstallDisplayIcon={app}\MixIt.exe
PrivilegesRequired=admin

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\MIX IT"; Filename: "{app}\MixIt.exe"
Name: "{group}\MIX IT user guide"; Filename: "{app}\USER_GUIDE.md"
Name: "{group}\Uninstall MIX IT"; Filename: "{uninstallexe}"
Name: "{autodesktop}\MIX IT"; Filename: "{app}\MixIt.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Registry]
Root: HKA; Subkey: "Software\Classes\.mixit"; ValueType: string; ValueName: ""; ValueData: "MixIt.Show"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\MixIt.Show"; ValueType: string; ValueName: ""; ValueData: "MIX IT show"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\MixIt.Show\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\MixIt.exe,0"
Root: HKA; Subkey: "Software\Classes\MixIt.Show\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\MixIt.exe"" ""%1"""

[Dirs]
; Licence file location for all users of the PC.
Name: "{commonappdata}\MIX IT"; Permissions: users-modify

[Run]
Filename: "{app}\MixIt.exe"; Description: "Start MIX IT"; Flags: nowait postinstall skipifsilent
