; SPDX-License-Identifier: GPL-3.0-or-later
;
; The Windows installer, built with Inno Setup from what `cmake --install` put in a folder:
;
;   iscc /DAppVersion=0.0.1 /DStageDir=<that folder> /DOutputDir=<where the installer goes> LCAD.iss
;
; It installs for the user alone, or for everyone when run as an administrator, adds LCAD to the
; Start menu, and makes LCAD what opens .lcad documents.

#ifndef AppVersion
  #error Pass the version: /DAppVersion=0.0.1
#endif
#ifndef StageDir
  #error Pass the folder LCAD was installed into: /DStageDir=...
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{3E3C487F-9296-4C19-89B2-45B35F5B7220}
AppName=LCAD
AppVersion={#AppVersion}
AppVerName=LCAD {#AppVersion}
AppPublisher=LCAD contributors
AppPublisherURL=https://github.com/CheapAzHobbies/LCAD
AppSupportURL=https://github.com/CheapAzHobbies/LCAD/issues
DefaultDirName={autopf}\LCAD
DefaultGroupName=LCAD
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=LCAD-{#AppVersion}-windows-x64-setup
SetupIconFile=LCAD.ico
UninstallDisplayIcon={app}\bin\lcad.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associate"; Description: "Open .lcad documents with LCAD"; GroupDescription: "File types:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\LCAD"; Filename: "{app}\bin\lcad.exe"
Name: "{autodesktop}\LCAD"; Filename: "{app}\bin\lcad.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.lcad"; ValueType: string; ValueName: ""; ValueData: "LCAD.Document"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.lcad\OpenWithProgids"; ValueType: string; ValueName: "LCAD.Document"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\LCAD.Document"; ValueType: string; ValueName: ""; ValueData: "LCAD document"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\LCAD.Document\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\lcad.exe,0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\LCAD.Document\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\lcad.exe"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\lcad.exe\SupportedTypes"; ValueType: string; ValueName: ".lcad"; ValueData: ""; Flags: uninsdeletekey; Tasks: associate

[Run]
Filename: "{app}\bin\lcad.exe"; Description: "{cm:LaunchProgram,LCAD}"; Flags: nowait postinstall skipifsilent
