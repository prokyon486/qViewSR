# GLB表示（feature/glb-viewer）

Qt WidgetsのqViewSRに、Qt Quick 3Dの`RuntimeLoader`を`QQuickWidget`で組み込みます。
GLBは静的な初期姿勢で表示し、画像・GIFと同じフォルダー一覧・並び順で移動できます。
表示の回転、カメラ操作、PNG保存で元のGLBを書き換えることはありません。

## 操作

「開く」、ドラッグ＆ドロップ、最近使ったファイル、コマンドラインから`.glb`を開けます。

| 操作 | 動作 |
|---|---|
| Ctrl＋左ドラッグ | モデルの中心を軸に回転 |
| 左ドラッグ | カメラの平行移動 |
| ホイール | カメラが近づく／遠ざかる。画角は一定 |
| Shift＋ホイール | 画角を変更（5～120度）。カメラ位置は一定 |
| 中央クリック／「3Dの全体表示に戻す」／既存の表示リセット | 回転・平行移動・画角をリセットして全体表示 |
| ←／→ | 同じフォルダーの前／次の画像・GIF・GLB |
| ダブルクリック／既存の全画面操作 | 全画面。3D・超解像ツールバーは非表示 |
| Ctrl＋Shift＋S／「表示中の3DをPNG保存…」 | 現在の3D表示をPNGで保存 |
| 右クリック | 既存のコンテキストメニューと3Dメニュー |

PNGはメニュー・タイトル・ツールバーを除いた**3D表示領域**と同じアスペクト比・実ピクセル数です。
HiDPI画面では論理的なウィンドウ寸法とは異なります。保存寸法は下の3Dバーに表示します。
背景も含めた描画結果をsRGBプロファイル付きで保存します。GLB保存や3Dへの超解像は行いません。
静止画・GIFに戻ると、既存の超解像メニューを再表示します。
GLBを引数にして起動した場合はNCSを初期化しません。画像表示で準備した既存のworkerは再利用のため保持します。

## 表示範囲と制約

- glTF 2.0のGLBを対象とします。`.gltf`、FBX、OBJやアニメーションの再生・編集は対象外です。
- テクスチャ、標準PBR材質、初期ノード変換をQtのインポーターで読み込みます。
- 元の原点や単位に依存せず、読み込んだメッシュ全体の境界からカメラ距離を計算します。
  最初は画角45度です。未操作時のウィンドウ変更では全体表示を維持します。
- オリジナルの簡易スタジオ環境光と2灯の補助照明を使用します。他の3Dソフトの照明・描画とは見え方が異なります。
- 未対応の必須拡張を持つGLBは理由を表示して停止します。Draco、meshopt、BasisU等の圧縮GLBは、標準の非圧縮GLBで再出力してください。
  任意拡張の代替表示に関する注意はステータスのツールチップに出ます。
- **3D表示はsRGB出力です。既存の画像用モニターICC変換は3D描画には適用しません。**
  PNG自体にはsRGBプロファイルを設定します。画像・GIFのICC対応には影響しません。
- Qt 6.4のRuntimeLoaderはインポートをGUIスレッドで実行します。大きなGLBの読み込み中は操作に待ち時間が生じます。
  フォルダー内のGLBを先読みしたり、画像キャッシュへ入れたりはしません。
- 描画にはQt Quick 3Dに対応したGPU/ドライバーが必要です。NCSやOpenVINOは3D描画には使いません。

## Ubuntu 24.04でのビルド・起動

既存の開発環境に追加する場合:

```bash
sudo bash tools/install_3d_deps.sh
cmake -S . -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTS=ON
cmake --build build/gui --parallel 4
ctest --test-dir build/gui --output-on-failure
tools/run_qviewsr.sh /path/to/model.glb
```

新しい開発環境では`tools/install_build_deps.sh`が3D用依存パッケージも導入します。
3Dだけを試す場合は、上記のGUIビルドで足り、OpenVINOモデルやworkerの取得は不要です。
すでにランチャーを登録した環境でも`python3 tools/install_desktop.py`で登録を更新すると、GLBの「別のアプリケーションで開く」にqViewSRが加わります。

## 検証

```bash
# コンテナー検査、壊れたファイル、混在ファイル一覧: ctestで実行
# 実際のデスクトップ/GPUで操作・PNG・全画面・ナビゲーションを検証
tools/test_gui_3d.sh
# 自分のGLBもすべて表示し、診断用PNGを生成（入力ファイルは読み取り専用）
tools/test_gui_3d.sh /path/to/glb-folder
# HiDPIで同じ操作・保存寸法を検証
QT_SCALE_FACTOR=2 tools/test_gui_3d.sh
```

診断用PNGは`diagnostics/local/glb-captures`に保存し、Gitへ含めません。
テストは元GLBのSHA-256が読み込み・操作・保存後も同じことを確認します。
GPUテストには内部で生成する、原点から離れたテクスチャ付きキューブも使います。
CIは実行せず、pushするコミットには`[skip ci]`を付けます。

2026-09-26のローカル検証では、Ubuntu 24.04・Qt 6.4.2のデスクトップ上で、
ユーザー提供の28個（約42～54 MB）のGLBすべての表示・回転・PNG保存と元ファイルのハッシュ不変を確認しました。
通常表示とHiDPI 2倍でカメラ操作、保存寸法、全画面、画像との往復、NCS workerの起動抑止と再利用を検証しています。
アニメーション付きの生成モデルも、時間をおいた2回の描画が一致することを確認します。
既存の超解像・GIF・ファイル順序を含むCTestの4スイートも成功しています。

## 構成

- `QVImageCore`: 共通のファイル一覧を維持し、GLBは画像デコード・画像先読みから除外。
- `MainWindow`: 2D/3D表示、各メニュー、操作の振り分け。
- `Model3D::View`: カメラ・回転状態、GLB検査、キャプチャ、PNGの原子的な書き出し。
- `resources/model3d/Scene.qml`: Qt Quick 3Dの描画と照明。画像モードでは初期化しません。

Qt Quick 3DはGPLv3または商用ライセンスで提供され、本プロジェクトのGPLv3と両立します。
仕様参照: [RuntimeLoader](https://doc.qt.io/qt-6.5/qml-qtquick3d-assetutils-runtimeloader.html)、
[QQuickWidget](https://doc.qt.io/qt-6.5/qquickwidget.html)、
[glTF 2.0](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)。
