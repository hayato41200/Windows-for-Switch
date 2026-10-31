# R392 — XUSB のファームウェアを ESP から読み、Bluetooth の UART のクロックを毎回戻す（R390 ベース）

## 変更点

- **XUSB のファームウェアを ROM に入れない（初期設定）:**
  - NVIDIA の `LICENCE.nvidia` は、再配布をオープンソースの OS で使う場合に限っている。そのため、ビルドした ROM に NVIDIA のファイルが入らないようにした。
  - `NintendoSwitch.dsc` の `EMBED_XUSB_FIRMWARE` が `FALSE`（初期値）なら、FDF はファームウェアを入れない。`-D EMBED_XUSB_FIRMWARE=TRUE` を付けると、今までどおり埋め込む（自分だけで使うビルド向け）。
  - 埋め込みが無いと、UsbPowerAuditDxe は FAT の区画（SD の ESP）の一番上にある `\tegra21x_xusb_firmware` を読む。R362 より前に使っていた読み方と同じ。
  - サイズのチェックを緩めた。ヘッダーの長さとファイルの大きさが合っていて、64〜256 KiB なら受け付ける。試したのは L4T の 124,416 バイト。linux-firmware の `nvidia/tegra210/xusb.bin`（126,464 バイト）は、まだ試していない。
- **Bluetooth の UART-D のクロック:** 起動のたびに PLLP 408 MHz（bit 24 なし）に書き直す。
  - R391 は 48 MHz（bit 24 あり）にしていた。bit 24 は CAR のクロックをそのまま UART の 16 倍のクロックにするので、UART が 3 Mbps になっていた。
  - switchbt は 3 Mbps のときにこの値を自分で変える。再起動のあとも値が残っていたので、ここで毎回戻す。
- 32 kHz のクロックの確認（R391）は、そのまま残した（表示だけで、値は変えていない）。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R392.rom`（各自でビルドする。ROM は配らない）
- SHA-256: `18376eb738c6bebad5a2a138713617614030f1405cb906c4e2db229de7754f1b`
- `EDK_SOURCE_RESTORE_MATCH=YES`。ビルドした FD に、ファームウェアのファイルの GUID も中身も無いことを確かめた。
- **実機の結果（2026-10-31、ユーザー報告）:**
  - ESP の一番上に `tegra21x_xusb_firmware` を置いて起動した。
  - USB、Bluetooth、再起動のあとの Bluetooth が、どれも問題なく使えた。

## もし起動しなくなったら

R390 の ROM で起動すれば、元に戻る（ファームウェアは ROM に埋め込まれている版）。
