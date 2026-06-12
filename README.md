# BeatPath DX - リズム × タイル配置パズル

**▶ ブラウザで今すぐ遊ぶ: https://qyuweriot.github.io/BeatPath/**

> タイルを置くたびに音楽が重なっていく、インタラクティブミュージック型パズルゲーム。  
> C++17 / SDL2 製。外部アセット不要（音はすべてリアルタイム合成）。

---

## 遊び方

- 常にリズム(キック)が刻まれ、**毎小節の頭にスタート(緑▶)から球が発射**されます
- 球は **1拍ごとに1マス直進**。ギミックタイルを通ると効果が発動し、音が鳴ります
- 右のパネル置き場から盤面へ **ドラッグ&ドロップ** で配置(取り上げ・置き直し自由)
- **小節頭で全ゴールが同時点灯するとクリア**
- クリア後も発射は続き、完成した曲がそのままループします

### 操作
| 入力 | 動作 |
|---|---|
| マウス左ドラッグ | タイルの移動 |
| R | ステージリセット |
| N | クリア後、次のステージへ |
| M / ESC | ステージセレクトへ |
| - / + | 音量 |

## ギミック一覧

| タイル | 効果 | 音 |
|---|---|---|
| TURN R | 右折 | スネア (C4) |
| TURN L | 左折 | スネア (E4) |
| SPLIT | 左右に二分裂 | マリンバ (E5) |
| SPLIT 3 | 直進+左右の三分裂 | マリンバ (G5) |
| SPEED X2 | 1拍で2マス進む | タム (G3) |
| STOP 1 | 1拍停止 | タム (G3、低め) |
| PAINT (赤/青/黄) | 白い球（未着色）に色を付ける | オルガン (R=C5 / B=E5 / Y=G5) |

### ステージ側の要素
- **ワープ**: 入口と出口のセット。通ると上昇スイープ音
- **色付きゴール**: 同じ色の球だけが点灯させられる(白い球は素通り)
- **複数スタート**: 同時に複数の球が走る

## ステージ構成 (8ステージ / セーブ対応)

| # | 名前 | 拍子 | テーマ |
|---|---|---|---|
| 1 | FIRST STEPS | 4 | 曲がる |
| 2 | SPLIT | 4 | 分裂で2ゴール同時 |
| 3 | SPEED | 4 | 加速して2ゴール |
| 4 | TRIPLE | 6 | 三分裂で3ゴール |
| 5 | WARP | 4 | ワープを経由 |
| 6 | COLORS | 6 | ペイントして同色ゴールへ |
| 7 | DUET | 8 | 2スタート×5ゴール |
| 8 | FINALE | 8 | ワープ+色+速度系の総決算 |

クリア状況はブラウザ版では `localStorage` に、ネイティブ版では `beatpath_save.dat` に自動保存されます。

## インタラクティブミュージックの仕組み

- **5レイヤー構成**: キック(常時) → 8分ハット → ベース → コードパッド → メロディ
  - 球が生存している間 → ハット追加
  - タイルを1枚置く → ベース追加
  - タイルを2枚以上置く → コードパッド追加
  - クリア → メロディ追加+クリアコード
- **Am → F → C → G の4小節コード進行**(画面上部に現在のコードを表示)
- **球が2個以上**になると16分シェイカーが追加される(球数でレイヤーが増える)
- ワープ=スイープと、**ステージ要素自体が効果音**になる
- 拍はオーディオスレッドのサンプル精度カウンタで駆動するため音とズレません
- ドラッグ&ドロップにも取る/置く/戻すで音程の違うクリック音

---

## ビルド方法 (ローカル実行)

### macOS (Apple Silicon / Homebrew)
```bash
brew install sdl2
make && ./beatpath
```
※ `/usr/local` にIntel版SDL2しか無い場合は `make SDL2_CONFIG=/opt/homebrew/bin/sdl2-config`

### Ubuntu / Debian
```bash
sudo apt install g++ make libsdl2-dev
make && ./beatpath
```

### Windows (MSYS2 / MinGW64)
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 make
make && ./beatpath.exe
```

### Web ブラウザ (Emscripten)
```bash
# Emscripten のインストール (初回のみ)
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
source ~/emsdk/emsdk_env.sh

# ビルド → docs/ に index.html / index.js / index.wasm が生成される
make -f Makefile.em

# ローカル確認
npx serve docs
```
