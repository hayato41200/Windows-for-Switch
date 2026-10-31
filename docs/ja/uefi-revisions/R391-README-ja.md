# R391 — Bluetooth の UART を 48 MHz にし、32 kHz のクロックを確かめる（R390 ベース）

Bluetooth の第3段（switchbt 1.0.0.8：ファームウェアのパッチと 3 Mbps）のための版です。

## 変更点

- **UART-D のクロック:** PLLP（408 MHz）を 8.5 で割った 48 MHz にした（分周を有効にして N = 15、Linux と同じ）。
  - 48 MHz / 16 = 3 Mbps なので、switchbt が 3 Mbps に上げられる。
  - UEFI の確認（115200 bps）は、分周 26 で同じ誤差（0.16 %）。
  - R390 のときは 408 MHz のまま。switchbt 1.0.0.8 は、どちらのクロックでも動く（48 MHz を先に試す）。
- **32 kHz のクロック（チップの低電力クロック）:**
  - L4T の DT では、MAX77620 の GPIO4 を `32k-out1` にしている。coreboot は AME_GPIO に 0x1C を書く。
  - UEFI は AME_GPIO と CNFG1_32K を読んで画面に出す。GPIO4 のビット（bit 4）が 0 なら 1 にする。
  - 例: `R391 Bluetooth 32k: PMIC AME_GPIO 1c -> 1c (bit 4 = GPIO4 32k-out1) CNFG1_32K ..`
- DSDT は R390 と同じ。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R391.rom`
- SHA-256: `fe8fe819dab16dc500c9d6a8e2255f40e067164ae91b627e6b1ab7922a6aec7b`
- `EDK_SOURCE_RESTORE_MATCH=YES`。
- 実機ではまだ試していない。

## もし起動しなくなったら

R390 の ROM で起動すれば、元に戻る。
