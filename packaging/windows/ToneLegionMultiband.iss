; Inno Setup script for the Tone Legion Multiband beta (Windows x64).
; Build with build-installer.ps1, which reads the version from CMakeLists.txt.
; Needs Inno Setup 6.3 or newer (x64compatible). Paths are relative to this file.

#ifndef AppVersion
  #error AppVersion is not defined. Run build-installer.ps1, or pass /DAppVersion=x.y.z to ISCC.
#endif
#ifndef ArtefactDir
  #define ArtefactDir "..\..\build\ToneLegionMultiband_artefacts\Release"
#endif
#ifndef AppBuild
  #define AppBuild "local"
#endif

#define AppName      "Tone Legion Multiband"
#define AppPublisher "Tone Legion"
#define BundleName   "Tone Legion Multiband"
#define ReleaseLabel "beta"

[Setup]
; Never change AppId: Windows uses it to find an earlier install to upgrade or uninstall.
AppId={{AE7A704F-3CFB-47C1-998E-671F9EF26D36}
AppName={#AppName}
AppVersion={#AppVersion}-{#ReleaseLabel}
AppVerName={#AppName} {#AppVersion} {#ReleaseLabel} (build {#AppBuild})
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}.0
VersionInfoDescription={#AppName} {#ReleaseLabel} installer
; Only the uninstaller and the notes go here; the plugins go to the standard Common Files folders.
DefaultDirName={commonpf64}\Tone Legion\Multiband
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousSetupType=no
UsePreviousComponents=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
InfoBeforeFile=..\beta-notice.txt
OutputDir=..\..\build\installer
OutputBaseFilename=ToneLegionMultiband-{#AppVersion}-beta-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#ReleaseLabel}
SetupLogging=yes

[Messages]
SelectComponentsDesc=Which plugin formats should be installed?
SelectComponentsLabel2=Tick the formats your DAW uses. Most DAWs on Windows use VST3; Bitwig, REAPER and some others also load CLAP. Click Next when you are ready to continue.

[Types]
Name: "full";   Description: "VST3 and CLAP"
Name: "custom"; Description: "Choose formats"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin  (Common Files\VST3)"; Types: full custom
Name: "clap"; Description: "CLAP plugin  (Common Files\CLAP)"; Types: full custom

[InstallDelete]
; Replace an earlier beta's VST3 bundle whole, so no stale files stay inside it.
Type: filesandordirs; Name: "{commoncf64}\VST3\{#BundleName}.vst3"; Components: vst3

[Files]
Source: "{#ArtefactDir}\VST3\{#BundleName}.vst3\*"; DestDir: "{commoncf64}\VST3\{#BundleName}.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#ArtefactDir}\CLAP\{#BundleName}.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion
Source: "..\..\docs\beta\TESTER-NOTES.md"; DestDir: "{app}"; DestName: "Tester notes.txt"; Flags: ignoreversion isreadme
Source: "..\beta-notice.txt"; DestDir: "{app}"; DestName: "Beta notice.txt"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\resources\fonts\OFL-*.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\..\libs\JUCE\LICENSE.md"; DestDir: "{app}\licenses"; DestName: "JUCE-LICENSE.md"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\libs\clap-juce-extensions\LICENSE"; DestDir: "{app}\licenses"; DestName: "clap-juce-extensions-LICENSE.txt"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\libs\clap-juce-extensions\clap-libs\clap\LICENSE"; DestDir: "{app}\licenses"; DestName: "CLAP-LICENSE.txt"; Flags: ignoreversion skipifsourcedoesntexist

[UninstallDelete]
Type: dirifempty; Name: "{commoncf64}\VST3\{#BundleName}.vst3"
Type: dirifempty; Name: "{app}\licenses"
Type: dirifempty; Name: "{app}"
Type: dirifempty; Name: "{commonpf64}\Tone Legion"
