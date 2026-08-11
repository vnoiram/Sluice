[CmdletBinding()]
param(
  [string]$Image = "sluice-engine-build:local",
  [string]$DockerExe
)

# vasio/ (仮想 ASIO ドライバ、実装ガイド §8.1) のビルド/テスト。
# 既存の scripts/build-engine-tests-in-windows-docker.ps1 と同じ Dockerfile.engine.windows
# イメージを再利用する(vasio は追加の vcpkg 依存を持たず、VS Build Tools + CMake だけで
# 足りるため)。ASIO SDK は不要(../asio-abi/ の独自 ABI 実装でビルドする、
# asio-abi/README.md 参照)なので、vasio.dll の実ビルドと shared_protocol.h の
# オフラインテストを常に両方実行する(vasio/CMakeLists.txt 参照)。

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
  $StagingRoot = Join-Path $env:TEMP "sluice-vasio-build"
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
  --workdir "C:\work" `
  $Image `
  "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "vasio\scripts\run-build.ps1"
