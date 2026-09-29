# qViewSR project instructions

- ユーザー指定により、pushでGitHub CIを実行しない。push対象の新規コミットすべてに `[skip ci]` を含め、push前にコミットメッセージを確認する。
- 明示的な依頼なしにGitHub Actionsを有効化・手動実行しない。ビルドとテストはローカルで行う。
- 検証用runtime・モデル・画像・ログは `.local` または `diagnostics/local` に置き、Gitへ追加しない。
- 動作版のバイナリ保管先は所有者の非公開リポジトリ `prokyon486/qViewSR-runtime-archive` のRelease。公開リポジトリへ元ランタイム全体を転載せず、ライセンスとSHA-256を保持する。
