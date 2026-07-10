; sluice.iss : Inno Setup インストーラスクリプト(実装ガイド §5.8)
;
; ビルド済みの sluice-engine.exe(engine/ を CMake --build 済み)と
; SluiceUi(ui/SluiceUi を `dotnet publish` 済み)をまとめてインストーラ化
; する。実行には Inno Setup(https://jrsoftware.org/isinfo.php、無償)が
; 必要で、本リポジトリには同梱していない(ツール自体は各自インストール
; する)。
;
; 署名について(実装ガイド §5.8): exe/DLL/インストーラは SignPath
; Foundation での署名を想定している。申請には公開リポジトリ・OSS
; ライセンス・CI ビルド(GitHub Actions)・署名ポリシー明記が必要。
; 署名自体はこのスクリプトの外(SignPath 連携の CI)で行うため、
; 未署名でビルドした場合は初回起動時に SmartScreen の警告が出る
; (README.md の「ライセンス・商標に関する注意」節を参照)。
;
; 使い方:
;   1. engine/ を Release ビルドし、engine\build\Release\sluice-engine.exe
;      を用意する。
;   2. ui/SluiceUi を `dotnet publish -c Release -r win-x64 --self-contained false`
;      等で発行し、ui\SluiceUi\bin\Release\net8.0-windows\publish\ を用意する。
;   3. Inno Setup Compiler (ISCC.exe) でこのファイルをコンパイルする:
;      ISCC.exe packaging\sluice.iss

#define MyAppName "Sluice"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "Sluice Project"
#define MyAppExeName "SluiceUi.exe"

[Setup]
AppId={{B6C1E9C0-4B0E-4B7D-9C2A-6E9F3D9B7A10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir=output
OutputBaseFilename=sluice-setup-{#MyAppVersion}
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes

; ASIO/VB-CABLE の商標・ライセンス表記(実装ガイド §10)
[Messages]
FinishedLabel=%nSetup has finished installing [name] on your computer.%n%nASIO is a trademark and software of Steinberg Media Technologies GmbH.%n仮想マイク機能を使うには VB-CABLE(公式サイトから別途入手)が必要です。

[Files]
; ビルド成果物の配置は各自の CI/ローカルビルドに合わせて調整すること。
Source: "..\engine\build\Release\sluice-engine.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\ui\SluiceUi\bin\Release\net8.0-windows\publish\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "デスクトップにアイコンを作成する"; GroupDescription: "追加のアイコン:"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{#MyAppName} を起動する"; Flags: nowait postinstall skipifsilent
