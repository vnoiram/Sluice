[CmdletBinding()]
param(
  [string]$Image = "sluice-engine-build:local",
  [string]$DockerExe
)

# tools/latencybench/ (仮想デバイス実測ベンチマーク、実装ガイド §7.3) のビルド/テスト。
# 既存の scripts/build-engine-tests-in-windows-docker.ps1 と同じ Dockerfile.engine.windows
# イメージを再利用する。latencybench.exe 本体(WasapiDevice/KsDevice を含む)は
# ASIO SDK を必要としないが、engine/device/*.cpp を直接コンパイルするため
# Windows SDK(setupapi/ksuser 等)は必要 —— これは Dockerfile.engine.windows の
# VS Build Tools に含まれる。xcorr.h のオフラインテストはプラットフォーム非依存。

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath
$WorkRoot = $Root
$StagingRoot = $null

function Resolve-DockerExe {
  param([string]$Override)

  if ($Override) {
    return $Override
  }

  $cmd = Get-Command docker.exe -ErrorAction SilentlyContinue
  if ($cmd) {
    return $cmd.Source
  }

  $candidate = "C:\Program Files\Docker\Docker\resources\bin\docker.exe"
  if (Test-Path $candidate) {
    return $candidate
  }

  throw "docker.exe was not found. Pass -DockerExe."
}

function Invoke-Docker {
  & $ResolvedDockerExe @args
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
}

function Copy-Tree {
  param(
    [string]$Source,
    [string]$Destination
  )

  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  robocopy $Source $Destination /MIR /XD .git build /XF *.zip | Out-Host
  if ($LASTEXITCODE -gt 7) {
    exit $LASTEXITCODE
  }
}

$ResolvedDockerExe = Resolve-DockerExe $DockerExe

# WSL UNC パスからの直接マウントは不安定なため、ローカル NTFS 一時ディレクトリに
# ステージングしてからビルドする(build-engine-tests-in-windows-docker.ps1 と同じ手法)。
if ($Root.StartsWith("\\wsl.localhost\", [StringComparison]::OrdinalIgnoreCase) -or
    $Root.StartsWith("\\wsl$\", [StringComparison]::OrdinalIgnoreCase)) {
  $StagingRoot = Join-Path $env:TEMP "sluice-latencybench-build"
  if (Test-Path $StagingRoot) {
    Remove-Item -Recurse -Force $StagingRoot
  }
  Copy-Tree $Root $StagingRoot
  $WorkRoot = $StagingRoot
}

Invoke-Docker build `
  --file (Join-Path $WorkRoot "Dockerfile.engine.windows") `
  --tag $Image `
  $WorkRoot

Invoke-Docker run --rm `
  --volume "${WorkRoot}:C:\work" `
  --workdir "C:\work\tools\latencybench" `
  $Image `
  "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "cmake -S . -B build -G 'Visual Studio 17 2022' -A x64; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; cmake --build build --config Release; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ctest --test-dir build -C Release --output-on-failure"
