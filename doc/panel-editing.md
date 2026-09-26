# パネルの絵の直しかた

フロントパネルの絵は画像ではなく、その場で描いている。位置・大きさ・色は
**`panel.txt` という文字ファイルに追い出してある**ので、作り直さずに直せる。

## 手順

### 1. いまの配置を書き出す

```bash
build/gui.exe --dump-layout panel.txt
```

組み込みの配置（実機の写真から採寸したもの）がそのまま出てくる。
**これを出発点にする。**

### 2. 物差しを出す

```bash
build/gui.exe --shot panel.png --grid --layout panel.txt --size 1400x560
```

ROM は要らないので**0.1 秒くらいで絵が出る**。`--grid` を付けると
論理座標の方眼が重なる。50 ごとに細い線、100 ごとに濃い線と
`100,200` のような数字。直したい部品の位置を読み取る。

### 3. 直して、見る

`panel.txt` を書き換えて、もう一度 2 を走らせるだけ。**作り直しは要らない。**

動かしながら直すなら、窓を出しておいて

```bash
build/gui.exe <rom ディレクトリ> --layout panel.txt
```

`panel.txt` を保存してから**窓で F5 を押すと読み直す**。

### 4. 置き場

`--layout` を付けないときは、この順に探して最初に見つかったものを読む。

1. いま居るところの `panel.txt`
2. `gui.exe` と同じところの `panel.txt`
3. `%LOCALAPPDATA%\S-MU2000\panel.txt`
4. 付属の写真調の絵 `art/real/panel.txt`（exe の横、その一つ上、いま居るところの順）
5. プラグインの束の中 `S-MU2000.vst3/Contents/Resources/panel/panel.txt`
   （`make vst3` が art/real を写しておく）

どこにも無ければ組み込みの配置を使う。VST3 では 1・2 がホスト（DAW）の場所に
なるので、ふつうは 3 か 5 が使われる。DAW で自分の絵にしたければ 3 に置く。

## 書き方

`#` から行末は覚え書き。ただし `#rrggbb` は色なので残る。

### 座標のきまり

**論理座標 1000 × 400**。窓の大きさが変わっても、この 1000 × 400 が
窓いっぱいに収まるように一律で拡大縮小されるだけ。**窓の大きさは
気にしなくてよい**。縦 0 - 385 が本体、385 - 400 は面を切り替える帯。

### 位置

```
body_h 385                # 本体の高さ
lcd    240 42 439 135     # LCD の窓  x y 幅 高さ

cat.x  288 354 419 482 545 607    # 音色カテゴリの列（6 列）
cat.y  219 266 310                # その行（3 行）
cat.size 52 20                    # 押すところの幅と高さ

mode.play 752 66          # 右上の丸ボタン。中心の座標
mode.edit 806 66          # 名前は play edit util effect sampling seq
mode.r 11 5               # ボタンの半径と、中の LED の半径

nav.mute_solo 840 44 48 34    # 右端の四角いボタン。x y 幅 高さ
                              # 名前は mute_solo part- part+ enter
                              # select- select+ exit value- value+

round.select   694 262 22 22  # 小さい丸ボタン。select と audition

dial 893 268 58           # 大きなダイヤル  中心 x y と半径
volume 141 154 30         # 音量つまみ  中心 x y と半径
plg  524 37 341           # MU / PLG-1..3 の表示灯  左端 間隔 y
card 57 336 201 21        # カードの差し込み口。押すと MIDI ファイルの品書き
adin 8 44 60 130          # A/D INPUT のジャック
phones 198 238 74 82      # PHONES のジャック。押すと音の出口（デジタル / アナログ）の品書き
columns.y 186             # 窓の下の札（PART VOL EXP …）の高さ
modes.x 686               # 右の札（XG GS PERFORM）の左端。高さは液晶の ▶ に合わせる。負なら描かない

low.x 0 11.96 30.5 48.5 56.1 62.2 70.3 78.3 86.1 92.5 102.9    # LCD 下段の並び
low.w 10.12 15.64 15 4 4 7.2 6.8 6.7 6.8 8.1 1.3    # 単位は上段の点 1 つぶん（端数も可）
```

`low.x` `low.w` の並びは左から
`01` / `A01` / 楽器のかたち / VOL / EXP / PAN / REV / CHO / VAR / KEY / モード。
くわしくは [doc/lcd-segments.md](lcd-segments.md)。

### 飾り

ボタンでも LCD でもない、ただ描くだけのもの。**1 つでも書くと、
書いたものだけになる**（組み込みの飾りは消える）。上から順に描く。

```
text x y 幅 高さ 書体 揃え 色 "文字"
disc 中心x 中心y 半径 面の色 ふちの色 線の太さ
box  x y 幅 高さ 角の丸み 面の色 ふちの色
art  x y 幅 高さ "絵.svg"
```

* 書体 … `small` `label`
* 揃え … `left` `center` `right` `leftmid` `centermid` `leftwrap` `centerwrap`
* 色 … `ink` `face` `key` `keyedge` `keydown` `jack` `jackedge` `socket`
  `slot` `slotedge` `slotink` `black` `white`、または `#rrggbb`
* 文字の中の `\n` で改行（揃えを `leftwrap` か `centerwrap` に）

例。

```
text 10 6 170 26 label leftmid ink "YAMAHA"
disc 32 74 19 jack jackedge 2
box 57 336 201 21 2 slot slotedge
art 0 0 1000 385 "mu2000-mame.svg"
```

### SVG を貼る

`art` は **SVG をそのまま嵌める**。Inkscape で描いて書き出したものが
そのまま使えるので、数字を並べるより楽に凝ったものが作れる。

道は `panel.txt` からの相対で探す。縦横比は保ったまま、指定した四角の
真ん中に収める。

MAME の絵を部分的に直したいときは、部品ごとに分けた `art/mame/parts/` から始めると楽
（`tools/svgsplit.py` で作った。`art/mame/README.md`）。

読めるのは要るぶんだけ。

* `<path d="…">` の `M L H V C Z`（大文字小文字とも）
* `<rect>`（`rx` `ry` の角の丸みも）、`<circle>`、`<ellipse>`、`<polygon>`、`<polyline>`
* `transform` の `translate(…)` と `matrix(…)`
* `style` の `fill` `stroke` `stroke-width`

形は書いてある順に重ねる。`<!-- -->` の中は読まない（古い形を残しておける）。
弧（`A`）、二次ベジエ（`Q S T`）、勾配、文字、`<image>` は読まない。
**曲線は読み込むときに折れ線にする**ので、窓を大きくしても粗くならない。
点線（`stroke-dasharray`）は実線になる。

### つまみを SVG にする（回る）

`dial` と `volume` は、うしろに SVG を書くと**組み込みの絵の代わりに、
その絵を回して描く**。

```
dial   893 268 58 "dial.svg"
volume 141 154 30 "knob.svg"
```

* `dial` … ホイールを 1 目盛り回すと 15 度回る。実機のロータリー
  エンコーダと同じで、際限なく回り続ける
* `volume` … 左いっぱいで −135 度、右いっぱいで +135 度

絵は**真ん中を軸にして**回る。だから SVG の真ん中につまみの中心が
来るように描いておく。指標（線や点）を上向きに描いておけば、
値どおりの向きを指す。

回るものを描くだけなら、こんな小さな SVG で足りる。

```svg
<svg width="100" height="100" viewBox="0 0 100 100">
  <path style="fill:#c8bfa0;stroke:#6e6650;stroke-width:2"
        d="M 50,4 C 75.4,4 96,24.6 96,50 C 96,75.4 75.4,96 50,96
           C 24.6,96 4,75.4 4,50 C 4,24.6 24.6,4 50,4 Z" />
  <path style="fill:none;stroke:#5a5240;stroke-width:3"
        d="M 50,50 L 50,10" />
</svg>
```

### ボタンと表示灯を SVG にする

ようす（消えている／点いている／押している）ごとに 1 枚ずつ渡す。

```
mode.art  "btn.svg" "btn-on.svg" "btn-down.svg"   # 右上の丸ボタン（LED 入り）
nav.art   "key.svg" "key-down.svg"                # 右端の四角いボタン
cat.art   "cat.svg" "cat-down.svg"                # 音色カテゴリ
round.art "rnd.svg" "rnd-down.svg"                # SELECT と AUDITION
plg.art   "plg.svg" "plg-on.svg"                  # MU / PLG-1..3 の表示灯
```

* 1 枚目 … ふつう（消えている）
* 2 枚目 … 点いている／押している
* 3 枚目 … 押している（省くと 2 枚目で代える）

絵は部品の四角にそのまま当てはまる。**LED とボタンは 1 枚に描いてよい**
（実機も兼用なので、そのほうが描きやすい）。

`art/sample/` に見本が入っている。

```bash
build/gui.exe <rom ディレクトリ> --layout art/sample/panel.txt
```

### MAME の絵を借りる

MAME の `mu2000.lay` には、DIN コネクタ・ジャック・つまみ・カードの
差し込み口・下の通気口まで描いた SVG が埋まっている。**license:CC0-1.0**
（hap、Felipe Sanches）なので、そのまま使ってよい。

```bash
python tools/lay2panel.py <mame>/src/mame/layout/mu2000.lay mypanel
build/gui.exe --shot p.png --size 1400x560 --layout mypanel/panel.txt
```

`mypanel/panel.txt` と `mypanel/mu2000-mame.svg` が出てくる。MAME の
座標（1640 × 680）をこちらの論理座標へ移してあるので、**絵とボタンの
位置がぴたりと合う**。あとは普通の `panel.txt` なので手で直せる。

**起こしたものは `art/mame/` に入れてある**ので、そのまま使える。

```bash
build/gui.exe <rom ディレクトリ> --layout art/mame/panel.txt
```

見本は 2 つある。

| | 中身 |
|---|---|
| `art/sample/panel.txt` | こちらの採寸 ＋ SVG のボタン・LED・つまみ |
| `art/mame/panel.txt` | **MAME の絵**（DIN コネクタ、ジャック、通気口ほか）＋ 同じボタン |
| `art/real/panel.txt` | **実機の写真に合わせた絵**（既定。`tools/panel_art/make_panel.py` で作る） |

ボタン・LED・つまみの絵は両方とも `art/parts/` を指している。

## 変なことを書いたら

その行だけ飛ばして、`何行目: 理由` と教えてくれる。読めた行は反映される
ので、直して F5 を押せばよい。ファイルごと無くしても組み込みの配置に
戻るだけで、動かなくなることは無い。

## 表に無いもの

LCD の中身（点の大きさ、目盛りの帯、下段のセグメント）は `lcd` の四角から
**計算で出している**。窓を動かしたり大きさを変えれば、中身は全部ついてくる。

押せる場所も同じ表から作られる。**絵と当たり判定を別々に直す必要は無い。**

エディタ面とエフェクト面はまだコードの中（`src/ui/editor.cpp`、
`src/ui/effects.cpp`）。

## それでもコードを触るなら

組み込みの配置は `src/ui/layout.cpp` の `layout::layout()`。
描く仕組みは `src/ui/panel.cpp`、描くための小物は `src/ui/draw.h`
（`fill` `round_box` `disc` `text_in`）。
直したら `mingw32-make`、VST3 は `make install-vst3` で入れ直す。
