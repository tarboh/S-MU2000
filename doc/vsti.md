# VST インストゥルメント（VST 2.4）

Windows 版では、従来形式の VST 2.4 インストゥルメント DLL も作れる。
エミュレータ本体、ROM の探し方、状態の保存形式、リサンプラー、パネル画面は
VST3・CLAP 版と共通。

```
make vsti
make vsti-probe
make install-vsti
```

出力は `build/S-MU2000.dll`。`vsti-probe` は DAW を使わずに DLL を読み込み、
インストゥルメントとしての宣言、ステレオ出力、MIDI 入力、状態の保存と復元、
画面の取り付けを確かめる。

ROM が使える状態なら、次のコマンドで firmware の起動を待ち、MIDI の音符から
実際に音が出るところまで確かめられる。

```
build/vstiprobe.exe build/S-MU2000.dll --audio
```

プラグインは MIDI IN A で MIDI とシステムエクスクルーシブを受け取り、
自動化できる `Output` パラメータを 1 本持つ。VST 2 には VST3・CLAP 版で使う
2 本のノートバスが無いため、このラッパーでは MIDI IN B を公開していない。

ROM の探し方はほかのプラグイン形式と共通。`S-MU2000.dll` の隣、または
`%LOCALAPPDATA%\S-MU2000\roms.txt` に、ROM のある場所を 1 行で書く。

ソースには、公開されている VST 2.4 のバイナリインターフェースを必要な分だけ
独自に宣言してある。提供終了済みの VST2 SDK は必要なく、同梱もしていない。
