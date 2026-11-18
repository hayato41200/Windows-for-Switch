# R409: 画面のデバイス DISP に表示コントローラーの割り込み(試験)

R408 の DSDT のデバイス `DISP`(`ACPI\SWOA0E01`)の `_CRS` に、DC-A の割り込みを 1 つ足しただけの版。ほかの動きは R408 と同じ。

- 割り込み: `Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x69 }`(SPI 73 = GSIV 105。L4T の DT の `dc@54200000` の `interrupts = <0 0x49 4>`)。
- 使うドライバー: `switchdisplay.sys` 1.2.0 以降。Windows が vsync を求めたときだけ、DC の V_BLANK 割り込みを有効にし(`DC_CMD_INT_ENABLE` と `DC_CMD_INT_MASK` の bit 2)、表示専用ドライバーの vsync(`DXGK_INTERRUPT_DISPLAYONLY_VSYNC`)として知らせる。switchdisplay 1.1.0 以前、または Basic Display のときは、割り込みは使われない。
- 戻し方: R408 で起動する(割り込みが無いだけで、switchdisplay はタイマーで動く)。R405 で起動すれば、switchdisplay は読み込まれない。

ROM: `coreboot-R409.rom`、SHA-256 `0edb19026e3ae1ed7c58b4f8f26001b273f438f1e890731723cccb8ff9625cb1`。作り方は `Tools/uefi/package-R409.sh`。

## 実機の結果

- 2026-11-08: **使えない。** Windows は起動するが、画面のデバイス(`ACPI\SWOA0E01`)がエラー 43(CM_PROB_FAILED_POST_START)になり、画面は Basic Display に戻る。Kernel-PnP のイベント 411 の Problem Status は **0xC00000E5(STATUS_INTERNAL_ERROR)**。
- switchdisplay 1.1.0(割り込みの処理なし)、1.2.0、1.2.1(AddDevice で DC の割り込みを止める)、1.2.2(ControlInterrupt の「止める」に常に成功を返す)の、どれでも同じだった。
- 1.2.2 の記録: StartDevice は成功(POST も受け取れた)。そのあと Windows が呼んだのは QueryChildRelations と QueryAdapterInfo の 1 回ずつだけ。ISR は 0 回、ControlInterrupt も 0 回。起動前の DC の INT_STATUS / MASK / ENABLE はすべて 0(割り込みが鳴りっぱなしではない)。
- 見立て(未確認): 表示専用(KMDOD)のアダプターに ACPI の割り込みが付いていると、dxgkrnl が内部エラーで起動を止める。ドライバー側では直せない見込み。
- **R409 は使わない。** DSDT は R408 の形(割り込みなし)に戻した。vsync は Windows のタイマーのまま(段階 3 は保留)。
