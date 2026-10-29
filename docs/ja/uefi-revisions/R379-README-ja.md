# R379 — 明るさセンサーと傾きセンサーを Windows から使えるようにする（R378 ベース）

## 変更点

- **明るさセンサー（BH1730FVC、I2C2 の 0x29）:**
  - GEN2_I2C のピン（PJ2、PJ3）を I2C2 にする。
  - I2C2 のクロック（CLK_M）を入れて、リセットを解く。I2C3 と同じやり方。
  - PART_ID（レジスター 0x12）を読んで、画面に出す。
  - 電源（LDO6）は、タッチパネル用に R373 から入っている。
- **傾きセンサー（LSM6DS3H、SPI4 の CS0）:**
  - SPI4 のピン（PC5〜PC7、PD0）を、L4T と同じ設定にする。
  - SPI4 のクロック（PLLP / 100 = 4.08 MHz）を入れて、リセットを解く。
  - WHO_AM_I（0x0F）を読んで、画面に出す。
- **DSDT:**
  - `I2C2`（`SWOA0201`、0x7000C400、GSI 116）を足した。
  - `SENS`（`SWOA0901`）を足した。SPI4 のレジスター（0x7000DA00）と、I2C2 の 0x29 を使う。
- **変えていないもの:** USB、充電、タッチ、CPU などは R378 と同じ。
- **チップの設定:** UEFI はクロックとピンだけ。チップの設定は、Windows のドライバー（switchsensor.sys）がする。

## 成果物と検証

- ROM: `D:\Codex\coreboot-R379.rom`
- SHA-256: `db94fcf7cfe04d6345cb2c85af5c9006756f8a7001db6938500d556e3623cb33`
- `EDK_SOURCE_RESTORE_MATCH=YES`。
- AML に `I2C2`、`SENS`、`SWOA0901` があることを確認した。
- 実機ではまだ試していない。

## 手順

1. **R379 で起動して、UEFI の画面を見る。**
   - `R379 light sensor: I2C2 clock ..., BH1730 PART_ID read 7x`
   - `R379 IMU: SPI4 clock ..., WHO_AM_I read 69`
   - `FAIL` や、`00`・`FF` が出たら、その行を教えてほしい。
2. **Windows まで起動するか見る。**
3. **センサーのドライバーを入れる。** 手順は、ドライバーのリポジトリの `kits/Switch-Sensor/README-ja.md` にある。

## もし起動しなくなったら

R378 の ROM で起動すれば、元に戻る。
