# USB-C DisplayPort Alt Mode(公式ドック → テレビ)の計画

対象: Nintendo Switch(Erista、T210)の UEFI と Windows on ARM。
参考: Switchroot L4T(`drivers/misc/bm92txx.c`、`drivers/video/tegra/dc/{dp,dp_lt,dpaux,sor}.c`)と、L4T の実機の稼働中のデバイスツリー。

## 1. 信号の経路(L4T の DT から)

| 部品 | 場所 | L4T の DT にあること |
|---|---|---|
| BM92T(USB-PD) | I2C1 0x18、ALERT# = GPIO PK4(0x54)、Low レベル | `rohm,dp-lanes = <2>`、DP の alert は無効 |
| レーンの mux | BM92T のファームウェアが自分で切り替える | L4T に mux のドライバーはない。0x3636 のあと BM92T が mux と HPD を扱う |
| HPD | ピン HDMI_INT_DP_HPD(GPIO PCC1)、機能 `dp` | `nvidia,hpd-gpio = <&gpio 0xE1>` |
| DPAUX1 | 0x54040000、IRQ 11 | prod `0x124: mask 0x37FE = 0x24C2`(DP モード) |
| SOR1 | 0x54580000、IRQ 76 | `nvidia,xbar-ctrl = <2 1 0 3 4>`(レーンの並べ替え)、2 レーン、`fast-lt-disable` |
| DC | **DC-B(head1、0x54240000)** → SOR1 | `nvidia,dc-or-node = "/host1x/sor1"`、親クロック `pll_d2_out0` |
| 電源 | vdd-dp-pwr / avdd-dp-pll / vdd-dp-pad | XUSB と同じレール(hvdd_usb、avdd_pll_utmip、dvdd_sata)。USB が動いていれば入っている |

内蔵パネルは DC-A → DSI のままです。外部画面は DC-B → SOR1 → USB-C を使うので、2 つの head は独立しています。

ドックの USB: L4T はピン割り当て D(DP 2 レーン + USB)を送りますが、コメントのとおりドックはピン割り当てを見ていません。ドックの USB ハブは USB-C の USB2(D+/D-)につながり、Switch 側は XUSB のホストとして扱います(今の UEFI の「シンク」の経路と同じ)。

## 2. 段階

### 段階 1: BM92T で DP Alt Mode に入る(UEFI、R424 / R424H)
L4T の `bm92t_event_handler` の手順を、割り込みの代わりに ALERT レジスター(0x02、読むと消える)のポーリングで行います。今の UEFI は、ドックを「電源をくれる相手(シンク)」として扱い、R363 で契約を読み、DR_SWAP で DFP になります。R424 はその直後に、次を行います。

1. Discover Identity(VDM)。返事の VID/PID が 057E:2003(type 5 = Alternate Mode Adapter)なら公式ドック。
2. 任天堂の Alt Mode に入る(Enter Mode、SVID 057E)。
3. `DP_ENTER_MODE`(0x3131)。BM92T が DP の Discover/Enter を行う。
4. DP の能力を読み、DP Configure の VDO(UFP_U as UFP_D、ピン D、だめならピン C)を書いて `DP_CFG_AND_START_HPD`(0x3636)。STATUS1 の lastcmd = 0(完了)を確かめる。
5. ドックの VDM: 状態の問い合わせ、LED をつける、USB ハブを有効にする(busy なら 250 ms ごとに最大 10 回)。
6. 10 秒間、DP_STATUS(0x18)と HPD(PCC1)の変化を表示する。

公式ドック以外(USB-C ハブ + 充電器など)は、SVID と DP のモードを読んで表示するだけで、モードには入りません。今の動きは変わりません。ドックをつないでいないとき(OTG、何もない)は、R424 は動きません。

**R425 / R425H(他社の機器):** 相手が公式ドックでなくても、Discover Mode(SVID FF01)で UFP_D を返したら、L4T と同じく任天堂のモードを抜いて `DP_ENTER_MODE`(0x3131)に進みます。DP Configure のピンは、相手がプラグ型なら能力のプラグ側、レセプタクル型ならレセプタクル側から選び、VDO の 1 バイト目に入れます(L4T と同じ)。ドックの VDM(状態、LED、ハブ)は行いません。電源をもらう機器(充電器つきのハブ、PD 入力つきの変換アダプター)だけが対象で、電源なしの変換ケーブル(Switch が VBUS を出す OTG の経路)はまだ対象外です。成功の目安は、まとめの行が `DONE at DONE (generic)` になり、`DP_EN=1` が出ること。

**成功の目安(実機で見えるもの):**
- 起動中の画面に `R424 partner VID=057e PID=2003 type=5`。
- `R424 Nintendo Alt Mode entered.` と、`DP_CFG_AND_START_HPD: ... lastcmd=0`。
- `R424 DP configured:` の行で `DP_EN=1`(DP_STATUS bit 15)。テレビがつながっていて電源が入っていれば `SIGNAL=1`(bit 7)と `HPD(PCC1)=1`。
- **ドックの緑の LED がつく**(VDM が届いた目に見える証拠)。
- **ドックの USB に挿したキーボードが、UEFI と Windows で使える**(ハブの有効化)。
- 最後のまとめに `R424 DP-ALT: DONE at DONE`。

テレビには、この段階ではまだ何も映りません(SoC の DP 出力を何も動かしていないため)。

**実機の結果(2026-11-19、R425H、ドック(非純正、VID/PID は 057E:2003)+ テレビ):** Nintendo Alt Mode に入れた。0x3131 と 0x3636 はどちらも lastcmd=0。DP_STATUS は C01A → C09A(DP_EN=1、INSERT=1、SIGNAL=1、PINCFG=1)。ドックの状態、LED、ハブの VDM はどれも ACK。HPD(PCC1 の GPIO)は 0 のままだった(ピンは SFIO で、GPIO の CNF に bit1 が入っていない)。段階 1 は通った。

### 段階 2: DPAUX1 と SOR1(UEFI)
**段階 2a(R426 / R426H):** 下の 1〜3。R424 の段階 1 が DONE のときだけ、続けて実行する。SOR のパーティションがオンであることを確かめる(DSI もここにあるので、内蔵パネルが動いていればオンのはず)。次に CAR の sor_safe(Y bit 30)と dpaux1(Y bit 15)をオンにし、dpaux1 をリセットし直す。そのあと prod、AUX モード、パッドの電源、HPD_CONFIG を設定し、AUXSTAT の bit 28 で HPD を読む。続けて DPCD の 0x000〜0x00F と 0x200〜0x205、EDID のブロック 0 を読む。成功の目安は、まとめの行が `R426 DPAUX: DONE at DONE` になり、テレビの名前と推奨の解像度が出ること。
**実機の結果(2026-11-19、R426H):** HPD(DPAUX)=1。DPCD は 12 14 E2 01 01 05 …(rev 1.2、最大 5.4 Gbps、2 レーン、enhanced framing、downstream port あり)、sink count 1。EDID は SHARP "SHARP HDMI"、1920x1080@60(148.5 MHz)、checksum OK。段階 2a は通った。
**段階 2b(R427 / R427H):** 下の 4〜5(SOR1 とリンクトレーニング)。R426 が DONE のときだけ、続けて実行する。手順は次のとおり。
- PLLDP を 270 MHz にする(M=1、N=28、P=4、SDM 0xF400)。
- sor1_out を sor_safe にして、sor1 のゲートを開け、リセットし直す。
- CLK_CNTRL(DIFF_DPCLK、リンク速度)を設定し、DT の prod_c_dp を当てる。
- 終端の校正をして、パッドの電源を手順 1〜5 で入れる。
- パッドのレーン 2 と 1 を起こし(xbar)、レーン数 2 にして、レーンシーケンサーで起動する。
- DPCD に D0、8b/10b、リンク速度、レーン数を書く。
- LINKCTL を有効にし、XBAR を設定して、sor1_out を sor1_pad_clkout に切り替える。
- プリチャージのあと、TPS1 で CR、TPS2 で EQ を通す。駆動レベルは DT の lt-data(pc2 = 0)を使う。

速度は 2.7 Gbps を先に試し、だめなら 1.62 Gbps で試す。成功の目安は、`R427 DP-LINK: DONE` が出て、レーンの状態が 0x77 / align 0x01 になること。

**実機の結果(2026-11-19、R427H):** SOR1 の起動、校正、パッド、レーンの起動は、どれも通った。CR は 2.7 Gbps でも 1.62 Gbps でも、1 回目で 0x11 になった(要求は vs0/pe1)。EQ は 6 回とも 0x11 のままで、シンボルロックも EQ 完了も立たなかった。PLLDP は前のローダーが動かしたままで(BASE 0x58103B02 = M2 N59 P3、約 378 MHz)、R427 はそれをそのまま使っていた。リンクの速度が約 1.4 倍ずれていたのが原因の候補。
**R428 / R428H:** PLLDP が 270 MHz の設定(M1 N28 P4、SDM オン)でなければ、いったん止めて設定し直す。
**実機の結果(2026-11-19、R428H):** 前のローダーの PLLDP は M2 N56 P3、SDM オフ(約 358 MHz)だった。止めて 270 MHz に設定し直すと、10 us で lock=1(BASE 0x58101C01)。CR は 2 回目で 0x11(vs0/pe1)、EQ は 1 回目で **0x77 / align 0x81**。`R427 LINK TRAINED: 2.7 Gbps x 2 lanes`。LINKCTL 0x00034101、PADCTL 0x00C03003。PLLDP を設定し直しても本体の画面は乱れなかった。**段階 2b 完了。**

1. DPAUX1: クロックとリセット(CAR)、パッドの電源(`DPAUX_HYBRID_SPARE`)、prod の値、DP のパッドモード。HPD を `DPAUX_DP_AUXSTAT` bit 28 で読む(段階 1 の PCC1 と一致すること)。
2. AUX で DPCD を読む(0x000〜0x00F: DPCD rev、最大リンク速度、レーン数)。ドックの DP→HDMI 変換チップが答えるはず。
3. AUX の I2C-over-AUX で EDID(0x50)を読み、テレビの名前と推奨の解像度を表示する。
4. SOR1: SOR の電源パーティション、`sor1`、`sor_safe`、`sor1_pad_clkout`、PLLDP、レーンの並べ替え(xbar 2 1 0 3 4)、パッドの電源。
5. リンクトレーニング(dp_lt.c の CR → EQ、2 レーン、RBR/HBR から)。`fast-lt-disable` なので、毎回フルのトレーニング。電圧振幅とプリエンファシスの表は DT の `lt-data` / `dp-lt-settings` を使う。
- 成功の目安: EDID のテレビ名が画面に出る。リンクトレーニングが CR/EQ とも通り、DPCD 0x202〜0x204 が「lock」になる。テレビが「信号なし」から黒画面やモード表示に変わるかもしれない。

### 段階 3: DC-B から外部画面へ
1. UEFI: DC-B のクロック(disp2、PLLD2)、ウィンドウ A に単色や UEFI の画面のコピーを出す。SOR1 の DP 出力を DC-B につなぐ。テレビに絵が出たら成功。

**R429 / R429H(段階 3-1 の最初の試し):** まずはフレームバッファもメモリ クライアントも使わず、DC-B の**背景色**だけを出す(ウィンドウはすべて無効)。
- DISB パーティションの電源を入れ(PMC、id 19)、disp2 のクロックを入れてリセットを解く。クロックは PLLD2 を使わず、PLLP(408 MHz)をシフトクロック分周 5.5 で割って 74.18 MHz にする(CEA 1280x720@60 の 74.25 MHz との差は 0.1%)。
- DC-B のタイミング: 1280x720、HFP 110 / HSYNC 40 / HBP 220、VFP 5 / VSYNC 5 / VBP 20、REF_TO_SYNC 1/1、BASE888。
- SOR1: coreboot の tegra_dc_dp_calc_config を移植して、TU サイズ、active count/frac、watermark、hblank/vblank のシンボル数を決める。STATE1 は head 1、DP-A、24 bpp、同期は正極性。HEAD_STATE1〜5 は head 1 用(+1 の位置)。
- つなぎ方は coreboot の tegra_dc_sor_attach と同じ順番。DC の WIN_OPTIONS は SOR1_ENABLE(bit 26)。
- 成功の目安: `R429 SOR1 head AWAKE`、テレビに赤 → 緑 → 青 → 白が 2 秒ずつ出て、最後は白のまま残る(色の並びは BG レジスタの形式しだいで入れ替わるかもしれない)。

**実機の結果(2026-11-19):**
- R429H: テレビは無信号。disp2 のクロック元を 0x140 に書いていた(正しくは 0x13C)。
- R430H / R431H: フレームは出ており(INT_STATUS 6)、DPCD 0x200〜0x205 = `01 00 77 00 81 01`(映像に同期)が 20 秒続いたが、テレビは無信号。ハブ(純正ドックではない、DP Alt 対応の USB-C ハブ。MacBook では映る)は 057E:2003 を名乗る。branch OUI 000000、port caps 0B(HDMI)。
- R432H: ハブは SVID FF01 も持つので、ふつうの DP Alt Mode(ピン D)で入るようにした。それでも無信号。
- **R433H: テレビに白が出た(テレビの表示は「HDMI 映像 720p」)。** 原因は DC の中の分周。SOR を使うときは DC のクロック = 画素クロックでないといけない(シフトクロック分周 1)。PLLP / 5.5 をやめ、PLLD2 を Linux の表どおり 74.25 MHz(M1 N30 SDM 0x0E00、P16 = hw 11、VCO 1188 MHz)にして、disp2 = pll_d2_out0(mux 5、1:1)。測ったフレームの速さは 60.0 fps。**段階 3-1 完了。**
- **R434H(2026-11-20): テレビに UEFI の画面が出た。** DC-A のウィンドウ A から FB(DFB80000)、DEPTH(0xC)、STRIDE などを読み、DC-B のウィンドウ A に同じ FB を渡して、720x1280 を 405x720 に縮めて中央(x=437)に置いた。本体に文字が増えるとテレビも一緒に変わる(R431 の毎秒の行もテレビに出た)。DC INT は 6 のままで、ウィンドウ A のアンダーフローは出ていない。MC / SMMU の設定は何もいらなかった。**段階 3-2 完了。**
- **R439H + switchdisplay 1.3.0(2026-11-20): Windows の画面がテレビに出た(段階 4a 完了)。** Status: `TV mirror (DP Alt): ON  DC-B cmd 0x20 winopt 0x4000000 active 0x2D00500 buffer 0xFD9F3000`。途中 R435H〜R438H では電源を完全に切ってから起動すると R427 の途中で SError になった。原因は SOR1 のパッドの電源 MAX77620 LDO8「vdd-hdmi-dp」1.05 V(L4T DT の vdd-dp-pad-supply、FPS source none)が切れていたこと。R428H〜R434H は前の起動で入った LDO8 が残っていたから動いていた(終端の調整が 8 ではなく 15 に振り切れるのが目印)。R439H で R427 の最初に LDO8 を入れ、HDMI パッドの DPD を外した。
2. Windows(段階 4)。3 つに分けて進める。
   - **4a(R435H + switchdisplay 1.3.0): ハードウェアでの複製。** UEFI が DC-B と SOR1 を動かしたまま Windows に渡す(R435H は色と 20 秒の待ちをやめるだけ)。switchdisplay は、CAR で disp2 のクロックが入っていて、DC-B が C_DISPLAY・SOR1・1280x720 のときだけ、自分の 1280x720 のバッファ(連続メモリ、4 GB 未満、write-combined)を DC-B のウィンドウ A に渡し、Present のたびに回転なしで写す。横向き(1280x720)の画面はそのまま、縦向きは 405x720 に縮めて中央。Windows から見える画面は 1 つのまま。止めるときは、UEFI が設定したウィンドウ A(起動時のフレームバッファ)に戻してからバッファを解放する。レジストリ TvMirror=0 で切れる。抜き差しにはまだ対応しない(起動時につないでおく)。
   - **4b: 抜き差し。** 2 つに分ける。
     - **4b-1(switchbattery 1.0.0.7):** BM92T を読むのは switchbattery だけのまま、シンクの設定のあと DFP なら UEFI R424 の手順で DP Alt Mode に入る(ドックを名乗るハブは標準の DP を優先、本物のドックは Nintendo モードとドック用 VDM)。状態はカーネルのコールバック `\Callback\SwitchWoaDpAlt`(Argument1 = 1 入った / 0 抜けた、Argument2 = DP_STATUS)で、変わったときと、入っている間は 5 秒ごとに知らせる。UEFI がすでに入れていれば(DP_EN)知らせるだけ。
     - **4b-2(switchdisplay):** コールバックを受けて、自分のスレッドで UEFI R426(DPAUX1)・R427(PLLDP、SOR1 のパッド、リンクトレーニング)・R429/R433(DC-B、PLLD2、ラスター、SOR1 の取り付け)をして、4a のウィンドウにつなぐ。抜けたら DC-B を止め、SOR1 をリセットに戻す。SOR1 のパッドの電源 LDO8(I2C5 の PMIC)は Windows からは触れないので、UEFI がテレビの有無にかかわらず起動時に入れておく。
   - **4c: 2 つ目の画面(拡張)。** 2 つ目の VidPN source / target。モードは EDID から。ホットプラグは BM92T の ALERT と HPD から。
3. Windows の BM92T: 今は switchbattery v4 が STATUS1 を読み、DR_SWAP をする。DP Alt Mode の状態(ドックの抜き差し)を扱うのは 1 つのドライバーにまとめる必要がある(ALERT を読むと消えるため、2 つのドライバーが読むと取り合いになる)。

## 3. 壊さないこと
- RCM(coreboot)と hekate の両方の経路で、同じソースをビルドする(R424 = RCM の確認用、R424H = hekate の bl33.bin)。
- 段階 1 は、シンクの経路で、R363 が成功し DFP になったときだけ動く。OTG(USB 機器を直接)や何もつながないときの経路には入らない。
- 段階 1 で失敗しても、そこで止めて XUSB の起動に進む(USB の動きは今までどおり)。
- eMMC には何も書かない。BM92T の設定を保存するコマンド(STORE_SYSCFG など)は使わない。

## 4. 未確認のこと
- GPIO PCC1 の入力が、ピンの機能が DP(SFIO)のときにも読めるか。読めなければ段階 2 の DPAUX の HPD で確かめる。
- ドックが、BM92T が自分で 15 V の契約をしたあとでも、DR_SWAP と Alt Mode に応じるか(L4T は自分で RDO を送る)。
- Windows に入ったあと、BM92T の Alt Mode の状態が保たれるか(ドックの LED とハブで分かる)。
- 電源なしの変換ケーブル: Switch がソース(VBUS を出す)で最初から DFP のとき、OTG の経路のどこで R424 の手順を呼ぶか。BM92T が VBUS を出す設定のまま Discover Identity に答えてもらえるか。
- ピン C だけの機器(4 レーン)で、BM92T と SOR1 の 2 レーンの設定が通るか。
