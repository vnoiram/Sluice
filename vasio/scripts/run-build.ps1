[CmdletBinding()]
param(
  [string]$BuildDir = "build-vasio"
)

# Windows Docker コンテナ内(Dockerfile.engine.windows と同じイメージ)で実行する。
# BUILD_VASIO_DRIVER は CMakeLists.txt の既定どおり ON のままにする。ASIO SDK は
# 不要(../asio-abi/ の独自 ABI 実装でビルドする、asio-abi/README.md 参照)なので、
# vasio.dll の実ビルドと shared_protocol.h のオフラインテストを常に両方行う。
#
# 既定のビルドディレクトリ名を engine/scripts/run-tests.ps1 の既定("build")と
# 意図的にずらしている: build-vasio-in-windows-docker.ps1 と
# build-engine-tests-in-windows-docker.ps1 は同じ --workdir "C:\work" で
# コンテナを起動するため、両方とも相対パス "build" のままだと同じ
# C:\work\build を取り合い、片方を実行した後にもう片方を実行すると
# 「異なる CMakeLists.txt から生成された既存キャッシュと一致しない」エラーに
# なる(実機での Windows Docker ビルドで確認済みの実バグ)。

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

& cmake -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& ctest --test-dir $BuildDir -C Release --output-on-failure
exit $LASTEXITCODE
