# R410: GPU のデバイス GPU0(試験、GPU 段階 A)

R408 の DSDT に、GPU(GM20B)のデバイス `GPU0`(`ACPI\SWOA0F01`)を足しただけの版。ほかの動きは R408 と同じ(画面の DISP もそのまま)。

- 使うドライバー: `switchgpu.sys` 0.1.0(テスト用。キット `SwitchGpu-0.1.0.0-testkit`)。GPU に電源を入れ、ID(`NV_PMC_BOOT_0`)を読む。
- `_CRS`(この順で使う):
  - BAR0 `0x57000000`、`0x01000000`
  - BAR1 `0x58000000`、`0x01000000`
  - I2C5 の `0x1C`(MAX77621、VDD_GPU)
  - I2C5 の `0x3C`(MAX77620。GPIO6 が VDD_GPU のイネーブル。PWRB も使うので Shared)
  - 割り込みは無し(段階 B で stall = SPI 157、nonstall = SPI 158 を足す)
- ドライバーを入れなければ、GPU には何もしない(レールは切れたまま)。switchgpu も、`PowerTest = 1` のときしか電源を入れない。
- 戻し方: R408 で起動する(GPU0 が無くなるだけ)。

ROM: `coreboot-R410.rom`、SHA-256 `40b67820f78fa3f695dfadce7cd6632767ace5322c44c63de89a2df6685c3947`。作り方は `Tools/uefi/package-R410.sh`。

## 実機の結果

MAX77620(0x3C)を開けなかった(STATUS_DEVICE_BUSY。電源ボタンが Exclusive で持っている)。直した版は R411(R411-README-ja.md に結果)。
