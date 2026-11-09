# R411: 電源ボタンと GPU で MAX77620 を共用(試験、GPU 段階 A)

R410 の DSDT で、電源ボタン `PWRB`(`ACPI\SWOA0601`)の I2C5 `0x3C`(MAX77620)を `Exclusive` から `Shared` に変えただけの版。GPU0 の `0x3C` はもともと `Shared`。

- 理由: R410 の実機で、switchgpu 0.1.0 の MAX77620 を開く処理が `0x80000011`(STATUS_DEVICE_BUSY)で失敗した。電源ボタンのドライバーが先に `Exclusive` で開いていたため(CPU のドライバーで TMP451 を開けなかったのと同じ)。GPIO6(VDD_GPU のイネーブル)が読めず、電源テストができなかった。
- 確かめること: 電源ボタン(押して画面オフ、長押し)がいままでどおり動くこと。switchgpu の `I2C open: ... PMIC 0x00000000`。
- 使うドライバー: switchgpu 0.1.1(キット `SwitchGpu-0.1.1.0-testkit`)。
- 戻し方: R408 で起動する。

ROM: `coreboot-R411.rom`。作り方は `Tools/uefi/package-R411.sh`。

## R410 でわかったこと(2026-11-09)

- GPU0 が見え、switchgpu が動いた。MAX77621(0x1C)は読めた: chip id 0x12/0x00、CONTROL1 0x02、CONTROL2 0x9D。
- 起動直後の状態: クランプ 1(かかっている)、gpu クロック 1、pllg_ref 1、gpu リセット 1(かかっている)、fuse クロック 1、PLLP_OUTC 0x00030000(OUT5 は有効だが分周なし = 408 MHz)、MC ホットリセット ctrl 0 / stat 0x1924。→ 起動のファームウェアがクロックは入れたまま、GPU はクランプとリセットで止まっている。
- switchgpu 0.1.1 では、OUT5 を 204 MHz(分周 2)にしてから進む(止めるときは元に戻す)。

## R411 の実機結果(2026-11-09)

- switchgpu 0.1.1 で MAX77620 を開けた(PMIC 0x0)。GPIO6 は入力で Low(VDD_GPU は切れていた)。
- PowerTest 成功: **NV_PMC_BOOT_0 = 0x12B000A1(GM20B、rev A1)**、PTIMER が 1 ms で約 100 万進む(動いている)、PMC_ENABLE 0xC0012020。かかった時間 102 ms。
- GPU から見える WPR: 0xFEB00000 から 0xFEB40000 まで(256 KB、coreboot の GSC2 と同じ)。
- 止めたあとは元どおり(クランプ 1、リセット 1、GPIO6 入力、VOUT の EN オフ)。
