# qViewSR

qViewを基盤に、**NCS×2＋NCS2×2による4倍超解像、ICCを考慮した表示と保存**を追加しています。Ubuntu 24.04のGUIで機能検証できる試作版です。実機4本を使い、GUIの開始ボタンから表示・切り替え・保存まで確認済みです。

## このPCで起動

```bash
cd /home/janis/qViewSR/qView
./tools/run_qviewsr.sh
```

画像を引数に渡すか、ウィンドウへ画像ファイルをドラッグしてください。WebP、BMP、TIFF、SVGなど、qViewのデコーダーが読み込める形式を超解像できます。このPCはビルド・モデル取得・USB権限設定まで完了しています。

| 操作 | ボタン／キー |
|---|---|
| 4倍超解像を開始 | 「超解像 ×4」／Ctrl+U |
| 元画像とSRを切り替える | 「元画像を表示」「SRを表示」／Ctrl+Space |
| sRGB ICC付きで別名保存 | 「SRを保存…」／Ctrl+Shift+S |
| 処理を中止 | 「中止」／Ctrl+. |
| 使用デバイス・表示ICCを変更 | 「SR設定…」 |
| ズーム／位置移動／画像送り | ホイール／ドラッグ／左右キー |
| qViewのメニュー | 右クリック |

超解像バーは長いエラー文も折り返さず1行に保ちます。収まらない部分はマウスを置くと全文を確認できます。フルスクリーンではバーを隠し、Ctrl+U・Ctrl+Spaceなどの操作は引き続き使えます。既存メニューと標準ダイアログも日本語で表示します。

処理中も元画像を閲覧できます。画像送りとウィンドウ終了では処理を中止し、前の画像の結果を表示しません。切り替えでは表示範囲を維持します。「原寸」はその時に表示している画像の1画素を基準にします。

設定の初期値は全NCS/NCS2（最大4本）、halo 16、メモリー上限2 GiBです。NCS2のみ、初代NCSのみ、個体IDのカンマ区切り、比較用CPUを選べます。「接続デバイスを確認」でIDを取得できます。完了後の状態表示にマウスを置くと、使用した各個体のタイル数を確認できます。

起動時にデバイスとモデルを準備し、同じウィンドウの次の画像から再利用します。デバイス選択・モデル・ランタイム・推論プログラムを変更した時に再初期化します。通常の中止では準備を保持し、応答停止などの異常時はworkerを終了して次回再初期化します。デバイスはウィンドウを閉じるまで保持し、複数ウィンドウ間での共有は未対応です。同時にSRを使うウィンドウは1つにしてください。

**ノイズ低減**は「SR設定… → ノイズ低減（SR前）」で指定します。0=なし（既定）、3=弱、6=標準、10=強、最大15です。JPEGのノイズをモデルが強調する前に、画像全体へ色付きNon-local Meansを適用します。強くすると細かい模様も滑らかになります。透明度はフィルターへ渡しません。変更して再実行しても、デバイス初期化は繰り返しません。

入力ICCからsRGBへ変換して推論します。PNGは透明度を別に拡大して保持し、JPEG保存では背景色を選択します。保存には現在の回転を適用し、**sRGB ICCを埋め込みます**。画面用ICCは保存画像に混ぜません。元画像への上書きはできません。

## 別のUbuntu 24.04環境でビルド

```bash
git clone --branch design/ncs-sr https://github.com/prokyon486/qViewSR.git
cd qViewSR
sudo bash tools/install_build_deps.sh
sudo bash tools/ncs/install_usb_access.sh
./tools/build_qviewsr.sh
./tools/run_qviewsr.sh
```

Qt 6.4以上とLittleCMS2を使います。日本語メニューには`qt6-l10n-tools`と`qt6-translations-l10n`が必要で、依存インストールスクリプトに含まれます。GUIはC++17、独立したworkerはC++14です。OpenVINO 2020.3.2と1032 FP16モデルは公式配布元から取得し、固定hashを確認して作業フォルダー内に展開します。DockerやシステムOpenVINOは不要です。初回は約198 MBをダウンロードします。通常のビルド・起動にsudoは不要です。

`QVIEWSR_ASSET_DIR`でアセット配置先、`QVIEWSR_BUILD_JOBS`でビルド並列数を変更できます。既存の`../.local`配置も自動検出します。配置を変更した場合はGUIのSR設定でパスを確認してください。旧runtimeのライブラリー検索パスはworkerだけに設定します。qView本家の設定とは別に保存します。

このPCではOpenSSL開発ヘッダーをローカルSDKから参照しています。通常の新規セットアップでは`install_build_deps.sh`に含まれる`libssl-dev`を使います。

## 検証と現時点の範囲

ビルドスクリプトはタイル座標・Qt GUIの自動テストも実行します。**ユーザー指定によりpushではGitHub CIを実行しません。** 全コミットに`[skip ci]`を付け、合格結果はローカル試験で確認します。従来の[CI移行パッチ](tools/ci/README.md)は未適用のままです。実機を使うテストは明示的に実行します。

```bash
./tools/test_gui_hardware.sh /path/to/test.png
# デスクトップ上でテストウィンドウも表示する場合
QT_QPA_PLATFORM=xcb ./tools/test_gui_hardware.sh /path/to/test.png
```

テストは専用の一時設定を使い、結果PNG/JPEG・画面画像・デバイス別タイル数を`diagnostics/local/gui-check/`に保存します。Gitには含めません。

- 確認済み: 実機4本による9タイルの処理、正確な4倍寸法、ズーム保持、元/SR切り替え、PNG/JPEGのICC付き保存、Adobe RGB→sRGBとalpha、表示ICC変更時の保存不変、表示回転保存、画像送りでの中止、終了時のworker停止、worker失敗・出力hash不一致時の元画像保持。
- 対象: qViewで画像として読み込める形式（導入済みデコーダーによる）。WebP/BMP/TIFF/PPM/ICO/XPM/SVGを追加試験済み。アニメーションGIF/WebP等はSR開始時の1フレームを静止して処理し、再生再開時にSR結果を解除します。動画全体のSRではありません。SRは8-bit SDR出力です。
- ICCなしはsRGBと仮定して表示に明記します。JPEG/PNG/WebPは元ICCも検査し、他形式はデコーダーの色情報を利用します。壊れたICCはSRを拒否します。CMYK/YCCKはデコーダーのRGB出力をsRGBと仮定する経路で処理し、元CMYK ICCをRGBへ誤適用しません。CMYKの厳密な印刷色再現は保証対象外です。
- 表示ICCの自動取得は既存のX11 rootプロファイルを使います。**複数モニターごとの自動選択・Waylandの色保証は未対応**です。必要な画面ICCはSR設定から手動指定できます。Qtが解析できない画面LUT ICCも手動指定を使います。
- 30分負荷、USB抜去からの復帰、写真セットでの世代間の画質差・最適台数・速度比較、全ICC形式、透明境界の画質は評価を継続する項目です。4本がCPUより速いとは確認していません。
- 大きな画像は割当前のメモリー見積りで開始を拒否します。workerとモデルは再利用しますが、SR画像の永続キャッシュやバッチ処理はありません。

[設計と実装状況](docs/sr/DESIGN.ja.md) · [実機検証記録](docs/sr/HARDWARE.ja.md) · [ソース・アセット固定情報](docs/sr/source-lock.json)

フォーク: [qViewSR](https://github.com/prokyon486/qViewSR) · [Open Model Zoo](https://github.com/prokyon486/qViewSR-open-model-zoo) · [OpenVINO](https://github.com/prokyon486/qViewSR-openvino)

上流qViewの説明は[README.md](README.md)に残しています。上流のQt 5・qmake・配布スクリプトは、この試作版のビルド手順には使いません。
