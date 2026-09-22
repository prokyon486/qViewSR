# 動作版の保存と復元

保存先: **[prokyon486/qViewSR-runtime-archive](https://github.com/prokyon486/qViewSR-runtime-archive)**（非公開）

Release: `runtime-2020.3.355-20260922`

OpenVINO 2020.3.2 / パッケージ2020.3.355の公式配布がなくなった場合に、所有者が同じ版を復元するための保管です。元の198,240,675バイトのtgzを変更せず保存し、SHA-256は`source-lock.json`と照合します。

## 保存するもの

- 元ランタイムtgz全体。Inference Engine、MYRIADプラグイン、NCS/NCS2ファームウェア、OpenCV 4.3、TBB、nGraph、ヘッダー、元のライセンス・第三者通知を含みます。
- 使用中の1032 FP16モデルXML/BINとモデルのライセンス。
- GUI・worker・Qtプラグインが依存する、このPCにインストール済みのUbuntuライブラリの同一版debとパッケージ一覧。OpenSSL開発用debも保存します。
- qViewSRのソーススナップショット、ビルド済みGUI/worker、ソース参照情報、環境情報、全ReleaseアセットのSHA-256。

Ubuntuのカーネル、GPUドライバー、デスクトップ全体、個人設定や画像は対象外です。Ubuntu依存パッケージは復旧資料であり、OS全体をオフラインで新規構築するイメージではありません。対応するUbuntu 24.04 amd64環境を用意してください。

## 配布条件

この世代の公式tgzに含まれる`licensing/EULA.txt`では内部コピーと再配布を分け、公開配布には`redist.txt`に列挙された範囲と製品への組込み等の条件を設けています。現在のISSLを古い全ファイルに一律適用せず、元の配布物をそのまま所有者向けに保管するため、保管リポジトリを非公開にしています。qViewSR本体のライセンスによって第三者バイナリを再ライセンスするものではありません。

参考: [Intelによる配布形態の説明](https://www.intel.com/content/www/us/en/support/articles/000095113/software/development-software.html)。2021.4を例にした現行説明のため、保存版には同梱の2020世代の条項を併せて残します。

## GitHubから直接復元

所有者アカウントで`gh auth login`を済ませ、qViewSRのソースで実行します。認証情報自体は保管物に含めません。

```bash
python3 tools/ncs/fetch_assets.py --source github --dest .local
./tools/build_qviewsr.sh
python3 tools/install_desktop.py
```

通常のビルドは公式配布を先に試し、取得失敗時に非公開保管先へフォールバックします。所有者以外の利用者は非公開保管先を使えません。`--source upstream`で公式配布のみに限定できます。

## アセットをダウンロードして復元

```bash
gh release download runtime-2020.3.355-20260922 \
  --repo prokyon486/qViewSR-runtime-archive --dir archive-download
(cd archive-download && sha256sum -c SHA256SUMS)
python3 tools/ncs/fetch_assets.py --archive-dir archive-download --dest .local
```

`--archive-dir`指定時はネットワークへ接続しません。既存のダウンロードが固定hashと異なる場合は上書きせず停止します。既存の展開先も書き換えないため、完全な復元確認には空の復元先を使ってください。

Ubuntu依存debを使用する場合は、アーカイブ内のパッケージ一覧を確認してから必要なものを選びます。日常利用中のOSに古いlibcやOpenSSL等を一括上書きせず、同じUbuntu版の復旧用環境で使用してください。ソースからの再ビルドを推奨します。保存バイナリのworkerには元のビルド場所へのRPATHが含まれるため、別の場所ではGUIのSR設定に復元したランタイムとモデルを指定してください。

## Ubuntuへの登録

`tools/install_desktop.py`は現在のユーザーのXDGデータディレクトリに`.desktop`とアイコンを配置し、MIMEデータベースを更新します。`%f`でファイル名を独立した引数として渡し、空白や日本語を含む画像パスにも対応します。登録するMIMEタイプはビルド済みqViewSRから取得します。既定アプリの設定は変更しません。
