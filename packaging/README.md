# パッケージング(実装ガイド §5.7)

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

以下は実装ガイド §5.7 が要求する内容のうち、コードとして書けない
(外部サービスへの申請・実行環境が要る)部分。このリポジトリでは
自動化していない。

1. **SignPath Foundation への署名申請**
   - 申請には次が必要: 公開リポジトリ、OSS ライセンス、CI ビルド
     (GitHub Actions)、署名ポリシーの明記。
   - CI(`.github/workflows/ci.yml`)は整備済み。OSS ライセンス(MIT、
     リポジトリルートの `LICENSE`)も整備済み。残るは
     **このリポジトリを GitHub 上で公開すること**(現時点では未公開)。
     公開後に申請できる。
   - 署名が通るまでは `sluice-engine.exe`/`SluiceUi.exe`/インストーラは
     未署名であり、初回実行時に Windows SmartScreen の警告が出る。
     ユーザー向け案内文はリポジトリルートの README.md
     「ライセンス・商標に関する注意」に追記済み(§ の場所は README 参照)。
2. **Inno Setup での実ビルド確認**
   - `packaging/sluice.iss` は参照実装であり、Inno Setup Compiler
     (ISCC.exe)でのコンパイル自体はこの環境(Windows Docker コンテナ、
     GUI ツール)では検証していない。実機で
     `ISCC.exe packaging\sluice.iss` を実行して確認すること。
3. **SignPath Foundation の表記義務**(実際に署名が通ってから追加する)
   - SignPath Foundation の利用規約は、署名済み成果物の配布時に
     "Free code signing provided by [SignPath.io](https://signpath.io/),
     certificate by [SignPath Foundation](https://signpath.org/)" という
     趣旨の表記を求める(文言は SignPath 側の最新のガイドラインを申請時に
     必ず確認すること)。追加先の候補: リポジトリルートの README.md
     「ライセンス・商標に関する注意」節、`packaging/sluice.iss` の
     `[Messages] FinishedLabel`、インストーラのライセンス/謝辞画面。
