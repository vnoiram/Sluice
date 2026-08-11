[CmdletBinding()]
param(
  [string]$Image = "sluice-engine-build:local",
  [string]$DockerExe
)

# gap 11: vasio/ (仮想 ASIO ドライバ) の 32bit ビルド。32bit DAW にロード
# できる vasio.dll を作る。vasio/CMakeLists.txt 自体はアーキテクチャに
# 依存しない(レジストリ登録も呼び出し元プロセスのビットネスに自然追従
# する、asio-abi/asio_registry.cpp 参照)ため、build-vasio-in-windows-
# docker.ps1 と同じ Dockerfile.engine.windows イメージのまま、
# vasio/scripts/run-build.ps1 に -Platform Win32 を渡すだけでよい。
# ビルドディレクトリは x64 版(build-vasio)と衝突しないよう
# build-vasio-x86 にする(run-build.ps1 のコメント参照)。

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
# ステージングしてからビルドする(他の build-*-in-windows-docker.ps1 と同じ手法)。
if ($Root.StartsWith("\\wsl.localhost\", [StringComparison]::OrdinalIgnoreCase) -or
    $Root.StartsWith("\\wsl$\", [StringComparison]::OrdinalIgnoreCase)) {
  $StagingRoot = Join-Path $env:TEMP "sluice-vasio-x86-build"
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
  "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "vasio\scripts\run-build.ps1" -BuildDir "build-vasio-x86" -Platform "Win32"
