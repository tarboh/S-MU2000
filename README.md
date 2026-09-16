# S-MU2000

Yamaha MU2000 のソフトウェア音源。DAW に挿して使えることを目指す。

**現在の状態: VST3・CLAP（Windows）と VST3・Audio Unit（macOS）として DAW に挿して鳴る。実機のフロントパネル風の画面と、PC で触るエディタが付いた。**

作りかけを晒しながら進めている。X では `#S_MU2000`。

> ヤマハとは無関係の非公式なプロジェクト。Yamaha・MU2000・XG はヤマハ株式会社の商標。

## 実機由来のデータは、配らない・載せない

**このリポジトリが公開しているのは、エミュレータと吸い出し道具のソースコードだけ。**
ヤマハの ROM・波形データ・ファームウェアは、このリポジトリにも配布物にも入っていないし、
今後も入れない。動かすのに要る ROM は、利用者が**自分の持っている MU2000 から各自で取り出す**。

ソースコードを公開することと、実機から取り出したデータを公開・共有することは**別の話**で、
このプロジェクトは後者をしない。使う人にも次をお願いする。

* 吸い出した波形 ROM・プログラム ROM のイメージを、GitHub その他どこにも再配布しない
* 実機由来の ROM イメージを、Issue・Pull Request・Discussion・Release・添付ファイルなどに
  載せない。不具合の報告は、ハッシュ値・ログ・MIDI ファイル・録音で足りる
* 吸い出しの途中で作ったカスタムファームウェア、改変したファームウェアイメージ、
  カスタムの `.ydl`（ダンパなど）も配らない。どれも純正ファームウェアを含んでいる
* ヤマハの更新プログラム（`mu2r1_uw.zip`）は、ヤマハの配布ページから各自で入手する

これらの実機由来のデータは、S-MU2000 のソースコードとは扱いが別で、
下の「由来とライセンス」の対象でもない。

> **Notice (English).** This repository contains source code only. It does not include
> or distribute any Yamaha ROM, wave data or firmware, and never will. To run it, you
> extract the ROMs from your own MU2000. Publishing this source code and sharing data taken
> from the hardware are separate matters; this project does only the former.
> Please do not upload ROM images, wave dumps, custom or modified firmware images, or
> custom `.ydl` files anywhere, including Issues, Pull Requests, Discussions, Releases and
> attachments. Hashes, logs, MIDI files and recordings are enough for bug reports.

実機の firmware をそのまま走らせ、MIDI を受けて発音する。音は実機を S/PDIF で
録ったものと直接比べて詰めている（いずれも 1/3 オクターブの帯ごとの差）。

* XG の GM 128 音色と、そのほかの XG 音色 1225 種（C2・C4・C6）。C4 では 1225 種のうち
  1109 種が差 1.5dB 未満。残る 116 種のうち 60 種は、実機で録り直しただけでも同じくらい揺れる
* パフォーマンスモードの 100 パフォーマンス。帯の差の平均 0.61dB（実機どうしの再現性は 0.22dB）
* システムのリバーブ・コーラスの全種類、バリエーションを送りで使う形、コントローラ、
  ドラムの楽器ごとの NRPN、マルチ EQ、128 声を超えたときの奪い合い

合わないものと、調べた経過は [doc/todo.md](doc/todo.md) に残してある。

音源の JIT（SH2 と MEG の命令を機械語に訳す）が入り、16 パートが鳴りっぱなしの
試験曲でも CPU の使用率は実時間のおよそ 22%（Ryzen 7 9700X、1 ブロックの平均）。
LCD は firmware が書いたものがそのまま出て、ボタンもダイヤルも触れる。

## これは何か

MU2000 の中身（SH7043 CPU + SWP30 音源チップ ×2）をソフトウェアで動かし、
実機の firmware をそのまま走らせる。エミュレータなので、音色も挙動も実機由来になる。

MAME でも MU2000 は鳴る。だが MAME は自分で時計を持って実時間に追いつこうとする
つくりで、DAW や外部シーケンサと同期させると遅れが溜まって破綻する（実測で
処理能力に 177% の余力がありながら平均速度が 98% 台から上がらない）。
ソフトシンセはホストのオーディオコールバックに駆動されるので、この問題が原理的に起きない。

くわしくは [doc/design.md](doc/design.md)。残っているものは
[doc/todo.md](doc/todo.md) に、直す順で並べてある。

## ROM について

**ROM は同梱しない。** 利用者が自分の MU2000 から吸い出す必要がある。
吸い出したものの扱いは、上の「[実機由来のデータは、配らない・載せない](#実機由来のデータは配らない載せない)」のとおり。

| ROM | 内容 |
|---|---|
| プログラム ROM | 4MB（本体の firmware） |
| 波形 ROM | 32MB（音色データ） |

**吸い出しの手順と道具は [doc/dump/](doc/dump/) に入っている。**

rom ディレクトリには次を置く。

| ファイル | 中身 |
|---|---|
| `mu2000_flash.bin` | プログラム ROM 4MB（CPU から見えるまま） |
| `dump/xv364a0.ic49` ほか 3 つ | 波形 ROM 8MB × 4 |
| `standin/sin-table.bin` | MEG が使う sin 表 64KB |

* プログラム ROM は**吸い出さなくていい**。ヤマハが公開している更新プログラム
  （`mu2r1_uw.zip`）から復元できる。中身は Flash 書き込みの SysEx をそのまま
  収めた MIDI ファイルで、組み直すと MAME 登録の SHA1 に一致する
* 波形 ROM は **USB ケーブル 1 本で約 36 分**。分解も MIDI インターフェースも
  要らない。自作のダンパを本体ファーム領域だけに書き込み、SWP30 の
  wave direct access で読んだものを USB へ流す。ダウンローダには触らないので
  純正アップデータでいつでも戻せる（が、ファーム書き換えなので自己責任で）

MIDI 経由の予備の経路もあり、両方で吸ったものが 1 バイト残らず一致することを
確かめてある。

## 使い方

**はじめて使うなら [doc/manual.md](doc/manual.md)**（用意するもの → 作る → 鳴らす → 画面 → DAW の通しの手引き。English: [doc/manual.en.md](doc/manual.en.md)）。

```
make

build/gui.exe    <rom ディレクトリ> [--midi 番号] [--midi-b/-c/-d 番号]  実機パネル風の画面で鳴らす
                 [--host-midi]                      USB ではなく DIN の口（A・B だけ）で受ける
                 [--play 曲.mid]                    MIDI ファイルを流す
                 [--exclusive] [--audio 名前] [--latency ms]  音の出口（下の「待ち時間」）
                 [--editor] [--list-window]         PC で触る窓を開いて起動
                 [--factory] [--nomidi] [--fast-midi]
build/gui.exe    <rom ディレクトリ> --lcd              LCD だけの画面で鳴らす
build/gui.exe    --list                              MIDI の入口と出口・音の出口の一覧
build/live.exe   <rom ディレクトリ> [--midi 番号] [--fast-midi]  画面なしで MIDI 入力を受けて鳴らす
build/live.exe   --list                              MIDI 入力の一覧
build/render.exe <rom ディレクトリ> <MIDI> <出力 wav> [秒数]  ファイルを WAV に
                 [--reset gm|gs|xg]                 リセットを明示して先頭に入れる
                 [--usb]                            USB の口で起動し、曲の口 1-4 を A-D へ渡す
                 [--fast-midi]                      firmware が読める速さで MIDI を渡す
                 [--card 絵.img] [--adc-in 入力.wav]  SmartMedia を差す／A/D INPUT に流す
build/panel.exe  <rom ディレクトリ> [--keys "play,edit"] [--list]  パネルを文字だけで動かす
build/boot.exe   <rom ディレクトリ> [サイクル数]       起動の確認
build/statetest.exe <rom ディレクトリ> [MIDI]          状態の保存と復元が正しいかを確かめる
build/blocktime.exe <rom> <MIDI> <フレーム数> [秒] [回数]  1 ブロックの所要時間を測る
build/midisend.exe <MIDI ファイル> [--port 番号]      MIDI 出力へ実時間で流す
build/rec.exe    --list                              音声入力の一覧
build/rec.exe    <番号> <wav> <秒> [--send <番号> <MIDI>]  実機の音を録る
```

**Domino など外のシーケンサから鳴らす手順は
[doc/domino.md](doc/domino.md)**。要るのは仮想 MIDI ケーブル（loopMIDI）
ひとつだけ。`gui.exe` は入口と出口を**動かしたまま画面から選べる**ので、
パネルの `MIDI IN A` のジャックを押すか、窓のどこかを右クリックする。

入口は **A〜D の 4 口**（パート 1-16・17-32・33-48・49-64）。実機の HOST SELECT を
USB にしたときと同じ形で起動するので、実機では USB でしか使えない C・D も使える。
`--host-midi` を付けると DIN の口（A・B の 32 パート）で起動する。THRU の出口も口ごとに選べる。
1 本の入口からでも、ケーブルメッセージ `F5 nn`（nn = 1〜4）を送れば以後のメッセージが口 A〜D へ行く
（MU128 などの TO HOST と同じ流儀。実機の MU2000 は USB で送った `F5` を無視する。[doc/dump/usb.md](doc/dump/usb.md)）。
選んだものは `%LOCALAPPDATA%\S-MU2000\gui.ini` に覚えておく。パネルの VOLUME の
つまみの位置もここ（実機でもアナログのつまみで、firmware の RAM には入らない）。

パネルのほかに、マウスとキーボードで触る窓がある（一覧・エディタ・インサーションの設定・
パートの音色。F2・F3 か右クリック。[doc/pc-editor.md](doc/pc-editor.md)）。
SmartMedia の差し込み口と、サンプリング用の A/D INPUT も使える（[doc/gui.md](doc/gui.md)）。
MIDI ファイルは窓に落とすか `--play` で流せる。

**MU2000 の設定は電源を入れ直しても残る。** 実機の電池で保持される RAM と同じ
ものを、`gui` と `live` が終わるときに `%LOCALAPPDATA%\S-MU2000\nvram\` へ残し、
次の起動で使う。ユーティリティの設定も、XG のマスタボリュームのような
値も残る（実機の firmware がそう作ってある）。プラグイン（VST3・CLAP・AU）はここを**読むだけ**で、
挿したときは gui / live で作った設定から始まる（プラグインの中で変えたものは DAW の
プロジェクトに残る）。工場出荷状態に戻すには `--factory` を付けて起動するか、
`gui` の窓を右クリックして「工場出荷状態に戻す」。ファイルを消しても同じ。

画面の中身は [doc/gui.md](doc/gui.md)。3 面ある。
**パネルの絵は作り直さずに直せる**。位置も色も `panel.txt` という文字
ファイルに追い出してある（[doc/panel-editing.md](doc/panel-editing.md)）。

* **パネル** … 実機のフロントパネル（LCD・ボタン 35 個・大きなダイヤル）
* **エディタ** … SOL2 風。パート別のフィルタ・EG・エフェクト送り・音色
* **エフェクト** … リバーブ／コーラス／バリエーションと、インサーション 2 系統
  （番地は実測で確かめてある。[doc/effects.md](doc/effects.md)）

プラグインの画面も同じもの。

`live` は音声デバイスが要求した分だけ音源を進める。自分で時計を持たないので、
外部と同期させてもずれない（MAME が破綻したのはここ）。

**出力はデバイスが言ってくる形式のまま開く。** 48000Hz を言ってくる機械では
44100 からの変換を自前の sinc でやる（Windows の変換器を通さない）。

## DAW に挿す

作り方と ROM の置き場は [doc/vst3.md](doc/vst3.md)。DAW ごとに分かったことは
[doc/reason.md](doc/reason.md)（Reason）・[doc/sonar.md](doc/sonar.md)（Cakewalk Sonar）。

**VST3**。置き場は `%LOCALAPPDATA%\Programs\Common\VST3`（利用者ごと）か
`C:\Program Files\Common Files\VST3`（全員）。ROM は同梱できないので、
バンドルの `Contents/Resources/roms.txt` に置き場所を 1 行書く。
入力は実機の MIDI IN A〜D と同じ 4 本（64 パート）。Cubase のように MIDI の
プログラムチェンジを `IUnitInfo` の音色の一覧で扱うホストでも、パートごとに音色が替わる。

**音色やエフェクトの設定はプロジェクトに残り、パートの音量・フィルタ・EG・EQ やマスター EQ などは
名前付きのパラメータとしてオートメーションで動かせる**（VST3・CLAP。画面で触った値もホストへ伝わる）。
[doc/automation.md](doc/automation.md)。

```
make vst3             build/S-MU2000.vst3/ にバンドルができる
make install-vst3     VST3 の置き場へ複製する
make probe            DAW 無しで読み込みと発音を確かめる
build/vst3probe.exe <バンドルの中の DLL> --torture
                      ホストの無茶な呼び方を一通り試す（DLL は Contents/x86_64-win/S-MU2000.vst3）
```

**CLAP**（Windows で確かめた。macOS 用の `make clap` も書いてあるが、まだ macOS のホストで試していない）。
中身は VST3 版と同じで、MIDI はバイト列のまま受け取る。ノートの入力も A〜D の 4 本。
置き場は `C:\Program Files\Common Files\CLAP`（全員）か
`%LOCALAPPDATA%\Programs\Common\CLAP`（利用者ごと）。ROM の置き場は
`S-MU2000.clap` のすぐ横の `roms.txt` か、`%LOCALAPPDATA%\S-MU2000\roms.txt` に 1 行書く。

```
make clap             build/S-MU2000.clap ができる
make install-clap     CLAP の置き場へ複製する
build/clapprobe.exe build/S-MU2000.clap <MIDI> <出力 wav>
                      DAW 無しで鳴らす（ROM の場所は環境変数 S_MU2000_ROMS でも渡せる）
```

Windows では VST 2.4 instrument DLL も作れる。中身と画面は VST3・CLAP と共通で、
廃止された SDK は使わず、必要なバイナリ ABI だけを `src/vsti/vst2_abi.h` に定義した。
MIDI / SysEx は MIDI IN A に入る。詳しくは [doc/vsti.md](doc/vsti.md)。

```
make vsti           build/S-MU2000.dll を作る
make vsti-probe     DLL の読込み・MIDI・状態・画面をホスト無しで確かめる
make install-vsti   VSTI_INSTALL（既定は Program Files/VstPlugins）へ複製する
```

プラグインも既定で USB の口（A〜D）で起動する。DIN の口（A・B）に戻すときは、
`%LOCALAPPDATA%\S-MU2000\plugin.ini` に `usb=0` と書く。

**macOS**（Apple silicon）では VST3 と Audio Unit（AUv2、`aumu`）が作れる。
`make` で道具と両方のバンドルができる。くわしくは [doc/porting-macos.md](doc/porting-macos.md)。

## 待ち時間

**実測で 117ms → 16ms まで詰めた**（MIDI を受けてから音が出るまで。実機と
並べて波形で測った値）。生演奏に使える。

| 耳に届くまでの実測 | 最初 | いま |
|---|---|---|
| 独り占め（`--exclusive`） | 117ms | **16ms** |
| 共有モード | 約 110ms | **50ms** |

共有モードの 50ms は、広めの部屋で手を叩いたときの反射音くらいの遅れ。実機と
同時に鳴らして比べない限り、共有モードのままでも演奏に使える。

要るのは次の 3 つ。

**1. `--exclusive`（デバイスを独り占め）**

Windows の混ぜ合わせを通らない。同じ機械で並べるとこれだけ違う。

下の表は、こちらから見て「書いたのにまだ鳴っていない量」。耳に届くまでの
実測（上の表）には、ここにインターフェースと空気のぶんが足される。

| | 出るまで（書いたのに鳴っていない量） |
|---|---|
| 共有 `--latency 20`（既定） | 48ms |
| 共有 `--latency 10` | **38ms**（共有の底） |
| 独り占め `--latency 10` | **11.6ms**（周期そのもの。余分なし） |

代わりに、鳴らしている間は他のアプリが音を出せない。

**共有モードの 38ms から先は縮まない。** うち 10ms はこちらの溜め（周期 1 つ
より短くはできない）で、残り約 28ms は Windows の混ぜ合わせとドライバが
抱えている分。`IAudioClient3` の低遅延の口（この機械では 441 フレーム固定で
既定と同じ）も、`--raw`（エンジンの信号処理を飛ばす）も効かなかった。

共有モードで詰めるなら `--latency 10`。**溜めを厚くしても安全にはならない**
（締め切りは周期が決めるので、20ms でも 10ms でも余裕は同じ 10ms）。
違うのは「合図を丸ごと 1 回取り逃したときに耐えられるか」だけ。

**2. 音の出口を指定する（`--audio`）**

Windows の「既定の再生デバイス」は勝手に変わる（実際、設定を触ったら別の口に
移った）。`--list` で名前を見て、`--audio "Analog (3+4)"` のように**名前の
一部**で指定する。番号ではなく名前で覚えるのは MIDI の口と同じ理由。
画面から起動したときは `gui.ini` に覚えるので、一度指定すれば次からは要らない。

**3. インターフェース側の設定**

ここは S-MU2000 からは触れない。RME なら Fireface USB Settings で:

* **Buffer Size** … 1024 サンプルだと 21ms。**256 まで下げる**
* **Sample Rate を 44100 に** … MU2000 は 44100 でしか動かない。カードも
  44100 にすると、こちらの変換が丸ごと消える（`変換 無し（44100 のまま）`
  と出る）。実機と S/PDIF で比べるなら、そもそも合っていないと比較にならない

### `--latency` の決め方

**溜める目標の長さ**。独り占めでは**デバイスの単位（2 の冪のフレーム数）に
丸める**。業務用の機械は 128/256/512/1024 で動いていて、割り切れない長さを
渡すと切り刻まれたような音になる（480 フレームを渡して実際にそうなった）。

| | 周期 | 音源の最悪 | 具合 |
|---|---|---|---|
| `--exclusive --latency 10` | 11.6ms（512） | 5.4ms | 落ちない |
| `--exclusive --latency 5` | 5.8ms（256） | 3.4ms | たまに間に合わない |

余裕は音源の 1 ブロックの最悪値より長くないと音が切れる。報告の「間に合わなかった」が
増えるなら `--latency` を伸ばす。1 ブロックの所要時間は `build/blocktime.exe` で測れる。

この節の待ち時間と表の数字は、JIT を入れる前（2026-09-13）に測った値で、まだ測り直していない。
音源の 1 ブロックはその後速くなり、16 パート同時の試験曲 `dense` を 512 フレームずつ回すと
平均 2.5ms・最悪 7.2ms（2026-09-17、Ryzen 7 9700X）。

## ビルドについて

Windows は MSYS2 / MinGW-w64 の g++、macOS は Apple の clang++、Linux は g++ を想定している。C++20 が要る。
`make test` で回帰試験が回る（[doc/testing.md](doc/testing.md)）。ROM が無い
機械でも、ROM の要らない分だけは走る。

### Linux で作る

> **Linux の画面とプラグインについて（PR [#33](https://github.com/tarboh/S-MU2000/pull/33)）**
> Linux の `gui`・VST3・CLAP は spessasus さんの寄稿で、作者は Linux を使っておらず、
> 動作を確かめることも、面倒を見ることも、**責任を取ることもできない**。使うのは自己責任で。
> 不具合の報告や直しは、Linux を使っている人からの issue・PR を歓迎する。
>
> **Linux GUI and plug-ins (PR #33).** These were contributed by spessasus. The maintainer does not use
> Linux and **cannot test, support, or take responsibility for them**. Use them at your own risk.
> Reports and fixes from Linux users are welcome.

Debian/Ubuntu では次を、Arch ではその下のを入れる。

```
sudo apt install build-essential libasound2-dev libcairo2-dev libfontconfig-dev libsdl3-dev
sudo pacman -Sy base-devel alsa-lib cairo fontconfig sdl3
```

`make` で `build-linux/` に道具一式と `gui`、VST3・CLAP ができる。ROM は
`roms/` に置く（中身は「[ROM について](#rom-について)」と同じ）。

```
build-linux/gui roms                       実機パネル風の画面で鳴らす（表示は英語。F2・F3 で PC の窓）
build-linux/gui roms --boot --shot out.png 画面の絵だけ書き出す
make vst3 / make clap                      DAW に挿す形（画面は汎用のもの）
make probe                                 DAW 無しで読み込みと発音を確かめる
make test                                  回帰試験（ROM と numpy が要る。Debian は python3-numpy、Arch は python-numpy）
```

VST3・CLAP の ROM は `S_MU2000_ROMS` 環境変数か、バンドルの横の `roms.txt`
（`~/.vst3`・`~/.clap` へ入れたものは `~/.local/share/S-MU2000/roms.txt`）
で場所を教える。待ち時間・MIDI の選び方など使い方は Windows 版と同じ。
くわしくは [doc/linux.md](doc/linux.md)（道具と `live`）と
[doc/porting-linux-gui.md](doc/porting-linux-gui.md)（画面）。
Windows の exe は **MSYS2 の DLL に依存しない**ように静的リンクしてある
（動的リンクのままだと、素の PowerShell から起動しても何も言わずに終わる）。

SH2 と MEG の JIT は x86-64 と arm64 の両方にある。`midisend` と `rec`（実機と比べるための道具）は
Windows だけ。macOS のビルドは [doc/porting-macos.md](doc/porting-macos.md)。

## 由来とライセンス

中核となるチップの実装は **MAME から取り込んでいる**。MAME 全体は GPL だが、
必要な個々のデバイス実装はすべて **BSD-3-Clause** で、流用が認められている。

| 取り込み元 | 著作権 |
|---|---|
| `src/mame/sound/swp30.*` | MAME `src/devices/sound/swp30.*` — Olivier Galibert |
| `src/mame/cpu/sh*` | MAME `src/devices/cpu/sh/` |
| `src/mame/machine/sci4.*` | MAME `src/devices/machine/sci4.*` |
| `src/mame/video/hd44780.*` | MAME `src/devices/video/hd44780.*` — Sandro Ronco |
| `src/mame/ymmu2000.cpp` | MAME `src/mame/yamaha/ymmu2000.cpp` |

`src/mame/cpu/sh*` は Olivier Galibert と David Haywood、パネルの絵
（`art/mame/`）は hap と Felipe Sanches（CC0-1.0）。取り込んだ側の改変には
`S-MU2000:` の印を付けてある。

取り込み元は MAME 0.289 相当（master 2026-09-06、コミット `1fb001f9`）。
MAME 本体はリンクしない（GPL のため）。VST3 は口の定義（MIT）だけを使い、
GPLv3 と Steinberg 独自ライセンスの二択になる SDK 本体は使っていない。

配るときに添えるものは [NOTICE.txt](NOTICE.txt) にまとめてある。

VST3 のインターフェース定義（`third_party/vst3/pluginterfaces`）は Steinberg の
ものだが **MIT** で配られている。GPLv3 の `public.sdk` は使っていないので、
プラグインの土台は全部このリポジトリの中にある。
くわしくは [third_party/vst3/README.md](third_party/vst3/README.md)。

CLAP のヘッダ（`third_party/clap`、Alexandre BIQUE、**MIT**）も手を加えずに取り込んだ。
くわしくは [third_party/clap/README.md](third_party/clap/README.md)。

gui.exe の PC エディタの窓は Dear ImGui（`third_party/imgui`、Omar Cornut、**MIT**）で
描いている。手を加えずに取り込んだ。

このリポジトリ独自のコードは BSD-3-Clause とする。

## 上流への還元

MU2000 を鳴らす過程で MAME の SWP30 に見つけたバグは、実機の測定値をもとに直して
MAME へ送っている。見つけたもの全部と、送ったかどうかは
[doc/upstream.md](doc/upstream.md) に溜めてある。

取り込まれたもの（2026-09-17 時点）:

| PR | 中身 |
|---|---|
| [mamedev/mame#16075](https://github.com/mamedev/mame/pull/16075) | ループ長のマスクの幅（ロングトーンで音色が次々に変わる）と、ピッチの clamp（発音 150ms 後にピッチが最大に張り付く） |
| [mamedev/mame#16115](https://github.com/mamedev/mame/pull/16115) | iir2 のレジスタの並び、DPCM の累算の漏れ |
| [mamedev/mame#16140](https://github.com/mamedev/mame/pull/16140) | リバーブ RAM の有効のレジスタを読めるようにした |
| [mamedev/mame#16141](https://github.com/mamedev/mame/pull/16141) | MEG の DRC がインタプリタと食い違う 3 か所 |
| [mamedev/mame#16142](https://github.com/mamedev/mame/pull/16142) | MEG のメモリ読み出しの絶対番地の指定 |
| [mamedev/mame#16143](https://github.com/mamedev/mame/pull/16143) | 逆向き再生のサンプルの補間の順 |

審査中: [#16144](https://github.com/mamedev/mame/pull/16144)（声の音量の積の切り捨て）・
[#16150](https://github.com/mamedev/mame/pull/16150)（DPCM の端数）・
[#16151](https://github.com/mamedev/mame/pull/16151)（歪み系エフェクトで見つけた MEG の演算器の端の場合）・
[#16154](https://github.com/mamedev/mame/pull/16154)（切ったリバーブ RAM の区画の読み書き）。
