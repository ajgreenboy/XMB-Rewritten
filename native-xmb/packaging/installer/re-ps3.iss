; Inno Setup script for the Windows installer (re-ps3-setup.exe).
;
; It installs the same folder that build-package.ps1 produces (and that re-ps3.zip carries), so the installed
; app and the portable zip are identical and the in-app updater keeps working on an installed copy:
;
; - Per user, no administrator rights, into %LOCALAPPDATA%\Programs\RE-PS3. The updater swaps the whole
;   install folder for the staged one beside it (apply-update.ps1), so the folder and its parent must be
;   writable by the user; Program Files would not be.
; - The uninstaller's own files live outside the install folder (UninstallFilesDir) because the updater's
;   folder swap would otherwise lose them and break "Uninstall" in Settings > Apps.
; - Autostart and the Start-menu shortcuts are made by install-autostart.ps1, the same script the XMB's
;   "Start at Sign-in" row runs, so the installer and the app agree on what they are.
; - Firmware: by default the installer runs "re-ps3-native.exe --firmware-install" (Sony download, 4.93 check, unpack) into
;   {app}\data\dev_flash, then writes xmb-config.json (devFlash, dataDir) from the result. Silent installs take
;   /FIRMWARE=download (default), /FIRMWARE=skip or /DEVFLASH="C:\path". Skip or failure leaves the XMB's own first-run
;   Firmware page to finish it.
; - xmb-config.json is written only when there isn't one yet; an upgrade never touches it or data\.
;
; Build: ISCC.exe /DAppVersion=0.3.0 /DSourceDir=C:\release\dist\re-ps3 /DOutputDir=C:\release\dist re-ps3.iss
; The release workflow does this; see packaging/README.md.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\..\dist\re-ps3"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\..\dist"
#endif

#define AppName "RE-PS3 XMB"
#define AppExe "re-ps3-native.exe"

[Setup]
; Never change AppId: Windows uses it to recognise upgrades of the same app.
AppId={{579E0505-1F6F-49BB-809B-4899F021DACE}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=RE-PS3
VersionInfoVersion={#AppVersion}
; Per user, and the user can't switch to a machine-wide install (see the header for why).
PrivilegesRequired=lowest
DefaultDirName={autopf}\RE-PS3
DisableProgramGroupPage=yes
UninstallFilesDir={localappdata}\RE-PS3\uninstall
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=re-ps3-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Stopping the app is done in PrepareToInstall (the watchdog would restart it otherwise).
CloseApplications=no

[Tasks]
Name: "autostart"; Description: "Start the XMB full-screen when I sign in to Windows (with crash watchdog)"
Name: "shortcuts"; Description: "Add ""XMB"" and ""Return to XMB"" to the Start menu"
; Controller Mode needs the ViGEmBus driver. Offered (and ticked) only when the ViGEmBus service is missing: an existing
; ViGEmBus, whatever its version, is left alone. The vendor documents no silent switch, so its own window shows.
Name: "controllerdriver"; Description: "Install the controller driver (needed for Controller Mode)"; Check: ViGEmBusMissing

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; \
  Excludes: "xmb-config.json,data\*,watchdog.log*"

[Run]
; Both are per user and need no administrator rights. -NoTask makes only the shortcuts.
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install-autostart.ps1"" -Display {code:DisplayArg} -Shortcuts"; \
  Flags: runhidden waituntilterminated; Tasks: autostart and shortcuts; StatusMsg: "Registering autostart and shortcuts..."
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install-autostart.ps1"" -Display {code:DisplayArg}"; \
  Flags: runhidden waituntilterminated; Tasks: autostart and not shortcuts; StatusMsg: "Registering autostart..."
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install-autostart.ps1"" -Display {code:DisplayArg} -NoTask"; \
  Flags: runhidden waituntilterminated; Tasks: not autostart and shortcuts; StatusMsg: "Adding shortcuts..."
; An upgrade that unticks a task removes what an earlier install made.
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install-autostart.ps1"" -Uninstall"; \
  Flags: runhidden waituntilterminated; Tasks: not autostart and not shortcuts
; The vendor installer is elevated by itself (runas); this setup stays per user. A silent setup skips it: the vendor installer has no documented silent mode.
Filename: "{app}\drivers\ViGEmBus_1.22.0_x64_x86_arm64.exe"; Verb: runas; \
  Flags: shellexec waituntilterminated skipifsilent; Tasks: controllerdriver; Check: ViGEmBusMissing and BundledDriverPresent; \
  StatusMsg: "Installing the controller driver..."
; Start it now the way the logon task would (without firmware it opens the XMB's own first-run setup). Through
; kiosk-launch.vbs, like the task: Windows 11's default terminal would otherwise open an empty terminal window.
Filename: "{sys}\wscript.exe"; \
  Parameters: "//B //Nologo ""{app}\kiosk-launch.vbs"" {code:DisplayArg}"; \
  WorkingDir: "{app}"; Description: "Start the XMB now"; Flags: postinstall nowait runhidden skipifsilent

[UninstallRun]
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install-autostart.ps1"" -Uninstall"; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemoveAutostart"

[UninstallDelete]
; The previous copy the in-app updater keeps after an update.
Type: filesandordirs; Name: "{app}.old"

[Code]
const
  // Longest the installer waits for the download and unpack (the app itself gives up on a stalled download after 10 minutes).
  FirmwareTimeoutMs = 2400000;

var
  FirmwarePage: TInputOptionWizardPage;   // download / I have a folder / skip
  OwnFlashPage: TInputDirWizardPage;      // the "I already have a dev_flash folder" alternative
  DisplayPage: TInputQueryWizardPage;
  ProgressPage: TOutputMarqueeProgressWizardPage;

// True when no ViGEmBus service exists, so an existing driver (the HTPC has 1.21.442) is never touched.
function ViGEmBusMissing(): Boolean;
begin
  Result := not RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\ViGEmBus');
end;

function BundledDriverPresent(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\drivers\ViGEmBus_1.22.0_x64_x86_arm64.exe'));
end;

function ConfigPath(): String;
begin
  Result := ExpandConstant('{app}\xmb-config.json');
end;

function FlashLooksValid(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\vsh\resource\explore_plugin_full.rco');
end;

procedure InitializeWizard();
var
  Mode, Flash: String;
begin
  FirmwarePage := CreateInputOptionPage(wpSelectDir,
    'PS3 system software', 'The XMB needs Sony''s PS3 system software (firmware 4.93)',
    'The XMB draws its icons, text and fonts from Sony''s own PS3 system software. This installer can fetch it ' +
    'for you: it downloads the official update file from Sony''s update server (about 200 MB), checks it, and ' +
    'unpacks it into the XMB''s data folder. Nothing else is needed.' + #13#10#13#10 +
    'If xmb-config.json already exists (an upgrade), this step is skipped and the file is kept as it is.',
    True, False);
  FirmwarePage.Add('Download the PS3 system software from Sony now (about 200 MB; recommended)');
  FirmwarePage.Add('I already have a dev_flash folder (you choose it on the next page)');
  FirmwarePage.Add('Skip for now (the XMB offers the download the first time it starts)');

  // Silent installs: /FIRMWARE=download (the default), /FIRMWARE=skip, or /DEVFLASH="C:\path\to\dev_flash".
  Mode := Lowercase(ExpandConstant('{param:FIRMWARE|download}'));
  Flash := ExpandConstant('{param:DEVFLASH|}');
  if Flash <> '' then FirmwarePage.SelectedValueIndex := 1
  else if Mode = 'skip' then FirmwarePage.SelectedValueIndex := 2
  else FirmwarePage.SelectedValueIndex := 0;

  OwnFlashPage := CreateInputDirPage(FirmwarePage.ID,
    'Existing dev_flash folder', 'Where is your dev_flash folder?',
    'Choose the folder that holds vsh\resource\explore_plugin_full.rco (a console dump or the dev_flash of an RPCS3 ' +
    'install). It is used where it is; nothing is copied. The decrypted vsh_module folder is optional and only ' +
    'matters for the exact ribbon animation; leave it empty if you do not have one.',
    False, '');
  OwnFlashPage.Add('dev_flash folder:');
  OwnFlashPage.Add('Decrypted vsh_module folder (optional, advanced):');
  OwnFlashPage.Values[0] := Flash;

  DisplayPage := CreateInputQueryPage(OwnFlashPage.ID,
    'Display', 'Which screen is the TV?',
    'The XMB runs full-screen on one display, numbered from 0 in the order Windows lists them. On a PC with ' +
    'one screen use 0; with a monitor and a TV the TV is usually 1.');
  DisplayPage.Add('Display number:', False);
  DisplayPage.Values[0] := GetPreviousData('Display', '1');

  ProgressPage := CreateOutputMarqueeProgressPage('Downloading the PS3 system software',
    'About 200 MB from Sony''s update server. This takes a few minutes; please keep this window open.');
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Display', DisplayPage.Values[0]);
end;

function DisplayArg(Param: String): String;
begin
  Result := IntToStr(StrToIntDef(Trim(DisplayPage.Values[0]), 1));
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  // An upgrade keeps its config, so don't ask about firmware again.
  if PageID = FirmwarePage.ID then Result := FileExists(ConfigPath())
  else if PageID = OwnFlashPage.ID then Result := FileExists(ConfigPath()) or (FirmwarePage.SelectedValueIndex <> 1);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir: String;
begin
  Result := True;
  if CurPageID = OwnFlashPage.ID then begin
    Dir := Trim(OwnFlashPage.Values[0]);
    if Dir = '' then begin
      MsgBox('Choose your dev_flash folder, or go back and pick the download.', mbError, MB_OK);
      Result := False;
    end else if not FlashLooksValid(Dir) then
      Result := MsgBox('That folder has no vsh\resource\explore_plugin_full.rco, so it does not look like a PS3 ' +
        'dev_flash. Use it anyway?', mbConfirmation, MB_YESNO) = IDYES;
  end;
  if CurPageID = DisplayPage.ID then begin
    if StrToIntDef(Trim(DisplayPage.Values[0]), -1) < 0 then begin
      MsgBox('Enter a display number: 0, 1, 2 ...', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function JsonString(S: String): String;
begin
  StringChangeEx(S, '\', '\\', True);
  StringChangeEx(S, '"', '\"', True);
  Result := '"' + S + '"';
end;

function LastLineOf(FileName: String): String;
var
  Lines: TArrayOfString;
begin
  Result := '';
  if LoadStringsFromFile(FileName, Lines) and (GetArrayLength(Lines) > 0) then
    Result := Trim(Lines[GetArrayLength(Lines) - 1]);
end;

// One attempt of "re-ps3-native.exe --firmware-install <Dest>". The app appends a line per event to LogFile and ends it with
// "Result: ok: ..." or "Result: failed: <reason>"; this polls the file so the page can show the percentage.
function DownloadFirmwareOnce(Dest, LogFile: String; var Reason: String): Boolean;
var
  Waited, Rc: Integer;
  Line: String;
  Finished: Boolean;
begin
  Result := False;
  Reason := '';
  Line := '';
  DeleteFile(LogFile);
  if not Exec(ExpandConstant('{app}\{#AppExe}'), '--firmware-install "' + Dest + '" --firmware-log "' + LogFile + '"',
      ExpandConstant('{app}'), SW_HIDE, ewNoWait, Rc) then begin
    Reason := 'Could not start the downloader: ' + SysErrorMessage(Rc);
    exit;
  end;
  Waited := 0;
  Finished := False;
  while (not Finished) and (Waited < FirmwareTimeoutMs) do begin
    Sleep(300);
    Waited := Waited + 300;
    ProgressPage.Animate;
    Line := LastLineOf(LogFile);
    if Copy(Line, 1, 7) = 'Result:' then Finished := True
    else if Copy(Line, 1, 7) = 'Stage: ' then ProgressPage.SetText(Copy(Line, 8, Length(Line)) + '...', '')
    else if Copy(Line, 1, 9) = 'Download:' then
      ProgressPage.SetText('Downloading the PS3 system software from Sony: ' + Trim(Copy(Line, 10, Length(Line))), '');
  end;
  if not Finished then begin
    Reason := 'Timed out waiting for the download.';
    exit;
  end;
  if Copy(Line, 1, 11) = 'Result: ok:' then
    Result := True
  else begin
    Reason := Trim(Copy(Line, 8, Length(Line)));
    if Copy(Reason, 1, 7) = 'failed:' then Reason := Trim(Copy(Reason, 8, Length(Reason)));
  end;
end;

// Download and unpack into Dest. On failure the user can Retry or Cancel (skip); a silent install skips. True when dev_flash is in place.
function DownloadFirmware(Dest: String): Boolean;
var
  Reason: String;
  Answer: Integer;
begin
  Result := False;
  try
    ProgressPage.Show;
    repeat
      ProgressPage.SetText('Contacting Sony''s update server...', '');
      Result := DownloadFirmwareOnce(Dest, ExpandConstant('{tmp}\firmware-install.log'), Reason);
      Answer := IDOK;
      if not Result then begin
        ProgressPage.Hide;
        Answer := SuppressibleMsgBox('The PS3 system software could not be set up:' + #13#10#13#10 + Reason + #13#10#13#10 +
          'Retry to try again, or Cancel to skip this step. If you skip, the XMB offers the download the first time it ' +
          'starts (Settings > System Settings > Run Setup Again does it too).', mbError, MB_RETRYCANCEL, IDCANCEL);
        if Answer = IDRETRY then ProgressPage.Show;
      end;
    until Answer <> IDRETRY;
  finally
    ProgressPage.Hide;
  end;
end;

// A fresh install writes xmb-config.json from what the firmware step produced. Skipped or failed: no file is written, so the
// XMB's first-run setup (and a later reinstall) can still finish the job. An upgrade never reaches here with a config.
procedure WriteConfigIfMissing();
var
  Lines, DevFlash, Vsh, DataDir: String;
begin
  if FileExists(ConfigPath()) then exit;
  DevFlash := '';
  Vsh := '';
  DataDir := ExpandConstant('{app}\data');
  case FirmwarePage.SelectedValueIndex of
    0: begin
      // The folder the XMB's own first-run download uses: <data>\dev_flash. data\ survives updates and uninstall.
      if DownloadFirmware(DataDir + '\dev_flash') and FlashLooksValid(DataDir + '\dev_flash') then
        DevFlash := DataDir + '\dev_flash';
    end;
    1: begin
      DevFlash := Trim(OwnFlashPage.Values[0]);
      Vsh := Trim(OwnFlashPage.Values[1]);
    end;
  end;
  if DevFlash = '' then exit;
  Lines := '{' + #13#10 + '  "devFlash": ' + JsonString(DevFlash) + ',' + #13#10 + '  "dataDir": ' + JsonString(DataDir);
  if Vsh <> '' then
    Lines := Lines + ',' + #13#10 + '  "vshModules": ' + JsonString(Vsh);
  Lines := Lines + #13#10 + '}' + #13#10;
  SaveStringToFile(ConfigPath(), Lines, False);
end;

// Stop a running XMB from this folder, and the watchdog that would restart it, so its files can be replaced.
procedure StopRunningXmb(AppDir: String);
var
  Script: String;
  ResultCode: Integer;
begin
  StringChangeEx(AppDir, '''', '''''', True);
  Script := '$d = ''' + AppDir + '\''; ' +
    'Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne $PID } | Where-Object { ' +
    '($_.CommandLine -and $_.CommandLine.IndexOf($d + ''watchdog.ps1'', [StringComparison]::OrdinalIgnoreCase) -ge 0) -or ' +
    '($_.ExecutablePath -and $_.ExecutablePath.StartsWith($d, [StringComparison]::OrdinalIgnoreCase)) } | ' +
    'Sort-Object { if ($_.Name -eq ''powershell.exe'') { 0 } else { 1 } } | ' +
    'ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }';
  Exec('powershell.exe', '-NoProfile -ExecutionPolicy Bypass -Command "' + Script + '"', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if DirExists(ExpandConstant('{app}')) then StopRunningXmb(ExpandConstant('{app}'));
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then WriteConfigIfMissing();
end;

function InitializeUninstall(): Boolean;
begin
  StopRunningXmb(ExpandConstant('{app}'));
  Result := True;
end;

// The in-app updater replaces the install folder with the files of a newer release, which the uninstall log
// doesn't know about. Remove everything in the folder except the user's own config and data, which stay (as
// most Windows apps do) so a reinstall picks up where it left off.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
  FindRec: TFindRec;
  Name: String;
begin
  if CurUninstallStep <> usPostUninstall then exit;
  App := ExpandConstant('{app}');
  if FindFirst(App + '\*', FindRec) then begin
    try
      repeat
        Name := FindRec.Name;
        if (Name <> '.') and (Name <> '..') and (CompareText(Name, 'data') <> 0) and
           (CompareText(Name, 'xmb-config.json') <> 0) then begin
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            DelTree(App + '\' + Name, True, True, True)
          else
            DeleteFile(App + '\' + Name);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  RemoveDir(App);   // only succeeds when nothing was kept
end;
