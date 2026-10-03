# ベクター変換worker

画像を「カラーの塗りSVG」と「色付き中心線SVG」に分けるLinux用CPU workerです。NCSとOpenVINOは使用しません。いずれのSVGにもラスタ画像を埋め込みません。画質上の制限とGUIの操作は `docs/vector/README.ja.md` を参照してください。

## 導入

Ubuntu 24.04ではPython 3.12を使用します。必要なOS開発パッケージは `python3-venv python3-dev gcc patch pkg-config libglib2.0-dev libpng-dev` です。セットアップはsudoを実行しません。不足している場合はOS側で導入してください。修正版VTracerの自動ビルドはLinux x86_64が対象です。

リポジトリのルートで実行します。

```sh
python3 tools/vector/setup_vector.py
python3 tools/vector/setup_vector.py --check
```

Python依存は `.local/vector-probe-venv`、AutoTraceは `.local/vector-probe-autotrace` に隔離します。VTracerの固定ソース、修正版wheel、Rust 1.90.0、Cargoの依存キャッシュは `.local/vector-patched-vtracer` に保存します。OSのRust環境やユーザーのCargo設定は変更しません。先行検証で作成した環境がある場合も再利用できます。初回セットアップにはネットワーク接続が必要です。

既存Python環境と保存済みソースアーカイブからAutoTraceを再ビルドする場合:

```sh
python3 tools/vector/setup_vector.py --offline --rebuild-autotrace
```

初回の修正版ビルド後は、保存済みソース・Rust・CargoキャッシュからVTracerも再ビルドできます。

```sh
python3 tools/vector/setup_vector.py --offline --rebuild-vtracer
```

ソースとパッチのSHA-256を毎回照合し、Cargoの固定lockfileを `--locked` で使用します。Rustの色合計・領域索引・細線判定の回帰テストと、小画像6種類を自動／1／8／16スレッドで変換するSVG一致検証がビルドに含まれます。wheelはビルドしたホスト向けのLinuxバイナリです。異なるOS・CPU・古いglibcへの互換性は保証しません。

OSのGLibヘッダーが未導入でも、先行検証で保存した `.local/vector-probe-autotrace/sdk` があれば再利用します。新しい環境ではOSの開発パッケージを使用してください。

## 固定バージョンと再現情報

`dependencies.json` と `requirements.txt` に固定値を保存しています。

| 依存 | 固定版 |
| --- | --- |
| VTracer Python package | 0.6.15+qviewsr.4（上流0.6.15への修正） |
| visioncortex | 0.8.10（ローカルパッチ適用） |
| Rust / Cargo | 1.90.0 |
| maturin | 1.9.4 |
| NumPy | 2.2.6 |
| Pillow | 10.2.0 |
| AutoTrace公式ソースタグ | 0.31.10 |

AutoTraceは公式GitHubのタグアーカイブを取得し、SHA-256 `14627f93bb02fe14eeda0163434a7cb9b1f316c0f1727f0bdf6323a831ffe80d` を照合します。このタグの `configure.ac` は内部バージョンを0.40.0と宣言しているため、生成バイナリの `--version` は0.40.0と表示されます。VTracer 0.6.15が生成するSVGコメントにも別の内部バージョンが表示される場合があります。固定値の確認にはパッケージ版・ソースタグ・SHAを使用します。

セットアップ後の実パス・Pythonパッケージ版・AutoTraceバイナリSHAは `.local/vector-probe-autotrace/qviewsr-dependencies.json` に保存します。修正版VTracerのソース・パッチ・wheel・Cargo.lockのSHAとビルド情報は `.local/vector-patched-vtracer/qviewsr-build.json` に保存します。ソースとライセンスファイルは削除しません。修正の根拠と検証方法は `patches/README.md` を参照してください。

## ライセンスの所在

- AutoTraceのソースにはGPL-2.0-or-laterの宣言があります。GPL v2本文は `.local/vector-probe-autotrace/autotrace-0.31.10/COPYING`、同梱されるLGPL v2.1本文は同ディレクトリの `COPYING.LIB` です。各ソースファイルの著作権・ライセンス宣言も保持します。
- VTracerはMITライセンスです。修正版の本文は `.local/vector-probe-venv/lib/python3.12/site-packages/vtracer-0.6.15+qviewsr.4.dist-info/licenses/LICENSE` にあります。同じディレクトリの `LICENSES` に、visioncortexのMIT/Apache-2.0本文と第三者著作権通知を保存します。Pythonのバージョンが異なる場合は対応する `pythonX.Y` ディレクトリになります。
- 元のVTracer・visioncortexソースとライセンスは `.local/vector-patched-vtracer/source` に保持します。Rustのライセンスは同ディレクトリ外の `rust-sources` に、Cargoが取得した各依存のソース・ライセンスは `cargo-home/registry/src` に保持します。
- NumPyはBSD系、PillowはHPND系のライセンスと付属ライブラリの通知を持ちます。インストール済みパッケージとOSパッケージのライセンス通知を保持してください。

このディレクトリには外部バイナリを同梱していません。セットアップ用ソースURLとハッシュ、再ビルド手順を保存しています。

## CLI

```sh
.local/vector-probe-venv/bin/python tools/vector/vector_worker.py \
  --input /absolute/source.png --cache /absolute/cache \
  --strength balanced --detail balanced --suppression 35
```

`QVIEWSR_AUTOTRACE` でAutoTrace実行ファイルのパスを指定できます。

- `--line-mode dark|color`: 既定darkは9×9輝度closingとの差。colorはRGBの色境界と不透明部分のシルエットを細線化します。細い暗線の両側を拾うため、両方式の単純合成はしません。
- `--strength weak|balanced|strong`: 線候補抽出の閾値20/12/6。どちらの抽出方式にも適用します。
- `--mask-gap 0|1|2`: dark方式の二値マスクの隙間補正。0は無効（既定）、1/2は3×3/5×5 closing。細い溝から生じる梯子状の分岐を抑えますが、近接した線や小さい文字の穴も結合します。透明部分には描画を広げません。color方式では無視し、キャッシュも共有します。
- `--detail fine|balanced|simple`: 曲線近似の許容誤差0.3/1/3px。単位は元画像の画素です。平滑化4回、角判定100°/60°、周囲4pxは共通です。
- `--suppression 0..100`: 塗りに残る元線を弱める割合。strongの固定マスクを周囲3×3へ拡張し、160回の近傍補間で塗りを推定します。主線側の抽出強度・曲線設定とは独立しています。

線のAutoTrace引数 `line-threshold=1`、`line-reversion-threshold=.01`、`despeckle-level=0` は固定ソース `src/fit.c` の初期値と同じです。`preserve-width` は使用しません。表示線幅はSVGを読む側で変更します。

成功時はstdoutの最終1行に次のJSONを出力します。経過と失敗理由はstderrへ出力し、失敗時は終了コードが0以外になります。

```json
{"width":736,"height":1024,"fill_svg":"/absolute/cache/fill.svg","lines_svg":"/absolute/cache/lines.svg","cache_hits":["fill","lines"]}
```

キャッシュには入力ファイルSHA、処理版、関連する依存バージョン／AutoTraceバイナリSHA、処理パラメーターを含めます。線と塗りは別々に再利用されます。線抽出設定を変更しても塗りを再計算せず、元線弱化を変更しても主線を再計算しません。

`--correct-lines` を追加すると、主線を直線・円・楕円・少ない制御点のベジエ曲線へ保守的に近似します。
省略時は補正OFFです。補正は色サンプリング前に適用し、元のAutoTrace出力を別にキャッシュするため、ON/OFFで再トレースしません。
`--shape-tolerance .25..100` は円・楕円の許容誤差（元画像px）。CLI既定1px、GUI既定2pxです。指定値と推定短半径の5%で移動量を制限し、開いた曲線の補正誤差1pxは変更しません。補正方式の版と許容誤差をキャッシュキーへ含めます。

`--clean-lines` は短い孤立線・横枝の除去と、接線方向の揃った近接端点の連結を行います。既定OFF。`--min-line-length 0..1000`（既定6px）は孤立線と横枝の長さ基準、`--join-distance 0..200`（既定4px）は接続距離、`--branch-strength 0..100`（既定50）は横枝除去強度です。それぞれ0で該当処理を無効化します。基準長0では横枝も除去しません。
順序は抽出→任意の隙間補正→AutoTrace→整理→形状補正→色サンプリングです。raw/clean/colored SVGを別にキャッシュし、抽出方式、隙間補正の版と半径、処理版、各有効パラメーターをキーに含めます。形状許容誤差を変えても、抽出や整理は再実行しません。
JSONには `line_mode`, `clean_lines`, `cleanup_stats`（`isolated_removed`, `spurs_removed`, `joins`, `closed_gaps` など）、`correct_lines`, `correction_stats`（`lines`, `circles`, `ellipses`, `simplified_curves` など）も返します。`joins`には閉輪郭の隙間を閉じた接続も含みます。

## CPUの利用

色境界は512pxタイルと12pxの重なりで最大4スレッド処理します。実機では16スレッドより4スレッドが速く、一時メモリーも少ないためです。タイル分割・実行順序による画素差はありません。線の整理・形状補正・AutoTraceは直列です。整理は空間索引で近傍だけを調べ、過密な候補や長すぎる支持長探索は保守的に変更を見送ります。

塗り側の元線弱化は、補間対象を262,144画素ずつの区間へ分け、近傍値の計算と画像への書き戻しを最大16スレッドで処理します。プロセスに割り当てられたCPU数と区間数でも制限し、1区間だけの小画像ではスレッドを起動しません。各反復で計算の全完了と書き戻しの全完了を待つため、160回の同時更新は直列版と全画素が一致します。元線弱化とVTracerなどの工程は順番に実行します。

修正版VTracerは、面積別の候補索引で領域の全走査を省き、面積順の管理を木構造へ変更して新規面積の挿入時の大量コピーを省き、確定した色領域からの曲線生成を最大16スレッドへ分散します。各領域の処理後に元の描画順へ戻すため、品質パラメーターや重なり順は変わりません。細い領域の判定は、通常のactive領域なら最初の内部画素で打ち切り、外接矩形の全走査を省きます。ZEROや保存済みの複製には従来計算を使います。色領域の統合は順序に依存するため直列です。SVG出力は64KiBのバッファを使い、flushの失敗も呼び出し元へ返します。

Pythonのファイル変換API `vtracer.convert_image_to_svg_py(..., num_threads=N)` は1～16の予算を受け付けます。省略時は利用可能CPU数と16の小さい方を選び、262,144画素未満では1スレッドを使用します。実際のスレッド数はCPU数と出力領域数でも制限します。ファイル変換中はGILを解放するため、Python側の別処理も進められます。並列にした領域ごとに一時的な二値画像と曲線データを持つので、1スレッドよりメモリー使用量は増えます。

主線の元画像色は4096区間ずつまとめてNumPyで計算します。従来の5点×3×3近傍から暗い画素を選ぶ規則と中央値を保持します。17984×21120画像の626,392区間で座標・色が全件一致しました。Pythonスレッドの追加では有効な短縮が得られず、色付けには単一スレッドの一括処理を使用します。

診断時に `QVIEWSR_VTRACER_PROFILE=1` を設定すると、VTracerの読込・領域確定・曲線生成・SVG書出しの経過秒数と曲線生成のスレッド数をstderrへ出力します。並列化できる工程の割合は画像に依存し、全処理時間がスレッド数に比例して短くなるわけではありません。

## 制限と停止

入力寸法・圧縮ファイルサイズに固定上限はありません。解像度は保持し、入力はディスクへストリーミング複製してSHAと処理内容を一致させます。
展開前に `190 bytes × 画素数 + 64MiB` の作業メモリーを見積もり、システム／現在のcgroupと祖先cgroupの残量の70%を超える場合に中止します。64bitの色合計による領域ごとの追加メモリーも見積もりに含めます。
Pillowの固定画素制限もこの確認に置き換えます。これは事前見積もりであり、実際のメモリー確保失敗もエラーとして扱います。
SVGは各層・合成後のファイルサイズを1GiBまでとし、解析前に `32 × SVGバイト数 + 64MiB` の作業メモリーを見積もります。利用可能メモリーの70%を超える場合は、見積もりと残量を示して中止します。残量を取得できない環境では、従来どおり64MiBまでです。キャッシュの資源不足も明示的なエラーにし、破損扱いで再生成しません。

RGBと二値alpha（0または255）を受け付け、完全透明部分は白地に合成して抽出し、透明領域を通る中心線セグメントを除外します。SVGの曲線近似によって透明輪郭の形状は変わり得ます。半透明を含む画像はエラーにします。VTracerが中間alphaを不透明に変換するため、現時点では半透明の保持に対応していません。

細部の欠落・主線の途切れ・補間による色にじみ・元線との二重化は残ります。中心線は元画像の線幅を復元せず、曲線区間ごとに近傍の色を推定します。曲線間で色の切り替わりが見える場合があります。

AutoTrace処理の上限は `min(7200, 45 + 画素数 / 25000)` 秒です。GUIも入力画素数に合わせてタイムアウトを延長します。
Linuxの親死亡シグナル設定により、GUIがworkerを強制終了するとAutoTraceも終了します。純Python／VTracer部分はworker自身で実行するため、別の処理プロセスを残しません。

## テスト

```sh
.local/vector-probe-venv/bin/python tools/vector/test_vector_worker.py
.local/vector-probe-venv/bin/python tools/vector/test_line_correction.py
.local/vector-probe-venv/bin/python tools/vector/test_color_paths.py
.local/vector-probe-venv/bin/python tools/vector/test_vtracer_patch.py
```

キャッシュの独立性と破損復旧、入力変更、二値透過・全透明、半透明の拒否、旧寸法・ファイルサイズ上限を超える入力、メモリー判定、純SVG、補正OFFの曲線保持、補正誤差・角と端点・誤判定の防止、worker強制終了時のAutoTrace停止を検証します。
