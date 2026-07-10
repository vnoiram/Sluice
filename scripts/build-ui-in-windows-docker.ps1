[CmdletBinding()]
param(
  [string]$Image = "sluice-ui-build:local",
  [string]$DockerExe
)

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
  robocopy $Source $Destination /MIR /XD .git build bin obj /XF *.zip | Out-Host
  if ($LASTEXITCODE -gt 7) {
    exit $LASTEXITCODE
  }
}

$ResolvedDockerExe = Resolve-DockerExe $DockerExe

# Building/mounting directly from the WSL UNC path is flaky, so stage to a
# local NTFS temp dir first (same approach as streamdock-plugins' Dockerfile.*.windows scripts).
if ($Root.StartsWith("\\wsl.localhost\", [StringComparison]::OrdinalIgnoreCase) -or
    $Root.StartsWith("\\wsl$\", [StringComparison]::OrdinalIgnoreCase)) {
  $StagingRoot = Join-Path $env:TEMP "sluice-ui-build"
  if (Test-Path $StagingRoot) {
    Remove-Item -Recurse -Force $StagingRoot
  }
  Copy-Tree $Root $StagingRoot
  $WorkRoot = $StagingRoot
}

Invoke-Docker build `
  --file (Join-Path $WorkRoot "Dockerfile.ui.windows") `
  --tag $Image `
  $WorkRoot

Invoke-Docker run --rm `
  --volume "${WorkRoot}:C:\work" `
  --workdir "C:\work" `
  $Image `
  "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "ui\scripts\run-tests.ps1"
