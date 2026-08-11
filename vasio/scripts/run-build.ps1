[CmdletBinding()]
param(
  [string]$BuildDir = "build"
)

# Windows Docker コンテナ内(Dockerfile.engine.windows と同じイメージ)で実行する。
# BUILD_VASIO_DRIVER は CMakeLists.txt の既定どおり ON のままにし、マウントされた
# ソースツリーに engine/thirdparty/asiosdk が無ければ自動的に OFF へフォールバック
# する(vasio.dll はスキップされるが、shared_protocol.h のオフラインテストは
# 常にビルド・実行できる)。

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

& cmake -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& ctest --test-dir $BuildDir -C Release --output-on-failure
exit $LASTEXITCODE
