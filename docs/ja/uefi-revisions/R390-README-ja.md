# R390 — Bluetooth のチップに電源を入れ、DSDT に BTH0 を足す（R389 ベース）

## 変更点

- **Bluetooth（BCM4356A3、Wi-Fi と一体のチップ）:**
  - Bluetooth の部分は UART-D（0x70006300、GIC SPI 90）につながっている（L4T の DT: `brcm,bcm43438-bt`）。
  - UEFI は次のことをする。
    - UART-D のピン（uart4_tx/rx/rts/cts、PI4〜PI7）を設定する。
    - UART-D のクロックを PLLP（408 MHz）から入れる。
    - チップの電源 PH4（BT_RST）を入れる。PH3（AP_WAKE_BT）は「起きている」側（Low）にする。PH5（BT_WAKE_AP）は入力。
  - 115200 bps で HCI_Reset と Read_Local_Version を送り、答えを画面に出す。
    - 例: `R390 Bluetooth: HCI ok, HCI 08 rev 2000 LMP 08 maker 000f subver 2409`（000f = Broadcom）
    - 答えがないときは `HCI_Reset NO ANSWER` と出る。どちらの場合も、起動はそのまま続ける。
  - 受信が止まらない場合でも、待ち時間は必ず終わる（受け取った1バイトごとに、時間を進める）。
  - チップの電源は入れたまま、Windows のドライバー（switchbt）に渡す。
- **DSDT:** `BTH0`（`SWOA0B01`）を足した。リソースは UART-D のレジスター（0x40 バイト）と、割り込み GSI 122（レベル、High）。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R390.rom`
- SHA-256: `39dfe889502fc5b66935c614a9a565c51dab4bd55e13f14437f96259482ffb75`
- `EDK_SOURCE_RESTORE_MATCH=YES`。ビルドした AML に、`BTH0` と `SWOA0B01` が1つずつあることを確かめた。
- **実機の結果（2026-10-31、ユーザー報告）:**
  - Windows のドライバー（switchbt 第1段）から、チップの名前 `BCM4356A3`、Broadcom、LMP subversion 0x2409 が読めた。
  - 第2段で、Windows の BthMini に「Bluetooth Radio」としてつながった（problem=0）。設定の Bluetooth は、まだ使えない（試験中）。
  - switchbt を入れたまま起動すると、ロゴで止まることがあった。これはドライバー側の問題として調べている（switchbt 1.0.0.5 に、止まった起動の次は何もしない仕組みを入れた）。

## もし起動しなくなったら

R389 の ROM で起動すれば、元に戻る。R389 の DSDT には BTH0 が無いので、switchbt は読み込まれない。
