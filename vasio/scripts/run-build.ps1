[CmdletBinding()]
param(
  [string]$BuildDir = "build"
)

# Windows Docker コンテナ内(Dockerfile.engine.windows と同じイメージ)で実行する。
# BUILD_VASIO_DRIVER は CMakeLists.txt の既定どおり ON のままにする。ASIO SDK は
# 不要(../asio-abi/ の独自 ABI 実装でビルドする、asio-abi/README.md 参照)なので、
# vasio.dll の実ビルドと shared_protocol.h のオフラインテストを常に両方行う。

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

& cmake -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& ctest --test-dir $BuildDir -C Release --output-on-failure
exit $LASTEXITCODE
