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

**成功の目安(実機で見えるもの):**
- 起動中の画面に `R424 partner VID=057e PID=2003 type=5`。
- `R424 Nintendo Alt Mode entered.` と、`DP_CFG_AND_START_HPD: ... lastcmd=0`。
- `R424 DP configured:` の行で `DP_EN=1`(DP_STATUS bit 15)。テレビがつながっていて電源が入っていれば `SIGNAL=1`(bit 7)と `HPD(PCC1)=1`。
- **ドックの緑の LED がつく**(VDM が届いた目に見える証拠)。
- **ドックの USB に挿したキーボードが、UEFI と Windows で使える**(ハブの有効化)。
- 最後のまとめに `R424 DP-ALT: DONE at DONE`。

テレビには、この段階ではまだ何も映りません(SoC の DP 出力を何も動かしていないため)。

### 段階 2: DPAUX1 と SOR1(UEFI)
1. DPAUX1: クロックとリセット(CAR)、パッドの電源(`DPAUX_HYBRID_SPARE`)、prod の値、DP のパッドモード。HPD を `DPAUX_DP_AUXSTAT` bit 28 で読む(段階 1 の PCC1 と一致すること)。
2. AUX で DPCD を読む(0x000〜0x00F: DPCD rev、最大リンク速度、レーン数)。ドックの DP→HDMI 変換チップが答えるはず。
3. AUX の I2C-over-AUX で EDID(0x50)を読み、テレビの名前と推奨の解像度を表示する。
4. SOR1: SOR の電源パーティション、`sor1`、`sor_safe`、`sor1_pad_clkout`、PLLDP、レーンの並べ替え(xbar 2 1 0 3 4)、パッドの電源。
5. リンクトレーニング(dp_lt.c の CR → EQ、2 レーン、RBR/HBR から)。`fast-lt-disable` なので、毎回フルのトレーニング。電圧振幅とプリエンファシスの表は DT の `lt-data` / `dp-lt-settings` を使う。
- 成功の目安: EDID のテレビ名が画面に出る。リンクトレーニングが CR/EQ とも通り、DPCD 0x202〜0x204 が「lock」になる。テレビが「信号なし」から黒画面やモード表示に変わるかもしれない。

### 段階 3: DC-B から外部画面へ
1. UEFI: DC-B のクロック(disp2、PLLD2)、ウィンドウ A に単色や UEFI の画面のコピーを出す。SOR1 の DP 出力を DC-B につなぐ。テレビに絵が出たら成功。
2. Windows: switchdisplay の 2 つ目の出力(2 つ目の VidPN source/target)。モードは EDID から。最初は複製(内蔵と同じ 1280x720)で始め、拡張はそのあと。ホットプラグは BM92T の ALERT と HPD から。
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
