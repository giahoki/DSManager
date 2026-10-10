; Inno Setup 7 script for DSM — Dark Souls Mod Manager.
; Build the Release x64 configuration first (bin\DSM.exe), then compile this file with Inno Setup.
; The installer is written to dist\; installers from earlier builds are deleted first.
;
; DSM is portable: fonts and pictures are inside the exe, and it keeps data\ (state, backups)
; and mods\ next to itself. The installer places DSM.exe and the mods from mods\ in a writable folder.

#define AppName      "DSM — Dark Souls Mod Manager"
#define AppPublisher "giahoki"
#define AppExe       "DSM.exe"

; The version comes from the exe itself (src\version.h -> the exe's version resource).
#define AppVersion   GetStringFileInfo(AddBackslash(SourcePath) + "bin\" + AppExe, "ProductVersion")
#if AppVersion == ""
  #error bin\DSM.exe is missing or has no version: build Release x64 first
#endif

; Remove installers left from earlier builds so dist\ only holds the current one.
#expr Exec(GetEnv("COMSPEC"), "/c if exist ""dist\DSM-Setup-*.exe"" del /q ""dist\DSM-Setup-*.exe""", SourcePath, 1, 0)

[Setup]
AppId={{9B1E5C6A-3F6D-4D8E-9C1A-6E0F2B7D4A11}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
; per user, into a folder DSM can write its data\ and mods\ to
DefaultDirName={localappdata}\Programs\DSM
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; LICENSE.md rendered for the license page (regenerate when LICENSE.md changes)
LicenseFile=assets\installer\license.rtf
OutputDir=dist
OutputBaseFilename=DSM-Setup-{#AppVersion}
Compression=lzma2/ultra
SolidCompression=yes
; dark, like DSM itself
WizardStyle=modern dark includetitlebar hidebevels
SetupIconFile=assets\icons\DSM.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
; asks to close DSM before installing or uninstalling (DSM holds this mutex while it runs)
AppMutex=Local\DSM_DarkSoulsModManager

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Dirs]
Name: "{app}\mods\Dark Souls Remastered"
Name: "{app}\mods\Dark Souls III"
Name: "{app}\mods\Elden Ring"

[Files]
Source: "bin\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE.md"; DestDir: "{app}"; Flags: ignoreversion
; licenses of the embedded fonts and libraries have to travel with them
Source: "THIRD-PARTY-LICENSES.md"; DestDir: "{app}"; Flags: ignoreversion
; the mods kept in the project's mods\, installed ready to use
Source: "mods\*"; DestDir: "{app}\mods"; Excludes: ".gitkeep"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\DSM"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\DSM"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,DSM}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; the whole program folder: exe, data\, mods\ and anything DSM created
Type: filesandordirs; Name: "{app}"

[CustomMessages]
russian.AskRestore=Убрать моды из игр перед удалением DSM?%n%nДа — вернуть оригинальные файлы игр, затем удалить DSM.%nНет — оставить моды в играх. Бэкапы оригиналов удалятся вместе с DSM, вернуть игру потом поможет только проверка целостности файлов в Steam.%nОтмена — ничего не удалять.
english.AskRestore=Remove the mods from the games before uninstalling DSM?%n%nYes - restore the original game files, then uninstall DSM.%nNo - keep the mods in the games. The backups of the original files are removed with DSM; only Steam's file verification can repair the games later.%nCancel - don't uninstall.
russian.RestoreFailed=Не удалось вернуть оригинальные файлы игр:%1%n%nСкорее всего, игра сейчас запущена. Закрой её и запусти удаление снова.%n%nУдалить DSM всё равно? Бэкапы оригинальных файлов будут потеряны, вернуть игру поможет только проверка целостности файлов в Steam.
english.RestoreFailed=Could not restore the original game files:%1%n%nThe game is probably running. Close it and run the uninstaller again.%n%nUninstall DSM anyway? The backups of the original files will be lost; only Steam's file verification can repair the game then.

[Code]
// data\ holds the backups of the original game files and is deleted with the program folder,
// so the user decides first whether the games are put back to their original state.
function RestoreGame(const Id: String): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{app}\{#AppExe}'), '--cli restore --game ' + Id, ExpandConstant('{app}'),
                 SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

function InitializeUninstall(): Boolean;
var
  Failed: String;
begin
  case MsgBox(CustomMessage('AskRestore'), mbConfirmation, MB_YESNOCANCEL) of
    IDCANCEL: begin Result := False; Exit; end;
    IDNO: begin Result := True; Exit; end;
  end;

  Failed := '';
  if not RestoreGame('dsr') then Failed := Failed + #13#10 + '  Dark Souls Remastered';
  if not RestoreGame('ds3') then Failed := Failed + #13#10 + '  Dark Souls III';
  if not RestoreGame('er') then Failed := Failed + #13#10 + '  Elden Ring';
  Result := (Failed = '') or
            (MsgBox(FmtMessage(CustomMessage('RestoreFailed'), [Failed]), mbError, MB_YESNO or MB_DEFBUTTON2) = IDYES);
end;
