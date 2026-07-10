# パッケージング(実装ガイド §5.8)

## 実装済み(コード)

- **クラッシュダンプ収集**: `engine/crash/crash_handler.h` を
  `engine/main.cpp` の起動直後に呼んでいる。未処理例外発生時に
  `%TEMP%`(既定)へ `.dmp`(MiniDumpNormal)を書き出す。
  `engine/tests/test_crash_handler.cpp` で実際にクラッシュさせて
  ダンプが生成されることを確認済み。
- **バージョン情報**: `engine/version.h` とアプリ内表示に加え、
  `engine/sluice-engine.rc` で `sluice-engine.exe` に Win32
  バージョンリソース(ファイルのプロパティダイアログに出る情報)を
  埋め込んでいる。
- **インストーラスクリプト**: `packaging/sluice.iss`(Inno Setup)。

## 未実装・ユーザー側のアクションアイテム

以下は実装ガイド §5.8 が要求する内容のうち、コードとして書けない
(外部サービスへの申請・実行環境が要る)部分。このリポジトリでは
自動化していない。

1. **SignPath Foundation への署名申請**
   - 申請には次が必要: 公開リポジトリ、OSS ライセンス、CI ビルド
     (GitHub Actions)、署名ポリシーの明記。
   - このリポジトリはまだ GitHub 上で公開していない/CI が無いため、
     申請の前提が整っていない。公開・CI 整備が先。
   - 署名が通るまでは `sluice-engine.exe`/`SluiceUi.exe`/インストーラは
     未署名であり、初回実行時に Windows SmartScreen の警告が出る。
     ユーザー向け案内文はリポジトリルートの README.md
     「ライセンス・商標に関する注意」に追記済み(§ の場所は README 参照)。
2. **Inno Setup での実ビルド確認**
   - `packaging/sluice.iss` は参照実装であり、Inno Setup Compiler
     (ISCC.exe)でのコンパイル自体はこの環境(Windows Docker コンテナ、
     GUI ツール)では検証していない。実機で
     `ISCC.exe packaging\sluice.iss` を実行して確認すること。
3. **GitHub Actions CI の整備**
   - SignPath 申請の前提でもあり、`engine`/`ui` それぞれの
     `scripts/build-*-in-windows-docker.ps1` を GitHub Actions の
     Windows ランナーから呼び出す形が素直(Docker Desktop が使える
     ランナーであれば、ローカルと同じ Dockerfile をそのまま使える)。
     今回のスコープ外。
