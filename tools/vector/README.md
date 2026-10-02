# ベクター変換worker

画像を「カラーの塗りSVG」と「色付き中心線SVG」に分けるLinux用CPU workerです。NCSとOpenVINOは使用しません。いずれのSVGにもラスタ画像を埋め込みません。画質上の制限とGUIの操作は `docs/vector/README.ja.md` を参照してください。

## 導入

Ubuntu 24.04ではPython 3.12を使用します。必要なOS開発パッケージは `python3-venv gcc pkg-config libglib2.0-dev libpng-dev` です。セットアップはsudoを実行しません。不足している場合はOS側で導入してください。

リポジトリのルートで実行します。

```sh
python3 tools/vector/setup_vector.py
python3 tools/vector/setup_vector.py --check
```

Python依存は `.local/vector-probe-venv`、AutoTraceは `.local/vector-probe-autotrace` に隔離します。先行検証で作成した環境がある場合も再利用できます。ネットワーク接続は通常のセットアップ時に必要です。

既存Python環境と保存済みソースアーカイブからAutoTraceを再ビルドする場合:

```sh
python3 tools/vector/setup_vector.py --offline --rebuild-autotrace
```

OSのGLibヘッダーが未導入でも、先行検証で保存した `.local/vector-probe-autotrace/sdk` があれば再利用します。新しい環境ではOSの開発パッケージを使用してください。

## 固定バージョンと再現情報

`dependencies.json` と `requirements.txt` に固定値を保存しています。

| 依存 | 固定版 |
| --- | --- |
| VTracer Python package | 0.6.15 |
| NumPy | 2.2.6 |
| Pillow | 10.2.0 |
| AutoTrace公式ソースタグ | 0.31.10 |

AutoTraceは公式GitHubのタグアーカイブを取得し、SHA-256 `14627f93bb02fe14eeda0163434a7cb9b1f316c0f1727f0bdf6323a831ffe80d` を照合します。このタグの `configure.ac` は内部バージョンを0.40.0と宣言しているため、生成バイナリの `--version` は0.40.0と表示されます。VTracer 0.6.15が生成するSVGコメントにも別の内部バージョンが表示される場合があります。固定値の確認にはパッケージ版・ソースタグ・SHAを使用します。

セットアップ後の実パス・Pythonパッケージ版・AutoTraceバイナリSHAは `.local/vector-probe-autotrace/qviewsr-dependencies.json` に保存します。ソースとライセンスファイルは削除しません。

## ライセンスの所在

- AutoTraceのソースにはGPL-2.0-or-laterの宣言があります。GPL v2本文は `.local/vector-probe-autotrace/autotrace-0.31.10/COPYING`、同梱されるLGPL v2.1本文は同ディレクトリの `COPYING.LIB` です。各ソースファイルの著作権・ライセンス宣言も保持します。
- VTracer 0.6.15はMITライセンスです。インストール後の本文は `.local/vector-probe-venv/lib/python3.12/site-packages/vtracer-0.6.15.dist-info/licenses/LICENSE` にあります。Pythonのバージョンが異なる場合は対応する `pythonX.Y` ディレクトリになります。
- NumPyはBSD系、PillowはHPND系のライセンスと付属ライブラリの通知を持ちます。インストール済みパッケージとOSパッケージのライセンス通知を保持してください。

このディレクトリには外部バイナリを同梱していません。セットアップ用ソースURLとハッシュ、再ビルド手順を保存しています。

## CLI

```sh
.local/vector-probe-venv/bin/python tools/vector/vector_worker.py \
  --input /absolute/source.png --cache /absolute/cache \
  --strength balanced --detail balanced --suppression 35
```

`QVIEWSR_AUTOTRACE` でAutoTrace実行ファイルのパスを指定できます。

- `--strength weak|balanced|strong`: 線候補抽出の閾値20/12/6。9×9輝度closingとの差を使用します。
- `--detail fine|balanced|simple`: 曲線近似の許容誤差0.3/1/3px。単位は元画像の画素です。平滑化4回、角判定100°/60°、周囲4pxは共通です。
- `--suppression 0..100`: 塗りに残る元線を弱める割合。strongの固定マスクを周囲3×3へ拡張し、160回の近傍補間で塗りを推定します。主線側の抽出強度・曲線設定とは独立しています。

線のAutoTrace引数 `line-threshold=1`、`line-reversion-threshold=.01`、`despeckle-level=0` は固定ソース `src/fit.c` の初期値と同じです。`preserve-width` は使用しません。表示線幅はSVGを読む側で変更します。

成功時はstdoutの最終1行に次のJSONを出力します。経過と失敗理由はstderrへ出力し、失敗時は終了コードが0以外になります。

```json
{"width":736,"height":1024,"fill_svg":"/absolute/cache/fill.svg","lines_svg":"/absolute/cache/lines.svg","cache_hits":["fill","lines"]}
```

キャッシュには入力ファイルSHA、処理版、関連する依存バージョン／AutoTraceバイナリSHA、処理パラメーターを含めます。線と塗りは別々に再利用されます。線抽出設定を変更しても塗りを再計算せず、元線弱化を変更しても主線を再計算しません。

## 制限と停止

入力上限は4,000,000画素、一辺4096px、圧縮ファイル128MiBです。RGBと二値alpha（0または255）を受け付け、完全透明部分は白地に合成して抽出し、透明領域を通る中心線セグメントを除外します。SVGの曲線近似によって透明輪郭の形状は変わり得ます。半透明を含む画像はエラーにします。VTracerが中間alphaを不透明に変換するため、現時点では半透明の保持に対応していません。

細部の欠落・主線の途切れ・補間による色にじみ・元線との二重化は残ります。中心線は元画像の線幅を復元せず、曲線区間ごとに近傍の色を推定します。曲線間で色の切り替わりが見える場合があります。

AutoTrace処理には45秒の上限があります。Linuxの親死亡シグナル設定により、GUIがworkerを強制終了するとAutoTraceも終了します。純Python／VTracer部分はworker自身で実行するため、別の処理プロセスを残しません。

## テスト

```sh
.local/vector-probe-venv/bin/python tools/vector/test_vector_worker.py
```

キャッシュの独立性と破損復旧、入力変更、二値透過・全透明、半透明・過大入力の拒否、純SVG、曲線座標の保持、worker強制終了時のAutoTrace停止を検証します。
