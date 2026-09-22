# NCS実機検証と環境構築

2026-09-22 JSTの初回調査、権限修正後の単独試験、およびGUI接続後の試験。**単独推論と4本並列でのGUI表示・保存が成功した。** 長時間負荷や配布版全体の品質保証を意味しない。

## 確認結果

ホストはUbuntu 24.04.5 LTS x86_64、kernel 6.17.0-35、X11。初回時点でQt/CMake開発環境とシステムOpenVINOは見つからなかった。

| USBポート | 起動前VID:PID | OpenVINOのID | 権限修正後 |
|---|---|---|---|
| 5-1.1 | 03e7:2485 | 5.1.1-ma2480 | NCS2、単独推論成功 |
| 5-1.2 | 03e7:2485 | 5.1.2-ma2480 | NCS2、単独推論成功 |
| 5-1.3 | 03e7:2150 | 5.1.3-ma2450 | NCS、単独推論成功 |
| 5-1.4 | 03e7:2150 | 5.1.4-ma2450 | NCS、単独推論成功 |

4本ともVIA hubのUSB2側（480Mbps）で列挙された。ハブのUSB3側も別に5000Mbpsで見えている。起動前の列挙情報だけでハブ故障や推論時の帯域不足を断定しない。firmware boot後に再取得する。

初回USB nodeはroot:root、0664で、実行ユーザーに書込権限がなかった。sysfsによる列挙とOpenVINOのID取得は成功したが、NCS2 5.1.1への1032推論は`Failed to find booted device after boot` / `Can not init Myriad device: NC_ERROR`で失敗。ユーザーが専用udev ruleを適用後、全4本でread/write access=trueとなり、同じモデルの推論が成功した。

Codexの制限環境内ではUSB device nodeが見えず`lsusb`が失敗したため、実機診断はホストへの許可された実行で行った。制限環境から見えないことをデバイス故障と解釈しない。

## runtimeとモデル

- OpenVINO 2020.3.2の公式Ubuntu18向けアーカイブを作業フォルダー内に展開。システムライブラリーは未変更。
- g++でQt/OpenCV不要の`ncs-probe`をビルドできた。`inference_engine`、`inference_engine_legacy`、同梱nGraph/TBBでロード可能。
- 1032 FP16のXML/BINがOMZ 2020.3のhashと一致。IR version 10とtensor shapeを確認。
- **CPUの定数画像smoke testは成功**。入力128（BGR全channel）、出力mean=0.50205452、min=0.42816615、max=0.84511781。モデルload等は約9504ms、単発Inferは約134ms。これは1回の動作確認で、写真品質・定常性能・NCS比較の結果ではない。
- **NCS/NCS2全4本の単独smoke testが成功**。4本並列・タイル境界・保存の追加試験は下記参照。連続負荷は未実施。
- `myriad_compile`で2450/2480両方へのoffline compileもexit 0で成功。入出力precisionはprobeと同じFP32に明示した。

各個体の定数画像試験（1回、cold/warmを揃えた性能比較ではない）:

| device | load等 ms | Infer ms | 出力mean | 結果 |
|---|---:|---:|---:|---|
| 5.1.1-ma2480 | 2794.2 | 899.6 | 0.50207228 | smoke_pass / exit 0 |
| 5.1.2-ma2480 | 2736.9 | 901.2 | 0.50207228 | smoke_pass / exit 0 |
| 5.1.3-ma2450 | 2538.6 | 2064.2 | 0.50201458 | smoke_pass / exit 0 |
| 5.1.4-ma2450 | 2004.9 | 917.5 | 0.50201458 | smoke_pass / exit 0 |

初代2本の単発時間に差があるので、この表から固定の配分比を決めない。CPUのmeanともわずかな差があり、写真の同一タイルについて世代間の差を次段階で検証する。

## 再実行

リポジトリーrootで実行する。初回のローカル配置は`/home/janis/qViewSR/qView`。`../references/`はフォークしたOMZ/OpenVINOの調査用checkout、`../.local/`は今回取得したruntime/model。

### 1. 固定アセットの取得

```bash
python3 tools/ncs/fetch_assets.py --dest ../.local
```

約198MBの公式runtimeと小さなモデルを取得し、size/SHA-256を検証する。既存の一致するダウンロードは再利用する。既存の展開先は上書きせず、展開済みライブラリーを再照合したとは報告しない。OSへのインストール、root、Docker、pipは不要。Python 3.12以上を使用。

runtimeのhashは公式HTTPSから取得したファイルをローカル計算したもの。モデルhashはOMZ当時のmanifestに掲載された値。両者の由来は`source-lock.json`に記録している。

### 2. USB権限

```bash
python3 tools/ncs/usb_inventory.py
sudo bash tools/ncs/install_usb_access.sh
python3 tools/ncs/usb_inventory.py
```

`install_usb_access.sh`は`/etc/udev/rules.d/70-qviewsr-ncs.rules`を設置し、VID 03e7の3種類のPID（2150/2485/f63b）だけにactive local session向け`uaccess`を付ける。udev reloadと該当VIDのadd eventを実行する。既存の同名ファイルが異なる場合は上書きせず停止する。

sudoのパスワードはローカルのターミナルで入力する。チャットへ渡さない。初回の自動実行では`sudo: a password is required`となったため、この操作はユーザー実行が必要だった。

各deviceの`read_write_access: true`を確認する。反映されなければログイン中のデスクトップでhubを抜き差しして再確認する。boot後はPIDが変わるため、現在のbus/device番号への一時的chmodだけでは不十分。SSH/headlessなどactive seatのない環境は別のgroup-based ruleを検討する。

設定を戻す場合はこの専用ルールだけを削除し、udev reload後にNCSを再接続する。他製品のudev rulesを変更しない。

### 3. probeのビルドと列挙

```bash
qviewsr_runtime=$(realpath ../.local/openvino-2020.3.355/l_openvino_toolkit_runtime_ubuntu18_p_2020.3.355)
bash tools/ncs/build_probe.sh "$qviewsr_runtime"
bash tools/ncs/run_probe.sh "$qviewsr_runtime"
```

`event=device`は列挙成功だけを意味する。設定スクリプトとprobeはrootで常時実行しない。probeは`timeout`で120秒、終了しなければさらに5秒後にkillする。強制USB resetは行わない。

### 4. 個体ごとの最小推論

列挙で得た現在のIDを使用する。以下のIDは初回の例。

```bash
qviewsr_model=$(realpath ../.local/models/1032-fp16/single-image-super-resolution-1032.xml)
bash tools/ncs/run_probe.sh "$qviewsr_runtime" --device 5.1.1-ma2480 --model "$qviewsr_model" --infer
bash tools/ncs/run_probe.sh "$qviewsr_runtime" --device 5.1.2-ma2480 --model "$qviewsr_model" --infer
bash tools/ncs/run_probe.sh "$qviewsr_runtime" --device 5.1.3-ma2450 --model "$qviewsr_model" --infer
bash tools/ncs/run_probe.sh "$qviewsr_runtime" --device 5.1.4-ma2450 --model "$qviewsr_model" --infer
bash tools/ncs/run_probe.sh "$qviewsr_runtime" --device CPU --model "$qviewsr_model" --infer
```

同時起動はせず1本ずつ試す。smoke testは定数128を2入力へ詰める。定数画像のbicubicは同じ定数になるため、OpenCV不要。shapeとprecisionを検査し、有限な出力と妥当なmeanを確認する。`event=smoke_pass`とexit 0の両方を成功条件にする。数値の色/画質検証は別途写真とreference出力が必要。

probe exit: 0=列挙または指定smoke成功、1=エラー、2=MYRIAD列挙0台、124/137=timeout系。旧runtimeのログはstdoutにも混ざり得るため、製品IPCのparser試験にはこのツールを使わない。

## GUIと並列workerの追加検証

Qt 6.4.2、CMake 3.28.3、LittleCMS2 2.14、g++ 13.2でビルドした。`tools/build_qviewsr.sh`でviewer・workerと自動試験を再実行できる。

| 試験 | 結果 |
|---|---|
| TilePlan | 1×1、奇数、固定shape前後など64寸法組合せ。全pixelが一度だけ書かれ、reflect101が範囲内。合格 |
| Qt GUI + fake worker | ICC変換・alpha、ボタン開始、切替・倍率保持、PNG/JPEG ICC、別画面ICCで保存不変、回転保存、中止、ウィンドウ終了、異常終了、hash不一致。合格 |
| 実worker・全4本 | 写真＋色帯＋文字＋線の897×477入力 → 3588×1908出力、halo 16、9タイル、14.289秒（worker単発、モデルload含む） |
| Qt GUI → 実機4本 | 上記入力で開始・完成表示・切替・PNG/JPEG保存。offscreenとX11の両方で合格。X11試験全体19.351秒（起動・テスト操作・保存含む） |
| CPU境界比較 | OMZ写真480×270の単一入力halo 0と、halo 16で4タイル化した出力を比較。外周64 HR pixelを除く内部は画素一致。内部境界x=1792 / y=952も一致 |

全4本の9タイルは、`5.1.1-ma2480`が2、`5.1.2-ma2480`が3、`5.1.3-ma2450`が2、`5.1.4-ma2450`が2。各個体のrequestが動的キューから取得した実績で、固定配分ではない。

CPU比較はこの1枚の境界処理の検証。初代/NCS2の丸め差、任意の内容、透明境界、halo値全般の保証には拡張しない。上記時間は各1回の動作確認であり、CPU優位・4本による高速化・warm性能を結論しない。

実機GUIテストの再実行:

```bash
./tools/test_gui_hardware.sh /path/to/test.png
QT_QPA_PLATFORM=xcb ./tools/test_gui_hardware.sh /path/to/test.png
```

Qt試験の設定は一時フォルダーに隔離する。生成PNG/JPEG、ウィンドウだけのスクリーンショット、デバイス別タイル数は`diagnostics/local/gui-check/`。実行時の入力画像を外部へ送信しない。

次に評価する項目は、写真セットでのCPU/NCS/NCS2の数値・見た目比較、NCS2×2と全4本の待ち時間、30分負荷、抜去・再接続、実モニターICCの検証。GUIでは現在の結果を確認しながら、デバイスを切り替えて試せる。

生ログと画像は`diagnostics/local/`へ保存し、Gitには含めない。共有する技術情報だけを本書に記録する。
