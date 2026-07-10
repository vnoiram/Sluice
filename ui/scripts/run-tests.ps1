[CmdletBinding()]
param(
  [string]$Configuration = "Release"
)

# Windows Docker コンテナ内(Dockerfile.ui.windows)で実行される想定。
# WPF アプリ(SluiceUi)自体はここでは実行できない(デスクトップ環境が
# 無いため)。dotnet build によるコンパイル確認までを行い、WPF に依存
# しない SluiceUi.Core / SluiceUi.Core.Tests は実際にテストを実行する。

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

Write-Host "== dotnet build: SluiceUi (WPF, compile-check only) =="
& dotnet build (Join-Path $Root "SluiceUi\SluiceUi.csproj") -c $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== dotnet run: SluiceUi.Core.Tests =="
& dotnet run --project (Join-Path $Root "SluiceUi.Core.Tests\SluiceUi.Core.Tests.csproj") -c $Configuration
exit $LASTEXITCODE
