[CmdletBinding()]
param(
  [string]$BuildDir = "build-vasio",
  # gap 11: 32bit DAW 対応。vasio.dll は CMakeLists.txt 自体が
  # アーキテクチャに依存しないため(レジストリ登録も KEY_WOW64_* を
  # 使わず呼び出し元プロセスのビットネスに自然追従する、
  # asio-abi/asio_registry.cpp 参照)、-Platform Win32 を渡すだけで
  # 32bit DAW 用の vasio.dll をビルドできる。既定は x64。
  [string]$Platform = "x64"
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
# なる(実機での Windows Docker ビルドで確認済みの実バグ)。同じ理由で
# x64/Win32 ビルドも別ディレクトリにする必要がある(呼び出し側が
# -BuildDir を variant ごとに変えること)。

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

& cmake -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A $Platform
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& ctest --test-dir $BuildDir -C Release --output-on-failure
exit $LASTEXITCODE
