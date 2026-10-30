# R389 — 音量ボタンの長押しの誤判定を直し、xHCI は USB が動いたときだけ Windows に見せる（R379 ベース）

## 変更点

- **音量ボタン（DSDT の BTNS）:**
  - GpioInt の debounce を 0 から 20 ms（0x07D0、10 µs 単位）にした。
  - 0 のときは、押した瞬間の接点のバタつきで「離した」の割り込みを取りこぼすことがあった。そうなると Windows は押しっぱなしと受け取り、音量を最大や最小まで連続で変えていた。
  - GPIO のドライバー（tegra210gpio）は、debounce を Windows（GpioClx）にまかせる設定。なので、DSDT の値を変えるだけで効く。
- **xHCI（DSDT の XHC0）:**
  - `_STA` は `XSTA` という値を返す。DSDT の中での初めの値は 0x10（見せない）。
  - UsbPowerAuditDxe は、XUSB を立ち上げてつないだときだけ、DSDT の `XSTA` を 0x0F（見せる）に書き換えて、DSDT のチェックサムを直す。
    - 画面には `R389 XHC0: XUSB running, shown to the OS` と出る。
  - 立ち上げなかったときは、Windows の USB ドライバーが止まっている XUSB に触らない。
    - 例: 充電器の設定が途中で止まったとき。hekate の L4T 起動で起きた。
    - 触ると、Windows はロゴのまま固まっていた。
- **PWM のクロック:**
  - 前のローダーが PWM を動かしたまま渡したときも、クロックの選び方を coreboot と同じ（`C0000004`）に直す。
    - 直したときは `R389 PWM clock source ... -> ...` と出る。
  - hekate のあとは `80000004`（遅いクロック）のままで、Windows でバックライトがちらついていた。
  - coreboot の起動では、もともと同じ値なので何もしない。
- **途中の版から入っているもの:**
  - **R386:** Windows に渡す直前（ExitBootServices）に、GPIO の割り込みをすべて止めて消す。受け取ったときに有効だった GPIO の割り込みを、画面に出す（`R386 GPIO interrupts ...`）。
  - **R388:** XUSB の状態を画面に出す。
    - 始めの1行（`R388 start`）は、受け取った時点の状態。電源の区画、XUSB のクロックとリセット、PLLE。
    - USB の状態の4回目の表示のあとの4行（`R388 end`、`PAD`、`HC`、`FW`）は、立ち上げたあとの状態。パッド、各ポート、Falcon、メールボックス。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R389.rom`
- SHA-256: `8aac4295d34f840622ebd6fafb49a4d2b93919bc89ebc39016a657349e070a67`
- `EDK_SOURCE_RESTORE_MATCH=YES`。
- ビルドした AML に、`XSTA`（08 'XSTA' 0A 10）が1つと、debounce の 0x07D0 が2つあることを確かめた。
- **実機の結果（2026-10-30、ユーザー報告）:**
  - RCM で起動して、Windows まで問題なく動いた。
  - USB が今までどおり使えた。
  - 音量ボタンは、1回押すと1段だけ変わる。
  - USB-C ハブは、何回挿し直しても認識した。

## もし起動しなくなったら

R379 の ROM で起動すれば、元に戻る。
